#include "Model/RoadAgent.h"

#include "AirsideLog.h"

void FRoadAgent::AdvanceEngine(double DeltaSeconds)
{
	if (!Airframe.Engine.IsSet())
	{
		// Nothing authored: fall back to the switch this replaced, so an airframe with no
		// engine figures still shows a turning propeller rather than a stopped one.
		EngineRPM = bEngineRunning ? 2000.0 : 0.0;
		return;
	}

	const double Target = bEngineRunning ? Airframe.Engine.MaxRPM : 0.0;

	// The rate is the whole travel over the time it takes, so the two seconds figures mean
	// what they say - idle to governed, and governed to stopped.
	const double Seconds = bEngineRunning ? Airframe.Engine.SpoolUpSeconds : Airframe.Engine.SpoolDownSeconds;
	const double Rate = Airframe.Engine.MaxRPM / Seconds;

	// Clamped by the REMAINING error, so the last step lands exactly on the target and the
	// propeller neither overshoots nor creeps. The same construction the line-up turn and
	// the flare use.
	const double Error = Target - EngineRPM;
	const double MaxStep = Rate * DeltaSeconds;
	EngineRPM += FMath::Clamp(Error, -MaxStep, MaxStep);
}

FAgentMotion FRoadAgent::DescribeMotion(const FVector2D& At, double Heading,
	double Altitude, double PitchDegrees) const
{
	FAgentMotion Motion;
	Motion.Position = At;
	Motion.Heading = Heading;
	Motion.Altitude = Altitude;
	Motion.PitchDegrees = PitchDegrees;

	// Whichever phase is driving. The follower's speed is meaningless once a departure has
	// taken over, and the departure's is meaningless before it.
	Motion.GroundSpeed = Phase == EAgentPhase::Departing ? Departure.Speed : Follower.Speed;

	// STATE, NOT SPEED. A stationary aircraft with its engine running is an aircraft with a
	// turning propeller, which is what this used to get wrong.
	Motion.bEngineRunning = bEngineRunning;

	// AND WHERE THE PROPELLER HAS GOT TO, which is not the same question - see
	// FAgentMotion::EngineRPM. The view spins the prop at this, so a shutdown winds down.
	Motion.EngineRPM = EngineRPM;

	// Off the wheels only once the rotation is finished. The phase already knows, so nothing
	// here has to infer it from the altitude being above zero.
	Motion.bAirborne = Phase == EAgentPhase::Departing && Departure.Phase == ETakeoffPhase::Climb;

	return Motion;
}

bool FRoadAgent::StartArrival(const FVector2D& Threshold, const FVector2D& Direction, double RunwayLength,
	const FAirframe& InAirframe, double VacateAt, const FRoutePlan& InTaxiInPlan)
{
	if (!Arrival.Start(Threshold, Direction, RunwayLength,
		InAirframe.Ground, InAirframe.Climb, InAirframe.Approach, VacateAt))
	{
		// FLandingRun has already logged why. Nothing else is touched: an arrival that
		// cannot be flown must leave no trace of itself on the agent, rather than one
		// half-armed.
		return false;
	}

	Phase = EAgentPhase::Arriving;
	Airframe = InAirframe;
	TaxiInPlan = InTaxiInPlan;
	bEngineRunning = true;

	// ALREADY TURNING. An arrival appears on final with its engine running - spooling up
	// from stopped would show it gliding down the approach with a dead propeller.
	EngineRPM = InAirframe.Engine.MaxRPM;

	// SEEDED FROM THE APPROACH'S OWN STARTING POSE, the same rule StartTaxi follows for
	// Polyline[0]: a caller that reads LastMotion before the first real Advance must see
	// where the arrival actually begins, never the FVector2D default - the "world origin"
	// bug this field exists to prevent. A zero-second Advance costs nothing (every term in
	// FLandingRun::Advance multiplies by DeltaSeconds) and returns exactly the pose Start()
	// just computed, via the same DescribeMotion call the real Arriving branch uses.
	FVector2D At = FVector2D::ZeroVector;
	double Heading = 0.0;
	double Altitude = 0.0;
	double Pitch = 0.0;
	Arrival.Advance(0.0, At, Heading, Altitude, Pitch);
	LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);

	return true;
}

void FRoadAgent::StartTaxi(const FRoutePlan& Plan, const FAirframe& InAirframe)
{
	Phase = EAgentPhase::Taxiing;
	Airframe = InAirframe;
	Follower.Start(Plan, InAirframe.Ground);

	bEngineRunning = true;

	// FROM COLD, deliberately: a plain dispatch starts at a stand, so the propeller spools
	// up as it begins to taxi, which is the thing that was asked for.
	EngineRPM = 0.0;

	// The fallback pose, in case the first Advance's Follower.Advance declines (a plan too
	// short to have a direction) - so the view still appears at the start of its route
	// rather than at the origin. Overwritten the moment Advance succeeds.
	LastMotion = FAgentMotion();
	if (Plan.Polyline.Num() > 0)
	{
		LastMotion.Position = Plan.Polyline[0];
	}
}

void FRoadAgent::ArmDeparture(const FVector2D& Threshold, const FVector2D& Direction, double RunwayLength)
{
	bDepartureArmed = true;
	DepartureOrder.Threshold = Threshold;
	DepartureOrder.Direction = Direction;
	DepartureOrder.RunwayLength = RunwayLength;
}

