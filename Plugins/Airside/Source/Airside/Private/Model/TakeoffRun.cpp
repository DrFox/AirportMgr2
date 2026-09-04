#include "Model/TakeoffRun.h"

DEFINE_LOG_CATEGORY_STATIC(LogTakeoff, Log, All);

namespace
{
	/**
	 * When the nose is considered to be pointing down the runway, radians.
	 *
	 * Effectively zero, and that is deliberate. A wider tolerance was tried and it produced
	 * exactly the defect the turn rate exists to remove: the heading was snapped to the
	 * runway direction once it came within two degrees, which is a 140 deg/s yaw in a single
	 * frame - one frame before the most-watched moment in the game.
	 *
	 * Nothing is snapped now. The slew below clamps by the remaining error, so the last step
	 * lands exactly on the runway heading of its own accord, and this only has to notice.
	 */
	constexpr double LinedUpTolerance = 1.0e-9;

	/**
	 * The pitch at which the wheels leave the ground, as a fraction of the climb attitude.
	 *
	 * A rotation is not an instant: the nose comes up, the wing reaches its angle, and the
	 * aircraft flies. Lifting off part-way through the rotation is what makes it look like
	 * one movement rather than a jump at the end of it.
	 */
	constexpr double LiftOffPitchFraction = 0.35;
}

double FTakeoffRun::RequiredRoll(const FGroundPerformance& InGround)
{
	const double Vr = InGround.Takeoff.SpeedCap;
	const double Accel = InGround.Takeoff.Accel;
	if (Vr <= 0.0 || Accel <= 0.0)
	{
		return 0.0;
	}

	// v^2 = u^2 + 2as from rest, rearranged. The same arithmetic the braking profile uses,
	// pointing the other way.
	return (Vr * Vr) / (2.0 * Accel);
}

bool FTakeoffRun::Start(const FVector2D& InThreshold, const FVector2D& InDirection,
	double InRunwayLength, const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
	double InHeading)
{
	Phase = ETakeoffPhase::Clear;

	if (!InGround.IsSet() || !InGround.Takeoff.IsSet() || !InClimb.IsSet())
	{
		UE_LOG(LogTakeoff, Warning,
			TEXT("Departure refused: the airframe has no take-off or climb performance."));
		return false;
	}

	if (InDirection.IsNearlyZero())
	{
		UE_LOG(LogTakeoff, Warning, TEXT("Departure refused: the runway has no direction."));
		return false;
	}

	const double Needed = RequiredRoll(InGround);
	if (InRunwayLength < Needed)
	{
		// The whole reason this returns a bool. A strip shorter than the roll to Vr is one
		// this aircraft cannot leave, and rolling anyway simulates an overrun.
		UE_LOG(LogTakeoff, Warning,
			TEXT("Departure refused: %.0f uu of runway, %.0f needed to reach %.0f uu/s."),
			InRunwayLength, Needed, InGround.Takeoff.SpeedCap);
		return false;
	}

	Threshold = InThreshold;
	Direction = InDirection.GetSafeNormal();
	RunwayLength = InRunwayLength;
	Ground = InGround;
	Climb = InClimb;

	Travelled = 0.0;
	Altitude = 0.0;
	Pitch = 0.0;
	Heading = InHeading;

	// Rolling, not stopped: it arrived under power and has to keep rolling to steer - the
	// same rule the taxi model states in FGroundPerformance::MinTaxiSpeed.
	Speed = Ground.MinTaxiSpeed;
	Phase = ETakeoffPhase::LineUp;

	UE_LOG(LogTakeoff, Log,
		TEXT("Departure armed: %.0f uu runway, %.0f needed, rotate at %.0f uu/s."),
		RunwayLength, Needed, Ground.Takeoff.SpeedCap);
	return true;
}

bool FTakeoffRun::Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading,
	double& OutAltitude, double& OutPitch)
{
	if (Phase == ETakeoffPhase::Clear)
	{
		return false;
	}

	const double RunwayHeading = FMath::Atan2(Direction.Y, Direction.X);

	switch (Phase)
	{
	case ETakeoffPhase::LineUp:
	{
		// Slewed at the airframe's rate, exactly as taxiing does. An aircraft that snapped
		// onto the runway heading would undo the whole of the turn-rate work one frame
		// before the most-watched moment in the game.
		const double Error = FMath::UnwindRadians(RunwayHeading - Heading);
		const double MaxStep = FMath::DegreesToRadians(Ground.MaxTurnRateDegPerSec) * DeltaSeconds;

		// Clamped by the REMAINING error, so the final step is exactly the error and the
		// heading arrives on the runway direction without ever being assigned it.
		Heading = FMath::UnwindRadians(Heading + FMath::Clamp(Error, -MaxStep, MaxStep));

		// Creeping while it turns, and held at the threshold: the line-up happens on the
		// spot as far as the runway is concerned, so Travelled stays at zero.
		Speed = Ground.MinTaxiSpeed;

		if (FMath::Abs(FMath::UnwindRadians(RunwayHeading - Heading)) <= LinedUpTolerance)
		{
			Phase = ETakeoffPhase::Roll;
		}
		break;
	}

	case ETakeoffPhase::Roll:
	{
		Speed = FMath::Min(Speed + Ground.Takeoff.Accel * DeltaSeconds, Ground.Takeoff.SpeedCap);
		Travelled += Speed * DeltaSeconds;

		if (Speed >= Ground.Takeoff.SpeedCap)
		{
			Phase = ETakeoffPhase::Rotate;
		}
		break;
	}

	case ETakeoffPhase::Rotate:
	{
		// Still accelerating: an aircraft does not stop gaining speed at Vr, it stops
		// gaining it on the ground.
		Speed += Ground.Takeoff.Accel * DeltaSeconds;
		Travelled += Speed * DeltaSeconds;

		Pitch = FMath::Min(Pitch + Climb.RotateRateDegPerSec * DeltaSeconds,
			Climb.ClimbPitchDegrees);

		// The wheels come off part way up, and the climb builds with the attitude rather
		// than starting at full rate the instant the nose moves.
		const double LiftOffPitch = Climb.ClimbPitchDegrees * LiftOffPitchFraction;
		if (Pitch > LiftOffPitch)
		{
			const double Established = FMath::Clamp(
				(Pitch - LiftOffPitch) / FMath::Max(Climb.ClimbPitchDegrees - LiftOffPitch, 1e-6),
				0.0, 1.0);
			Altitude += Climb.ClimbRate * Established * DeltaSeconds;
		}

		if (Pitch >= Climb.ClimbPitchDegrees && Altitude > 0.0)
		{
			Phase = ETakeoffPhase::Climb;
		}
		break;
	}

	case ETakeoffPhase::Climb:
	{
		Pitch = Climb.ClimbPitchDegrees;
		Altitude += Climb.ClimbRate * DeltaSeconds;
		Travelled += Speed * DeltaSeconds;

		if (Altitude >= Climb.ClearAltitude)
		{
			Phase = ETakeoffPhase::Clear;
		}
		break;
	}

	default:
		break;
	}

	OutPosition = Threshold + Direction * Travelled;
	OutHeading = Heading;
	OutAltitude = Altitude;
	OutPitch = Pitch;
	return true;
}
