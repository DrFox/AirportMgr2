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

void FRoadAgent::AdvanceGear(double DeltaSeconds)
{
	if (!Airframe.Gear.IsSet())
	{
		// FIXED GEAR, permanently - see FGearPerformance, where zero travel means a fact
		// about the aeroplane rather than a missing measurement. Asserted rather than left
		// alone because an FRoadAgent is reused across dispatches and could otherwise carry
		// a retracted phase into an airframe that has no way of lowering it again.
		GearPhase = EGearPhase::Down;
		GearCycleSeconds = 0.0;
		return;
	}

	// THE COMMAND - what the pilot has called for, not where the gear has got to. The two are
	// separate for the same reason FAgentMotion::bEngineRunning and EngineRPM are: between a
	// command and its completion there are nine seconds in which they disagree.
	//
	// READ BY PHASE, NOT BY ALTITUDE ALONE. ExtendBelowHeight sits ABOVE RetractAboveHeight,
	// so an arrival descending through the retract height satisfies "airborne and above it"
	// word for word and would raise its gear on short final. The model already knows which
	// way the aeroplane is going; asking the altitude to tell us would re-derive it.
	bool bWantUp = false;
	if (Phase == EAgentPhase::Departing)
	{
		// bAirborne IS THE PRECONDITION AND HEIGHT IS THE CUE. Airside.Present.AgentMotion
		// case 4 guards the half that matters most - a rotation is not airborne - so nothing
		// here can start moving while the mains are still carrying the aeroplane.
		bWantUp = LastMotion.bAirborne
			&& LastMotion.Altitude >= Airframe.Gear.RetractAboveHeight;
	}
	else if (Phase == EAgentPhase::Arriving)
	{
		// LATENT AT TODAY'S FIGURES: an arrival joins final at FinalAltitude, 2000 uu, well
		// below ExtendBelowHeight, so this is false from the first frame and every arrival is
		// born down and locked. It starts mattering the day the approach is joined higher.
		bWantUp = LastMotion.Altitude > Airframe.Gear.ExtendBelowHeight;
	}
	// Every other phase is on the ground, where the gear is down by definition.

	const bool bMoving = GearPhase == EGearPhase::Raising || GearPhase == EGearPhase::Lowering;

	// A CYCLE IN PROGRESS IS NOT INTERRUPTED. Real gear can be reversed mid-travel; nothing
	// in this game commands that, and honouring it would mean carrying the position across a
	// direction change rather than restarting a timer at zero. Left out deliberately.
	if (!bMoving && bWantUp != (GearPhase == EGearPhase::Up))
	{
		GearPhase = bWantUp ? EGearPhase::Raising : EGearPhase::Lowering;
		GearCycleSeconds = 0.0;
		return;
	}

	if (!bMoving)
	{
		return;
	}

	GearCycleSeconds += DeltaSeconds;
	if (GearCycleSeconds >= Airframe.Gear.CycleSeconds())
	{
		// THE PHASE SAYS WHERE IT IS AT REST, not the timer. A timer left running past the
		// end would answer correctly right up until anything reset it.
		GearPhase = GearPhase == EGearPhase::Raising ? EGearPhase::Up : EGearPhase::Down;
		GearCycleSeconds = 0.0;
	}
}

