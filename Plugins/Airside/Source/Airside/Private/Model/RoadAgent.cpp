#include "Model/RoadAgent.h"

#include "AirsideLog.h"

void FRoadAgent::ClearArbitration()
{
	// See the declaration for why LastOverlaps is not touched here: some callers reset it
	// alongside this, one has already overwritten it with a freshly computed list.
	StopWithin = TNumericLimits<double>::Max();
	WaitingOn = 0;
	BlockedStep = INDEX_NONE;
}

void FRoadAgent::BeginCrossing(FRoadSegmentId Seed, ECrossingPhase InPhase)
{
	// THE INVARIANT IS "SET IFF" (see CrossingPhase), so both halves of it are checked here.
	// EndCrossing is the call for leaving a crossing, so this is never asked to arm "no
	// crossing" with a seed attached.
	checkf(InPhase != ECrossingPhase::None,
		TEXT("BeginCrossing needs a real phase; call EndCrossing to leave a crossing"));
	// AND Seed MUST NAME A REAL RUNWAY - the other direction of "set iff": a crossing phase
	// with no seed is exactly the half-written state the pair exists to make unrepresentable.
	checkf(Seed.IsSet(), TEXT("BeginCrossing needs a real seed; an unset one names no runway"));
	CrossingRunway = Seed;
	CrossingPhase = InPhase;
}

void FRoadAgent::EndCrossing()
{
	CrossingRunway = FRoadSegmentId();
	CrossingPhase = ECrossingPhase::None;
}

void FRoadAgent::SetGoalFrom(const FRoutePlan& Plan)
{
	GoalNode = Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : FGuidelineNodeId();
}

void FRoadAgent::StartEngineAtSpeed()
{
	bEngineRunning = true;

	// The same fallback AdvanceEngine uses when nothing is authored, so an airframe with no
	// engine figures still shows a turning propeller rather than a stopped one.
	EngineRPM = Airframe.Engine.IsSet() ? Airframe.Engine.MaxRPM : FEnginePerformance{}.MaxRPM;
}

void FRoadAgent::AdvanceEngine(double DeltaSeconds)
{
	if (!Airframe.Engine.IsSet())
	{
		// Nothing authored: fall back to the switch this replaced, so an airframe with no
		// engine figures still shows a turning propeller rather than a stopped one.
		EngineRPM = bEngineRunning ? FEnginePerformance{}.MaxRPM : 0.0;
		return;
	}

	const double Target = bEngineRunning ? Airframe.Engine.MaxRPM : 0.0;

	// The rate is the whole travel over the time it takes, so the two seconds figures mean
	// what they say - idle to governed, and governed to stopped.
	const double Seconds = bEngineRunning ? Airframe.Engine.SpoolUpSeconds : Airframe.Engine.SpoolDownSeconds;
	const double Rate = Airframe.Engine.MaxRPM / Seconds;

	// FInterpConstantTo clamps by the REMAINING error, so the last step lands exactly on
	// the target and the propeller neither overshoots nor creeps. The same construction
	// the line-up turn and the flare use.
	EngineRPM = FMath::FInterpConstantTo(EngineRPM, Target, DeltaSeconds, Rate);
}

