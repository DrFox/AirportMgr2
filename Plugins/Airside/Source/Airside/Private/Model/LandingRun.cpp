#include "Model/LandingRun.h"

#include "AirsideLog.h"

double FLandingRun::RequiredLandingDistance(const FGroundPerformance& InGround,
	const FClimbPerformance& InClimb, const FApproachPerformance& InApproach)
{
	// FLOWN, NOT ESTIMATED. See the header: a closed form is a second description of the
	// model, and this one disagreed with it by more than a factor of two - it charged the
	// flare's whole horizontal run against the runway, when most of that run happens before
	// the threshold, over the approach.
	//
	// Begin rather than Start, because Start's whole job is to apply the answer this is
	// computing. A probe refused by the check it exists to feed would return zero and make
	// every runway acceptable.
	FLandingRun Probe;
	// A long but FINITE runway. TNumericLimits<double>::Max() works arithmetically and puts
	// 1.8e308 into the armed log line, which is 300 characters of noise in the one place
	// somebody reads to find out why an arrival was refused.
	constexpr double UnboundedRunway = 1.0e9;
	if (!Probe.Begin(FVector2D::ZeroVector, FVector2D(1.0, 0.0),
		UnboundedRunway, InGround, InClimb, InApproach, 0.0))
	{
		return 0.0;
	}

	FVector2D At = FVector2D::ZeroVector;
	double Heading = 0.0;
	double Altitude = 0.0;
	double Pitch = 0.0;

	// A fixed step, so the answer does not depend on the frame rate of whoever asks. Fine
	// enough that the flare is integrated properly; the guard is against a set of numbers
	// that never touches down at all rather than against slow convergence.
	constexpr double Step = 1.0 / 60.0;
	constexpr int32 MaxSteps = 60 * 600;

	for (int32 Guard = 0; Guard < MaxSteps; ++Guard)
	{
		if (!Probe.Advance(Step, At, Heading, Altitude, Pitch))
		{
			break;
		}
		if (Probe.Phase == ELandingPhase::Vacated)
		{
			break;
		}
	}

	// Travelled is measured from the THRESHOLD and is negative on final, so this is exactly
	// the runway consumed and nothing else.
	return FMath::Max(Probe.Travelled, 0.0);
}

bool FLandingRun::Start(const FVector2D& InThreshold, const FVector2D& InDirection,
	double InRunwayLength, const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
	const FApproachPerformance& InApproach, double InVacateAt)
{
	Phase = ELandingPhase::Vacated;

	const double Needed = RequiredLandingDistance(InGround, InClimb, InApproach) * LandingMargin;
	if (InRunwayLength < Needed)
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("Arrival refused: %.0f uu of runway, %.0f needed to stop from %.0f uu/s "
				 "(%.0f flown plus a %.0f%% margin)."),
			InRunwayLength, Needed, InGround.Landing.SpeedCap,
			Needed / LandingMargin, (LandingMargin - 1.0) * 100.0);
		return false;
	}

	return Begin(InThreshold, InDirection, InRunwayLength, InGround, InClimb, InApproach,
		InVacateAt);
}

bool FLandingRun::Begin(const FVector2D& InThreshold, const FVector2D& InDirection,
	double InRunwayLength, const FGroundPerformance& InGround, const FClimbPerformance& InClimb,
	const FApproachPerformance& InApproach, double InVacateAt)
{
	Phase = ELandingPhase::Vacated;

	if (!InGround.IsSet() || !InGround.Landing.IsSet() || !InClimb.IsSet() || !InApproach.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("Arrival refused: the airframe has no landing or approach performance."));
		return false;
	}

	if (InDirection.IsNearlyZero())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("Arrival refused: the runway has no direction."));
		return false;
	}

	VacateAt = InVacateAt;

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

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Arrival armed: %.0f uu runway, vacating at %.0f, Vref %.0f uu/s, joining %.0f uu out."),
		RunwayLength, VacateAt, Speed, Approach.FinalDistance());
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

			UE_LOG(LogAirsideTraffic, Log,
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

		// SLOWED IS NOT VACATED. It has to reach the taxiway, and where the braking ran out
		// is not where the taxiway is - on this airport the only exit is at the far END of
		// the runway, so an aircraft that called itself vacated the moment it hit taxi speed
		// would hand over hundreds of metres short and jump to the exit node.
		if (Speed <= Floor && Travelled >= VacateAt)
		{
			Phase = ELandingPhase::Vacated;
			UE_LOG(LogAirsideTraffic, Log,
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