void FRoadAgent::GearFractions(double& OutGearDown, double& OutDoorOpen) const
{
	// THE RESTING POSES ARE ANSWERED HERE AND NOT BY THE EVALUATOR, because a resting pose is
	// not a point in a cycle - a fixed-gear airframe has no cycle to sample at all.
	// THE TWO RESTING POSES ARE DIFFERENT, and that is the point: on a 737 the nose bay doors
	// are linked to the strut, so they hang OPEN with the gear down and shut only once it is
	// stowed. A parked or approaching aeroplane therefore sits gear-down, doors-open - which
	// is also SK_Plane4's bind pose, so the animgraph applies no rotation at all there.
	switch (GearPhase)
	{
	case EGearPhase::Down: OutGearDown = 1.0; OutDoorOpen = 1.0; return;
	case EGearPhase::Up:   OutGearDown = 0.0; OutDoorOpen = 0.0; return;
	default: break;
	}

	Airframe.Gear.FractionsAt(GearCycleSeconds, GearPhase == EGearPhase::Raising,
		OutGearDown, OutDoorOpen);
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
	// A PUSH IS MOTION TOO, and the wheels turn under it - BACKWARDS, which this line used to
	// discard. It read `= Pushback.Speed` and said so in its own comment: "the view has no
	// signed wheel rate and a tyre rolling the other way at 1.5 m/s reads the same". It does
	// not read the same, it reads as a tyre rolling the wrong way, and it was reported from
	// play on 2026-09-20 - on the fuel truck, which reverses in front of the camera on every
	// service cycle, but the defect was always here too. Omitting the line entirely is the
	// "stopped wheels" defect above, in the one phase where the aeroplane is closest to the
	// camera.
	//
	// THE SIGN IS APPLIED HERE AND NOWHERE ELSE. FPushbackRun::Speed and FReverseRun::Speed
	// both stay magnitudes, as FRouteFollower::Speed is, because which way a phase points is
	// a fact about the PHASE - and this switch is the one place that knows it. Signing them
	// at the source would be the same fact written in three structs that must agree.
	case EAgentPhase::Manoeuvring: Motion.GroundSpeed = -Pushback.Speed; break;
	// REVERSE.SPEED, NOT REVERSE.REVERSESPEED, and the difference is a whole defect. The
	// latter is the AUTHORED CAP, written once by FReverseRun::Start and never again, so a
	// truck held at a standstill by arbitration went on reporting a full metre a second and
	// its wheels spun on the spot - "they seem to rotate independent of speed, they just
	// spin". Speed is what the last Advance actually covered. See FReverseRun::Speed.
	case EAgentPhase::Reversing:   Motion.GroundSpeed = -Reverse.Speed;  break;
	default:                       Motion.GroundSpeed = Follower.Speed;  break;
	}

	// WHERE IT PITCHES ABOUT, which is a fact about the airframe rather than about this
	// frame - carried here because FAgentMotion is everything the view needs and the view
	// has no airframe to ask. See FAgentMotion::PitchPivotX.
	Motion.PitchPivotX = Airframe.FixedAxleX;

	// THE STEERING, from whichever phase is doing it - which for every phase but one is the
	// follower. A landing rollout and a take-off roll are steered on the rudder with the
	// nosewheel trailing straight, so the follower's zero is the right answer there rather
	// than a missing one, and a pushback goes straight out of the stand.
	//
	// REVERSING IS THE EXCEPTION, and this line read `= Follower.SteerDegrees` unconditionally
	// until 2026-09-20 - its own comment said "from the follower WHATEVER THE PHASE". The
	// follower does not run during a reverse, so SteerDegrees held whatever it last computed
	// before the manoeuvre armed; a truck parks its steered axle on the service point with the
	// wheels near straight, so the whole reverse inherited that. Reported from play as a truck
	// that "straightened its wheels while still turning and then slid around the last part".
	//
	// THE THIRD FIELD IN THIS FUNCTION WITH THE SAME SHAPE - see GroundSpeed above, which was
	// missing the Arriving phase once and reporting a cap rather than a state for Reversing.
	// Each time, the struct that was moving the agent was not the struct being read. A fourth
	// field added here should be asked which phase owns it before it is wired to the follower.
	Motion.SteerAngleDegrees = Phase == EAgentPhase::Reversing
		? Reverse.SteerDegrees : Follower.SteerDegrees;

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

	// THE GEAR, BOTH NUMBERS FROM THE ONE EVALUATOR - see FGearPerformance::FractionsAt. The
	// view applies these to bones and derives neither of them; a second evaluator would let
	// the doors the player sees disagree with the doors the model thinks it opened.
	GearFractions(Motion.GearDownFraction, Motion.BayDoorOpenFraction);

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

	// AND THE GEAR, for the same reason and on the same terms - see AdvanceGear. A cycle that
	// only ran inside one phase's branch would freeze the doors half open at a handover.
	AdvanceGear(DeltaSeconds);

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

		// A SPAN MEANT TO BE DRIVEN BACKWARDS IS NOT THE FOLLOWER'S TO DRIVE, and this is the
		// check that was missing until 2026-09-17.
		//
		// FStandLayoutBuild marked the reverse leg, FReverseRun was written to play one back,
		// and EAgentPhase::Reversing was declared for it - and NOTHING JOINED THE THREE. The
		// follower walked the reverse leg like any other span, which means turning the body
		// through 180 degrees to face along it first, beside a parked aeroplane. Reported from
		// PIE: "it just flipped 180 degrees and went out forwards". That is the fourth time
		// this codebase has shipped a list nothing consumed; see CLAUDE.md.
		//
		// ASKED OF THE PLAN, NOT THE NETWORK, because this struct is world-free - the mark
		// rides on FRouteStep::bReverseLeg, copied there by the search.
		//
		// AT THE SPAN'S START, not one frame late: the follower has not moved yet this frame,
		// so the handover happens before any forward motion is committed along it.
		if (Phase == EAgentPhase::Taxiing)
		{
			int32 From = INDEX_NONE;
			int32 To = INDEX_NONE;
			for (int32 Step = 0; Step < Follower.Plan.Steps.Num(); ++Step)
			{
				if (!Follower.Plan.Steps[Step].bReverseLeg)
				{
					continue;
				}
				const double SpanStart =
					Step == 0 ? 0.0 : Follower.Plan.Steps[Step - 1].EndDistance;
				if (SpanStart + UE_DOUBLE_KINDA_SMALL_NUMBER < Follower.Travelled)
				{
					// Behind us: a span already driven, this frame or on an earlier one.
					continue;
				}
				From = Step;
				To = Step;

				// THE WHOLE CONTIGUOUS RUN, because a reverse leg is several edges and arming
				// them one at a time would stop and restart the manoeuvre at every vertex.
				while (Follower.Plan.Steps.IsValidIndex(To + 1)
					&& Follower.Plan.Steps[To + 1].bReverseLeg)
				{
					++To;
				}
				break;
			}

			if (From != INDEX_NONE)
			{
				const double SpanStart =
					From == 0 ? 0.0 : Follower.Plan.Steps[From - 1].EndDistance;
				if (Follower.Travelled + UE_DOUBLE_KINDA_SMALL_NUMBER >= SpanStart)
				{
					const FRoutePlan Span = RouteSearch::Section(Follower.Plan, From, To);
					if (Reverse.Start(Span, Airframe, ReverseSpeed))
					{
						// WHERE THE TAXI PICKS UP, read before the phase changes because
						// Follower.Plan is what it is read from.
						ResumeStep = Follower.Plan.Steps.IsValidIndex(To + 1) ? To + 1 : INDEX_NONE;
						Phase = EAgentPhase::Reversing;

						// ZEROED for the reason the Parked branch zeroes it: DescribeMotion
						// reads Follower.Speed for GroundSpeed in every phase that has no
						// speed of its own, and a reversing vehicle reporting its taxi speed
						// is the small version of a pose that disagrees with its phase.
						Follower.Speed = 0.0;

						// ONE WHEELBASE IN, because that is where the FIXED axle already is.
						//
						// THE TWO PHASES PUT DIFFERENT PARTS OF THE VEHICLE ON THEIR LINE, and
						// nothing said so until a truck jumped in PIE. FRouteFollower walks the
						// STEERED axle along its polyline and then reports the body ORIGIN,
						// trailed back by SteerAxleX (see the last line of its Advance).
						// FReverseRun walks the FIXED axle and reports that point as it stands.
						// For a vehicle whose origin sits on its fixed axle - which the fuel
						// truck's does, SteerAxleX being its wheelbase - those two reports are
						// THE SAME POINT, so nothing has to be converted between them. What has
						// to be right is WHERE ALONG THE LINE the manoeuvre starts.
						//
						// The vehicle is parked with its steered axle on the service point, so
						// its fixed axle is one wheelbase back along the line it came in on -
						// which is the line this span begins on, run the other way. Arming at
						// zero claims the fixed axle sits ON the service point and steps the
						// whole body forward by a wheelbase to suit. Measured at 494.3 uu on a
						// 494 uu wheelbase by AirportOps.Ops.TruckNeverTeleportsOnItsRoundTrip,
						// which is the test that reproduces the REDIRECT the player watched -
						// a truck parked at a service point being handed its route home.
						Reverse.Travelled = FMath::Min(Airframe.Wheelbase(), Span.Length);

						// POSED ON THE ARMING FRAME, not on the next one, and this is the same
						// rule UGroundTraffic follows at dispatch: a zero-second Advance asks
						// where the manoeuvre starts without moving it. Reporting the taxi's
						// heading for one frame and the reverse's on the next is a 180 degree
						// snap in the view - a smaller copy of the very bug being fixed, and it
						// showed up as exactly that the first time this ran.
						FVector2D BackAt = At;
						double BackHeading = Heading;
						Reverse.Advance(0.0, Airframe, StopWithin, BackAt, BackHeading);
						LastMotion = DescribeMotion(BackAt, BackHeading);
						OutMotion = LastMotion;
						UE_LOG(LogAirsideTraffic, Log,
							TEXT("Backing out: %.0f uu at %.0f uu/s."),
							Span.Length, ReverseSpeed);
						return true;
					}

					// REFUSED, AND SAID SO. FReverseRun::Start declines a curve this airframe
					// cannot hold backwards and logs the radius; taxiing forwards along it is
					// the crab this whole piece exists to delete, so the vehicle stops instead
					// and the stall shows up as itself.
					UE_LOG(LogAirsideTraffic, Warning,
						TEXT("Reverse leg refused - %s cannot back along it. Stopping rather "
						     "than driving it forwards."),
						Airframe.HasAxles() ? TEXT("this vehicle") : TEXT("an unmeasured vehicle"));
					Follower.Speed = 0.0;
					OutMotion = LastMotion;
					return true;
				}
			}
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

	case EAgentPhase::Reversing:
	{
		// PLAYED BACK, NOT TRACKED. See FReverseRun's header for why reversing is solved once
		// and walked rather than steered: a heading error going backwards GROWS.
		FVector2D BackAt = At;
		double BackHeading = Heading;
		if (Reverse.Advance(DeltaSeconds, Airframe, StopWithin, BackAt, BackHeading))
		{
			LastMotion = DescribeMotion(BackAt, BackHeading);
			OutMotion = LastMotion;
			return true;
		}

		// BACKED OUT: pick the taxi up on what is LEFT of the route.
		//
		// THE REMAINDER IS CUT, not seeked to. FRouteFollower::Start takes an InitialSpeed and
		// no Travelled - it always starts a plan at its beginning - so resuming means handing
		// it a plan that begins where the vehicle is. Passing a distance in that slot is what
		// put the truck back at the service point driving the reverse arm forwards.
		//
		// THE HEADING CARRIES ACROSS UNCHANGED, which is the whole point of the four-leg cycle.
		// The reverse leg ends with the body already facing the way the depart leg leaves, so
		// the follower starts with ZERO heading error and the vehicle simply drives away. That
		// is the same handover the push makes into its taxi out, and for the same reason.
		//
		// SPEED ZERO: this vehicle has been moving backwards and is about to move forwards.
		//
		// A COPY OF THE PLAN, because Follower.Start assigns to Follower.Plan and passing a
		// member into a function that overwrites it is how a self-assignment bug looks.
		const FRoutePlan Continue = Follower.Plan;
		const FRoutePlan Remainder = Continue.Steps.IsValidIndex(ResumeStep)
			? RouteSearch::Section(Continue, ResumeStep, Continue.Steps.Num() - 1)
			: FRoutePlan();
		ResumeStep = INDEX_NONE;

		if (!Remainder.IsValid())
		{
			// THE REVERSE WAS THE LAST THING THE ROUTE DID. Nothing left to drive, so this is
			// the same arrival the follower reports at the end of a taxi - taken here because
			// the follower never ran on this stretch and so will never report it itself.
			Phase = EAgentPhase::Parked;
			OutEvent = EAgentEvent::Parked;
			ShutdownCountdown = ShutdownPause;
			Follower.Speed = 0.0;
			OutMotion = LastMotion;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Backed out; nothing further to drive."));
			return true;
		}

		Phase = EAgentPhase::Taxiing;
		Follower.Start(Remainder, Airframe, 0.0, LastMotion.Heading);

		// AND ONE WHEELBASE IN, because the two phases measure DIFFERENT AXLES along their
		// polyline and this is where that bites. FReverseRun tracks the FIXED axle - it is what
		// a reversing body pivots about, which is the whole reason the struct exists - and
		// FRouteFollower tracks the STEERED one. So at the handover the fixed axle is at the
		// span's end and the steered axle is already a wheelbase along what comes next; a
		// follower started at zero puts it back at the end instead, and the body snaps forward.
		// Measured at 495 uu on a 494 uu wheelbase, which is how the convention was found.
		//
		// SET AFTER Start RATHER THAN PASSED IN, because Start's third parameter is InitialSpeed
		// and it has no Travelled - the same fact that put the truck back at the service point
		// an hour ago. Travelled is a cursor Start zeroes and nothing else in Start reads, so
		// moving it afterwards is the whole of the correction.
		//
		// CLAMPED, so a remainder shorter than the vehicle cannot seek past its own end.
		Follower.Travelled = FMath::Min(Airframe.Wheelbase(), Remainder.Length);

		UE_LOG(LogAirsideTraffic, Log, TEXT("Backed out; driving on (%.0f uu left)."),
			Remainder.Length);

		// AND DRIVE THIS SAME FRAME rather than returning, exactly as the push's handover does:
		// the reverse declined this frame without moving, so the frame's dt is the follower's,
		// and a frame with no motion at all is the step the handover-continuity test catches.
		FVector2D OnAt = At;
		double OnHeading = LastMotion.Heading;
		if (Follower.Advance(DeltaSeconds, Airframe, StopWithin, OnAt, OnHeading))
		{
			LastMotion = DescribeMotion(OnAt, OnHeading);
		}
		OutMotion = LastMotion;
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