bool FRoadAgent::Advance(double DeltaSeconds, FAgentMotion& OutMotion)
{
	// FIRST, AND WHATEVER PHASE IS DRIVING. An engine spooled only inside one phase's branch
	// would freeze whenever the aircraft was doing something else - which is most of the time.
	AdvanceEngine(DeltaSeconds);

	FVector2D At = LastMotion.Position;
	double Heading = LastMotion.Heading;
	double Altitude = LastMotion.Altitude;
	double Pitch = LastMotion.PitchDegrees;

	switch (Phase)
	{
	case EAgentPhase::Arriving:
	{
		if (Arrival.Advance(DeltaSeconds, At, Heading, Altitude, Pitch))
		{
			LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);
			OutMotion = LastMotion;
			return true;
		}

		// VACATED: hand over to the taxi. The route was planned at dispatch - see
		// UAirsideTraffic::DispatchArrival - so this cannot fail here and strand an
		// aircraft on the runway with nowhere to go.
		//
		// Started from Airframe.Ground, NOT Follower.Ground: the follower has never been
		// started before this point, so its Ground is still the struct default (Accel 100,
		// SpeedCap 1000, turn rate 10) rather than the airframe's figures - see issue #27.
		Phase = EAgentPhase::Taxiing;
		Follower.Start(TaxiInPlan, Airframe.Ground);
		UE_LOG(LogAirsideTraffic, Log, TEXT("Vacated; taxiing in."));

		// THIS FRAME'S ARRIVAL ADVANCE DECLINED, so - same rule as every other decline -
		// LastMotion is handed back UNRECOMPUTED rather than described afresh. Recomputing
		// it here would read GroundSpeed off the follower Start() just reset to zero,
		// which would report the aircraft as instantaneously stopping for one frame before
		// its taxi speed ramps back up - a glitch the old Tick avoided by leaving the view
		// untouched on exactly this frame.
		OutMotion = LastMotion;
		return true;
	}

	case EAgentPhase::Taxiing:
	{
		FVector2D FollowAt = At;
		double FollowHeading = Heading;
		if (Follower.Advance(DeltaSeconds, FollowAt, FollowHeading))
		{
			LastMotion = DescribeMotion(FollowAt, FollowHeading);
			OutMotion = LastMotion;
		}
		else
		{
			// Advance is the ONLY thing that decides where an agent is. When it declines -
			// no route, or a polyline too short to have a direction - the pose is left
			// exactly as it was, rather than an unset FVector2D writing it to the origin.
			OutMotion = LastMotion;
		}

		if (Follower.HasArrived())
		{
			if (bDepartureArmed)
			{
				// ARRIVED ON A RUNWAY: hand over. The heading it arrived on carries across,
				// so the line-up turn starts from where the taxi actually left it rather
				// than from a fresh guess - which is what makes a backtrack read as one.
				bDepartureArmed = false;
				if (Departure.Start(DepartureOrder.Threshold, DepartureOrder.Direction,
					DepartureOrder.RunwayLength, Airframe.Ground, Airframe.Climb, LastMotion.Heading))
				{
					Phase = EAgentPhase::Departing;
					UE_LOG(LogAirsideTraffic, Log, TEXT("Taxi complete; rolling for departure."));
				}
				// Declined (see FTakeoffRun::Start): bDepartureArmed is already cleared
				// above, so this does NOT stay Taxiing indefinitely - the follower has
				// already arrived, so the very next Advance re-checks HasArrived(), finds
				// bDepartureArmed false, and takes the PARKED branch below instead. This
				// route was validated before dispatch, so it is not expected to happen,
				// but parking one frame late is a safer failure than flying a departure
				// that just refused itself.
			}
			else
			{
				// PARKED: the taxi is over, so the turnaround starts. Reached only once -
				// the next frame is Phase == Parked rather than Taxiing, so this branch
				// cannot re-arm the countdown. Counted rather than acted on at once, because
				// an engine that stopped the instant the wheels did would look like a
				// stall - an arriving aircraft sits at the stand with the engine running
				// while the chocks go in.
				Phase = EAgentPhase::Parked;
				ShutdownCountdown = ShutdownPause;

				// ZEROED HERE: the Parked branch below never calls Follower.Advance again, so
				// its Speed would otherwise sit at whatever the last taxiing tick left it at
				// FOR EVER - and DescribeMotion reads exactly that field as GroundSpeed
				// regardless of phase, so a parked aircraft would report itself still rolling.
				Follower.Speed = 0.0;
				UE_LOG(LogAirsideTraffic, Log,
					TEXT("Parked. Shutting down in %.0f s."), ShutdownCountdown);
			}
		}

		return true;
	}

	case EAgentPhase::Parked:
	{
		if (ShutdownCountdown > 0.0)
		{
			ShutdownCountdown -= DeltaSeconds;
			if (ShutdownCountdown <= 0.0)
			{
				ShutdownCountdown = 0.0;

				// The flag bEngineRunning was introduced for - see its comment, which says
				// a shutdown at the stand is what would clear it. The propeller stops and
				// the aircraft stays where it is.
				bEngineRunning = false;
				UE_LOG(LogAirsideTraffic, Log, TEXT("Engine shut down at the stand."));
			}
		}

		LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);
		OutMotion = LastMotion;
		return true;
	}

	case EAgentPhase::Departing:
	{
		if (Departure.Advance(DeltaSeconds, At, Heading, Altitude, Pitch))
		{
			LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);
			OutMotion = LastMotion;
			return true;
		}

		// Cleared. The aircraft has gone - see UAirsideTraffic::Advance for why the caller
		// destroys the view and drops the agent the moment this returns false.
		UE_LOG(LogAirsideTraffic, Log, TEXT("Departure complete, agent despawned"));
		Phase = EAgentPhase::Gone;
		return false;
	}

	case EAgentPhase::Gone:
	default:
		return false;
	}
}