FAgentMotion FRoadAgent::DescribeMotion(const FVector2D& At, double Heading,
	double Altitude, double PitchDegrees) const
{
	FAgentMotion Motion;
	Motion.Position = At;
	Motion.Heading = Heading;
	Motion.Altitude = Altitude;
	Motion.PitchDegrees = PitchDegrees;

	// WHICHEVER PHASE IS DRIVING, and that is three of them, not two. The follower's speed is
	// meaningless once a departure has taken over, the departure's is meaningless before it -
	// and the ARRIVAL's was missing entirely, so every landing reported the follower's speed,
	// which is zero until the follower starts at the exit.
	//
	// What that looked like: an aeroplane touching down at 70 knots with its wheels perfectly
	// still, which began to turn only as it swung off the runway and the taxi took over. The
	// view reads this figure and nothing else - see UAirsideAgentAnim, where wheel rate is
	// speed over radius - so a phase missing from this line is a phase with stopped wheels.
	switch (Phase)
	{
	case EAgentPhase::Arriving:    Motion.GroundSpeed = Arrival.Speed;   break;
	case EAgentPhase::Departing:   Motion.GroundSpeed = Departure.Speed; break;
	// A PUSH IS MOTION TOO, and the wheels turn under it - backwards, but the view has no
	// signed wheel rate and a tyre rolling the other way at 1.5 m/s reads the same. Omitting
	// this line is the "stopped wheels" defect above, in the one phase where the aeroplane is
	// closest to the camera.
	case EAgentPhase::Manoeuvring: Motion.GroundSpeed = Pushback.Speed;  break;
	default:                       Motion.GroundSpeed = Follower.Speed;  break;
	}

	// WHERE IT PITCHES ABOUT, which is a fact about the airframe rather than about this
	// frame - carried here because FAgentMotion is everything the view needs and the view
	// has no airframe to ask. See FAgentMotion::PitchPivotX.
	Motion.PitchPivotX = Airframe.FixedAxleX;

	// THE STEERING, from the follower WHATEVER THE PHASE - unlike GroundSpeed above. A
	// landing rollout and a take-off roll are steered on the rudder with the nosewheel
	// trailing straight, so the follower's zero is the right answer there rather than a
	// missing one.
	Motion.SteerAngleDegrees = Follower.SteerDegrees;

	// STATE, NOT SPEED. A stationary aircraft with its engine running is an aircraft with a
	// turning propeller, which is what this used to get wrong.
	Motion.bEngineRunning = bEngineRunning;

	// AND WHERE THE PROPELLER HAS GOT TO, which is not the same question - see
	// FAgentMotion::EngineRPM. The view spins the prop at this, so a shutdown winds down.
	Motion.EngineRPM = EngineRPM;

	// Off the wheels only once the rotation is finished, ON THEM from the moment an arrival's
	// wheels have not yet touched down. Departure and arrival are opposite questions of the
	// same fact, asked separately: a departure is airborne once it reaches the climb; an
	// arrival is airborne until FLandingRun::IsOnGround (Rollout or Vacated). The arrival half
	// was missing entirely, so an approach or a flare reported bAirborne=false and
	// UAirsideAgentAnim - gated on !bAirborne - spun the wheels under an aircraft still in the
	// air, while InspectFacts.cpp's "On final" status already assumed the opposite.
	Motion.bAirborne = (Phase == EAgentPhase::Departing && Departure.Phase == ETakeoffPhase::Climb)
		|| (Phase == EAgentPhase::Arriving && !Arrival.IsOnGround());

	return Motion;
}

bool FRoadAgent::StartArrival(const FRunwayEnd& End, const FAirframe& InAirframe, double VacateAt,
	const FRoutePlan& InTaxiInPlan)
{
	if (!Arrival.Start(End, InAirframe, VacateAt))
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
	Arrival.Advance(0.0, InAirframe, At, Heading, Altitude, Pitch);
	LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);

	return true;
}

