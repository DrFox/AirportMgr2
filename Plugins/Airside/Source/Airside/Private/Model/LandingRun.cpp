#include "Model/LandingRun.h"

DEFINE_LOG_CATEGORY_STATIC(LogLanding, Log, All);

double FLandingRun::RequiredLandingDistance(const FGroundPerformance& InGround,
	const FApproachPerformance& InApproach)
{
	const double Vref = InGround.Landing.SpeedCap;
	const double Brake = InGround.Landing.Decel;
	if (Vref <= 0.0 || Brake <= 0.0 || !InApproach.IsSet())
	{
		return 0.0;
	}

	// THE AIR DISTANCE. On the glideslope alone the flare height would be flown off in
	// FlareHeight / tan(slope). The flare stretches that, because arresting the descent is
	// precisely a refusal to keep descending at the glideslope rate - so doubling it is a
	// deliberate over-estimate rather than a fudge, and it is the direction a refusal wants
	// to err in.
	const double Slope = FMath::Tan(FMath::DegreesToRadians(InApproach.GlideslopeDegrees));
	const double AirDistance = Slope > 0.0 ? 2.0 * (InApproach.FlareHeight / Slope) : 0.0;

	// THE BRAKING ROLL, to taxi speed rather than to a stop: an aircraft that has slowed to
	// taxi speed has vacated, and demanding room to stop dead would refuse runways that are
	// perfectly usable.
	const double TaxiCap = FMath::Max(InGround.Taxi.SpeedCap, 0.0);
	const double Excess = FMath::Max(Vref * Vref - TaxiCap * TaxiCap, 0.0);

	return AirDistance + Excess / (2.0 * Brake);
}

bool FLandingRun::Start(const FVector2D& InThreshold, const FVector2D& InDirection,
	double InRunwayLength, const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
	const FApproachPerformance& InApproach)
{
	Phase = ELandingPhase::Vacated;

	if (!InGround.IsSet() || !InGround.Landing.IsSet() || !InClimb.IsSet() || !InApproach.IsSet())
	{
		UE_LOG(LogLanding, Warning,
			TEXT("Arrival refused: the airframe has no landing or approach performance."));
		return false;
	}

	if (InDirection.IsNearlyZero())
	{
		UE_LOG(LogLanding, Warning, TEXT("Arrival refused: the runway has no direction."));
		return false;
	}

	const double Needed = RequiredLandingDistance(InGround, InApproach);
	if (InRunwayLength < Needed)
	{
		UE_LOG(LogLanding, Warning,
			TEXT("Arrival refused: %.0f uu of runway, %.0f needed to stop from %.0f uu/s."),
			InRunwayLength, Needed, InGround.Landing.SpeedCap);
		return false;
	}

	Threshold = InThreshold;
	Direction = InDirection.GetSafeNormal();
	RunwayLength = InRunwayLength;
	Ground = InGround;
	Climb = InClimb;
	Approach = InApproach;

	// SHORT OF THE THRESHOLD, hence negative, and at the height the glideslope puts it at
	// that distance. Derived from FinalAltitude rather than authored beside it - see
	// FApproachPerformance::FinalDistance.
	Travelled = -Approach.FinalDistance();
	Altitude = Approach.FinalAltitude;
	Speed = Ground.Landing.SpeedCap;
	Heading = FMath::Atan2(Direction.Y, Direction.X);

	// The approach attitude is the angle the wing needs at Vref, LESS the descent angle: the
	// aircraft is flying nose-high relative to its flight path while the flight path itself
	// points down. Derived, so the nose sits where the speed says rather than where a number
	// typed into a details panel says.
	Pitch = Climb.RequiredAngleAt(Speed, Ground.Takeoff.SpeedCap) - Approach.GlideslopeDegrees;

	Phase = ELandingPhase::Approach;

	UE_LOG(LogLanding, Log,
		TEXT("Arrival armed: %.0f uu runway, %.0f needed, Vref %.0f uu/s, joining %.0f uu out."),
		RunwayLength, Needed, Speed, Approach.FinalDistance());
	return true;
}

