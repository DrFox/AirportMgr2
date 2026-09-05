#include "Model/TakeoffRun.h"

#include "AirsideLog.h"

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

}

double FTakeoffRun::RequiredRoll(const FGroundPerformance& InGround,
	const FClimbPerformance& InClimb)
{
	const double Vr = InGround.Takeoff.SpeedCap;
	const double Accel = InGround.Takeoff.Accel;
	if (Vr <= 0.0 || Accel <= 0.0 || InClimb.RotateRateDegPerSec <= 0.0)
	{
		return 0.0;
	}

	// To Vr: v^2 = u^2 + 2as from rest, rearranged. The same arithmetic the braking profile
	// uses, pointing the other way.
	const double ToRotate = (Vr * Vr) / (2.0 * Accel);

	// Then the rotation, which is still ground roll. Timed as though the full angle were
	// needed - see the header for why over-estimating is the safe direction here.
	const double RotateSeconds = InClimb.LiftAngleAtRotateDegrees / InClimb.RotateRateDegPerSec;
	const double WhileRotating = Vr * RotateSeconds + 0.5 * Accel * RotateSeconds * RotateSeconds;

	return ToRotate + WhileRotating;
}

bool FTakeoffRun::Start(const FVector2D& InThreshold, const FVector2D& InDirection,
	double InRunwayLength, const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
	double InHeading)
{
	Phase = ETakeoffPhase::Clear;

	if (!InGround.IsSet() || !InGround.Takeoff.IsSet() || !InClimb.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("Departure refused: the airframe has no take-off or climb performance."));
		return false;
	}

	if (InDirection.IsNearlyZero())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("Departure refused: the runway has no direction."));
		return false;
	}

	const double Needed = RequiredRoll(InGround, InClimb);
	if (InRunwayLength < Needed)
	{
		// The whole reason this returns a bool. A strip shorter than the roll to Vr is one
		// this aircraft cannot leave, and rolling anyway simulates an overrun.
		UE_LOG(LogAirsideTraffic, Warning,
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

	UE_LOG(LogAirsideTraffic, Log,
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
		// STILL ON THE WHEELS. The nose comes up and the mains stay down - that is what a
		// rotation is, and an aircraft that started gaining height the instant the nose moved
		// was rising off the tarmac flat, which reads as a lift rather than a take-off.
		Speed += Ground.Takeoff.Accel * DeltaSeconds;
		Travelled += Speed * DeltaSeconds;
		Altitude = 0.0;

		Pitch = FMath::Min(Pitch + Climb.RotateRateDegPerSec * DeltaSeconds,
			Climb.ClimbPitchDegrees);

		// IT FLIES WHEN THE WING HAS THE ANGLE IT NEEDS AT THE SPEED IT HAS. Required angle
		// falls as the square of speed while the nose is coming up, so the two cross - and
		// they cross at zero climb rate, so the aircraft leaves the ground smoothly instead
		// of stepping off it.
		if (Pitch >= Climb.RequiredAngleAt(Speed, Ground.Takeoff.SpeedCap))
		{
			Phase = ETakeoffPhase::Climb;
		}
		break;
	}

	case ETakeoffPhase::Climb:
	{
		// Accelerating to best-rate speed, which is why the climb steepens after lift-off
		// rather than being established the moment the wheels come up.
		Speed = FMath::Min(Speed + Ground.Takeoff.Accel * DeltaSeconds, Climb.ClimbSpeed);
		Pitch = FMath::Min(Pitch + Climb.RotateRateDegPerSec * DeltaSeconds,
			Climb.ClimbPitchDegrees);

		// HEIGHT FROM SPEED AND PITCH, not from a rate someone typed in. The flight path is
		// the attitude less the angle the wing is using, and the climb is the speed along it.
		const double Rate = Climb.ClimbRateAt(Speed, Ground.Takeoff.SpeedCap);
		const double Gamma = FMath::Asin(FMath::Clamp(
			Speed > 0.0 ? Rate / Speed : 0.0, -1.0, 1.0));

		Altitude += Rate * DeltaSeconds;

		// Over the ground it covers only the horizontal component - a climbing aircraft is
		// not making its airspeed good along the runway.
		Travelled += Speed * FMath::Cos(Gamma) * DeltaSeconds;

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