void FRoadAgent::StartTaxi(const FRoutePlan& Plan, const FAirframe& InAirframe)
{
	Phase = EAgentPhase::Taxiing;
	Airframe = InAirframe;
	Follower.Start(Plan, InAirframe);

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

bool FRoadAgent::StartPushback(const FRoutePlan& PushPlan, const FRoutePlan& InTaxiOutPlan,
	const FAirframe& InAirframe, double PushSpeed, double PushAccel, double ThrustRPM)
{
	// A POWERBACK IS THE ENGINE DOING THE WORK; anything on a bar is moved by the tug and its
	// propeller is incidental. This is the ONE place in this slice where the pushback need
	// changes what happens, and it is justified because it is a fact about the AEROPLANE
	// rather than about a tug that does not exist yet.
	const bool bNeedsThrust = InAirframe.PushbackNeed == EPushbackNeed::SelfManoeuvre;

	if (!Pushback.Start(PushPlan, PushSpeed, PushAccel, bNeedsThrust))
	{
		// FPushbackRun has already declined. Nothing else is touched: a manoeuvre that cannot
		// be flown must leave no trace of itself on the agent rather than one half-armed -
		// the rule StartArrival states above, for the same reason.
		return false;
	}

	Phase = EAgentPhase::Manoeuvring;
	Airframe = InAirframe;
	PushbackThrustRPM = ThrustRPM;

	// THE ROUTE OUT, CARRIED FROM DISPATCH exactly as TaxiInPlan is for an arrival. The push
	// ends somewhere the departure route never visits, so this is the only thing that knows
	// how the aeroplane leaves from there - see FRoadAgent::TaxiOutPlan.
	TaxiOutPlan = InTaxiOutPlan;

	// PUSH AND START, and the cold start lives HERE rather than in StartTaxi - see this
	// function's declaration. AdvanceEngine spools from this zero over SpoolUpSeconds whatever
	// phase is driving, so the propeller is still coming up as the taxi takes over, which is
	// what "the spool outlasts the tug" means.
	bEngineRunning = true;
	EngineRPM = 0.0;

	// SEEDED FROM THE PUSH'S OWN STARTING POSE, the rule StartArrival follows: a caller that
	// reads LastMotion before the first real Advance must see where the manoeuvre actually
	// begins, never the FVector2D default - the "world origin" bug this field exists to
	// prevent. The HEADING comes from FPushbackRun, which has already turned the line's
	// tangent about; taking the polyline's own direction here would face the aeroplane out of
	// its stand for one frame, which is the very thing this phase exists to stop.
	LastMotion = FAgentMotion();
	if (PushPlan.Polyline.Num() > 0)
	{
		LastMotion.Position = PushPlan.Polyline[0];
	}
	LastMotion.Heading = Pushback.Heading;
	return true;
}

void FRoadAgent::ArmDeparture(const FRunwayEnd& End, double EntryOffset)
{
	bDepartureArmed = true;
	DepartureOrder.End = End;
	DepartureOrder.EntryOffset = EntryOffset;
}

bool FRoadAgent::Advance(double DeltaSeconds, FAgentMotion& OutMotion, EAgentEvent& OutEvent)
{
	// NONE ON THE COMMON FRAME, overwritten below only at an actual handover - see
	// EAgentEvent's own comment.
	OutEvent = EAgentEvent::None;

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
		if (Arrival.Advance(DeltaSeconds, Airframe, At, Heading, Altitude, Pitch))
		{
			LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);
			OutMotion = LastMotion;
			return true;
		}

		// VACATED: hand over to the taxi. The route was planned at dispatch - see
		// UGroundTraffic::DispatchArrival - so a landing is never armed for a stand it has
		// no way of reaching.
		//
		// ONE WAY IT CAN STILL BE UNUSABLE BY NOW, since 2026-09-06: the player rebuilt the
		// guideline graph while this aircraft was on final, and UGroundTraffic::ReResolvePlan
		// could not find live pavement for TaxiInPlan - so it marked it Unreachable (see
		// OnGraphRebuilt). Start() on an invalid plan leaves the follower already arrived, so
		// the aircraft becomes Taxiing and then Parked on the next tick, at the exit it
		// vacated to, and ClaimAhead's invalid-plan branch hands the strip back. That is the
		// intended outcome - a parked aeroplane off the runway, with a Warning in the log
		// naming it - and not a runway blocked by an aircraft with nowhere to go.
		//
		// STARTED FROM Airframe, THE AGENT'S OWN FIELD: the follower used to store its own
		// Ground copy, which had never been started before this point and so was still the
		// struct default (Accel 100, SpeedCap 1000, turn rate 10) rather than the airframe's
		// figures - see issue #27. Issue #83 removed that copy entirely, so passing anything
		// but this agent's own Airframe here is now a compile-time question, not a runtime one.
		//
		// AT THE ROLLOUT'S SPEED, not from rest: the landing run brakes to the taxi cap
		// before VacateAt and the exit arc begins there, so the taxi carries on at the
		// speed the wheels already have. Starting from zero here was the "stops dead at the
		// exit, turns on the spot, pulls away" the player reported on 2026-09-06.
		Phase = EAgentPhase::Taxiing;
		OutEvent = EAgentEvent::Vacated;
		// Speed, heading AND distance carry over: the rollout crossed VacateAt last frame
		// and stopped this far past it, which is this far along the arc.
		Follower.Start(TaxiInPlan, Airframe, Arrival.Speed, LastMotion.Heading,
			Arrival.Travelled - Arrival.VacateAt);
		UE_LOG(LogAirsideTraffic, Log, TEXT("Vacated; taxiing in."));

		// AND TAXI THIS SAME FRAME. The landing run declined this frame without moving (it
		// stops the tick AFTER it crosses VacateAt), so the frame's dt is the follower's.
		// This used to hand LastMotion back unrecomputed, which hid the follower's speed
		// reset to zero for one frame; a frame with no motion at all is exactly the step
		// the handover-continuity test exists to catch.
		[[fallthrough]];
	}

	// TWO LABELS ON ONE BODY, and NOT a third [[fallthrough]] arm above this one.
	//
	// THIS COST EIGHT TESTS. Arriving ends in [[fallthrough]] and relies on the NEXT case
	// label being the taxi. A Manoeuvring case written between the two silently redirected
	// every arrival's vacate handover into the push arm - the aeroplane landed, and then ran
	// FPushbackRun::Advance on an unarmed struct. Nothing about the insertion looked wrong;
	// the coupling is purely positional, which is exactly the kind this codebase's "check
	// where a list is CONSUMED" rule exists to catch.
	//
	// So the push shares the taxi's case instead of preceding it. Arriving still falls
	// through, arriving with Phase already set to Taxiing, so the branch below is false for
	// it and the adjacency it depends on can no longer be broken by inserting a case.
	case EAgentPhase::Manoeuvring:
	case EAgentPhase::Taxiing:
	{
		if (Phase == EAgentPhase::Manoeuvring)
		{
			// THE THRUST GATE IS ASKED HERE rather than inside FPushbackRun, because the RPM
			// is the AGENT's: that struct is world-free and holds no engine, exactly as it
			// holds no airframe. A powerback waits at rest until the propeller has something
			// to push with; a towed aeroplane moves on frame one.
			const bool bHasThrust = EngineRPM >= PushbackThrustRPM;

			FVector2D PushAt = At;
			double PushHeading = Heading;
			if (Pushback.Advance(DeltaSeconds, StopWithin, bHasThrust, PushAt, PushHeading))
			{
				LastMotion = DescribeMotion(PushAt, PushHeading);
				OutMotion = LastMotion;
				return true;
			}

			// OFF THE STAND: hand over to the taxi, on the route planned FROM HERE at dispatch.
			// Not a splice into the push's own plan, which is the correction this phase was
			// rewritten for: a push reverses onto the arm the departure does NOT take, so when
			// it ends the aeroplane is standing somewhere that route never visits. See
			// FRoadAgent::TaxiOutPlan.
			//
			// FROM ITS BEGINNING, therefore - Travelled zero, not the push's distance - and
			// the heading carries across unchanged. The push finished facing the line's
			// tangent turned about, and the taxi out leaves the same point the other way, so
			// the two agree exactly and the follower starts with ZERO heading error. That is
			// what makes the aeroplane pull away instead of pirouetting.
			//
			// SPEED ZERO, deliberately, and unlike the Vacated handover above which carries
			// the rollout's speed into the taxi. This aeroplane has just been moving BACKWARDS
			// and is about to move forwards: carrying the push's speed across would have it
			// pull away at the speed it was pushed at, in the other direction, on the
			// handover frame.
			//
			// Follower.Start AND NOT StartTaxi, which writes EngineRPM = 0.0 from cold and
			// would undo the spool the push has been running - see StartPushback.
			Phase = EAgentPhase::Taxiing;
			OutEvent = EAgentEvent::PushedBack;
			Follower.Start(TaxiOutPlan, Airframe, 0.0, Pushback.Heading);
			UE_LOG(LogAirsideTraffic, Log, TEXT("Push complete; taxiing out."));

			// AND TAXI THIS SAME FRAME, falling out of this branch rather than returning, for
			// the reason the Vacated fallthrough gives: the push declined this frame without
			// moving, so the frame's dt is the follower's. A frame with no motion at all is
			// exactly the step the handover-continuity test catches.
		}

		FVector2D FollowAt = At;
		double FollowHeading = Heading;
		// StopWithin, not the unbounded overload: arbitration is the ONE input into the one
		// follower, and it defaults to unbounded, so an agent nobody has arbitrated for
		// drives exactly as it did before M2.
		if (Follower.Advance(DeltaSeconds, Airframe, StopWithin, FollowAt, FollowHeading))
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
				// ARRIVED ON A RUNWAY: hand over. Heading, speed and WHERE it joined carry
				// across, so the roll starts from where the taxi actually left it - aligned
				// and rolling after an entry arc, or facing the wrong way at the threshold
				// after a backtrack, where the line-up turn is then the right behaviour.
				// Restarting at the threshold from creep was the teleport-and-spin of
				// samples/runway1.png (2026-09-07).
				bDepartureArmed = false;
				if (Departure.Start(DepartureOrder.End, Airframe, LastMotion.Heading,
					DepartureOrder.EntryOffset, Follower.Speed))
				{
					Phase = EAgentPhase::Departing;
					OutEvent = EAgentEvent::LinedUp;
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
				OutEvent = EAgentEvent::Parked;
				ShutdownCountdown = ShutdownPause;

				// ZEROED HERE: the Parked branch below never calls Follower.Advance again, so
				// its Speed would otherwise sit at whatever the last taxiing tick left it at
				// FOR EVER - and DescribeMotion reads exactly that field as GroundSpeed
				// regardless of phase, so a parked aircraft would report itself still rolling.
				Follower.Speed = 0.0;
				// AND RE-DESCRIBED, so the motion this frame hands back agrees with the phase
				// it just entered: the pose computed above carried the arriving speed, and a
				// panel reading "Parked, 0.4 m/s" for one frame is the small version of the
				// bug the zeroing above exists to stop (Airside.Model.InspectFacts caught it).
				LastMotion = DescribeMotion(FollowAt, FollowHeading);
				OutMotion = LastMotion;
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
		// BEFORE, so the climb transition can be told apart from "already climbing" - NOT an
		// EAgentPhase change (Departing both before and after), which is why AdvanceOnce used
		// to check this sub-phase directly from outside rather than being told. See
		// EAgentEvent::Airborne's own comment.
		const ETakeoffPhase TakeoffPhaseBefore = Departure.Phase;
		if (Departure.Advance(DeltaSeconds, Airframe, At, Heading, Altitude, Pitch))
		{
			LastMotion = DescribeMotion(At, Heading, Altitude, Pitch);
			OutMotion = LastMotion;
			if (TakeoffPhaseBefore != ETakeoffPhase::Climb && Departure.Phase == ETakeoffPhase::Climb)
			{
				OutEvent = EAgentEvent::Airborne;
			}
			return true;
		}

		// Cleared. The aircraft has gone - see UGroundTraffic::Advance for why the caller
		// destroys the view and drops the agent the moment this returns false.
		UE_LOG(LogAirsideTraffic, Log, TEXT("Departure complete, agent despawned"));
		Phase = EAgentPhase::Gone;
		OutEvent = EAgentEvent::Gone;
		return false;
	}

	case EAgentPhase::Gone:
	default:
		return false;
	}
}