bool FLandingRun::Advance(double DeltaSeconds, FVector2D& OutPosition, double& OutHeading,
	double& OutAltitude, double& OutPitch)
{
	if (Phase == ELandingPhase::Vacated)
	{
		return false;
	}

	const double Vr = Ground.Takeoff.SpeedCap;

	switch (Phase)
	{
	case ELandingPhase::Approach:
	{
		// Stabilised: Vref, on the slope, nose where the speed puts it. A real approach is
		// flown to be boring and this one is modelled that way deliberately.
		Speed = Ground.Landing.SpeedCap;

		const double Gamma = FMath::DegreesToRadians(Approach.GlideslopeDegrees);
		Altitude -= Speed * FMath::Sin(Gamma) * DeltaSeconds;
		Travelled += Speed * FMath::Cos(Gamma) * DeltaSeconds;

		if (Altitude <= Approach.FlareHeight)
		{
			Phase = ELandingPhase::Flare;
		}
		break;
	}

	case ELandingPhase::Flare:
	{
		// THRUST COMES OFF, so the speed bleeds. This is what drives everything below: the
		// angle the wing needs goes as the inverse square of speed, so a falling speed makes
		// a RISING requirement, and the nose has to chase it.
		Speed = FMath::Max(Speed - Approach.FlareDecel * DeltaSeconds, 0.0);

		// THE PILOT FLIES A PATH, NOT AN ATTITUDE, AND THE PATH EASES WITH HEIGHT.
		//
		// The commanded sink is proportional to the height remaining - h' = -h/tau, the law a
		// pilot flies and the one an autoland computes. That is what makes a flare terminate
		// smoothly: the closer the ground, the gentler the descent, so it arrives softly
		// instead of either thumping on or floating for ever.
		//
		// Bounded at both ends. Never steeper than the approach, or entering the flare would
		// command a PUSH; never gentler than the touchdown path, or the aircraft would ease
		// asymptotically toward the runway and never reach it.
		const double Required = Climb.RequiredAngleAt(Speed, Vr);

		const double ApproachSink =
			Speed * FMath::Sin(FMath::DegreesToRadians(Approach.GlideslopeDegrees));
		const double TouchdownSink =
			Speed * FMath::Sin(FMath::DegreesToRadians(Approach.TouchdownSinkAngleDegrees));
		const double Commanded = FMath::Clamp(
			Altitude / Approach.FlareTimeConstantSeconds, TouchdownSink, ApproachSink);

		// The attitude that flies that path: the angle the wing needs at THIS speed, plus the
		// (negative) flight path wanted. Capped at the tailstrike limit, which should not
		// normally bind - when it does, the aircraft has floated too long and settles firmly.
		const double WantedGamma = -FMath::RadiansToDegrees(
			FMath::Asin(FMath::Clamp(Speed > 0.0 ? Commanded / Speed : 0.0, -1.0, 1.0)));
		const double Wanted = FMath::Min(Required + WantedGamma, Approach.MaxFlarePitchDegrees);

		// Slewed, never assigned - the nose has a rate. Clamped by the REMAINING error so the
		// last step lands exactly on the wanted attitude, the same construction the line-up
		// turn uses in FTakeoffRun and for the same reason.
		const double Error = Wanted - Pitch;
		const double MaxStep = Approach.FlareRateDegPerSec * DeltaSeconds;
		Pitch += FMath::Clamp(Error, -MaxStep, MaxStep);

		// FLIGHT PATH IS ATTITUDE LESS THE ANGLE THE WING IS USING - the same expression the
		// climb uses, with the same two terms. Taken from the ACTUAL attitude, not the wanted
		// one, so an aircraft whose nose cannot keep up sinks faster than commanded. That
		// coupling is what keeps touchdown a consequence rather than a decision.
		const double Gamma = FMath::DegreesToRadians(Pitch - Required);

		Altitude += Speed * FMath::Sin(Gamma) * DeltaSeconds;
		Travelled += Speed * FMath::Cos(Gamma) * DeltaSeconds;

		if (Altitude <= 0.0)
		{
			// TOUCHDOWN. Not declared at a height or a time - it is where the arrested
			// descent finally reached the ground, which is why the flare above has to be
			// integrated rather than solved.
			Altitude = 0.0;
			Phase = ELandingPhase::Rollout;

			UE_LOG(LogLanding, Log,
				TEXT("Touchdown %.0f uu past the threshold at %.0f uu/s, %.1f deg nose-up."),
				Travelled, Speed, Pitch);
		}
		break;
	}

	case ELandingPhase::Rollout:
	{
		Altitude = 0.0;

		// Braking. Not below the taxi cap: at that point it has vacated, and continuing to
		// brake would stop it dead on the runway.
		const double Floor = FMath::Max(Ground.Taxi.SpeedCap, Ground.MinTaxiSpeed);
		Speed = FMath::Max(Speed - Ground.Landing.Decel * DeltaSeconds, Floor);
		Travelled += Speed * DeltaSeconds;

		// The nose comes down at the same rate it went up, and reaches the ground before the
		// aircraft is at taxi speed - so it is level while it is still rolling fast, which is
		// what a nosewheel touching down looks like.
		Pitch = FMath::Max(Pitch - Approach.FlareRateDegPerSec * DeltaSeconds, 0.0);

		if (Speed <= Floor)
		{
			Phase = ELandingPhase::Vacated;
			UE_LOG(LogLanding, Log,
				TEXT("Vacated %.0f uu past the threshold of %.0f available."),
				Travelled, RunwayLength);
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
