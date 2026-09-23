#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"
#include "Model/PushbackRun.h"
#include "Solve/GuidelineGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// Prefixed against the UNITY build - these test files share one translation unit.

	/** Straight two-point plan, built by hand rather than through RouteSearch - see the
	 *  brief for FRoadAgent's own tests: the agent's handovers do not need a real graph. */
	FRoutePlan StraightPlan(const FVector2D& From, const FVector2D& To)
	{
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline = { From, To };
		Plan.Length = (To - From).Size();
		return Plan;
	}
}

// ---------------------------------------------------------------------------------------
// (a) ARRIVING -> TAXIING. The handover this issue exists to make world-free-testable: it
// used to be assertable only through Airside.Present.ArrivalDispatch, which needs a whole
// world, an actor and a bounded Tick loop to reach one line of behaviour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAgentArrivalHandoverTest,
	"Airside.Model.RoadAgent.ArrivalHandover",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAgentArrivalHandoverTest::RunTest(const FString& Parameters)
{
	FAirframe Airframe = TestAirframes::Piper();

	// DISTINCTIVE, not authored: 1000 is both the Piper's own Taxi.SpeedCap and what a
	// default-constructed FGroundPerformance carries, so leaving the figure alone could not
	// tell "the follower got the airframe's ground performance" apart from "the follower
	// never got it and is still running on the struct default" - the exact defect issue #27
	// was. 1234 belongs to neither.
	Airframe.Chassis.Ground.Taxi.SpeedCap = 1234.0;

	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
	const double RunwayLength = Needed * 1.5;

	FRunwayEnd End;
	End.Threshold = FVector2D(0.0, 0.0);
	End.Direction = FVector2D(1.0, 0.0);
	End.Length = RunwayLength;
	const double VacateAt = RunwayLength * 0.5;

	const FRoutePlan TaxiIn = StraightPlan(
		FVector2D(VacateAt, -1000.0), FVector2D(VacateAt, -50000.0));

	FRoadAgent Agent;
	if (!TestTrue(TEXT("a runway this long accepts the arrival"),
		Agent.StartArrival(End, Airframe, VacateAt, TaxiIn)))
	{
		return false;
	}

	TestEqual(TEXT("StartArrival puts the agent in the Arriving phase"),
		Agent.Phase, EAgentPhase::Arriving);

	// Ticked in a BOUNDED LOOP rather than a fixed frame count, for the same reason
	// Airside.Present.ArrivalDispatch was: a landing plus taxi is dozens of simulated
	// seconds, and hard-coding that would make the test as fragile as the numbers it
	// exercises.
	constexpr double Step = 0.1;
	constexpr int32 MaxTicks = 6000;
	int32 Ticks = 0;
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	while (Agent.Phase == EAgentPhase::Arriving && Ticks < MaxTicks)
	{
		Agent.Advance(Step, Motion, Event);
		++Ticks;
	}

	if (!TestEqual(TEXT("the arrival hands over to taxiing within the bounded loop"),
		Agent.Phase, EAgentPhase::Taxiing))
	{
		return false;
	}

	// THE SEAM ISSUE #105 ITEM 6 ADDS: the exact Advance call that made the handover reports
	// it as an event, which is what UGroundTraffic::AdvanceOnce now switches on instead of
	// diffing Phase itself.
	TestEqual(TEXT("the handover tick reports EAgentEvent::Vacated"), Event, EAgentEvent::Vacated);

	// THE MEASUREMENT. Issue #27 was the VACATED handover reading Follower.Ground instead of
	// the airframe's; issue #83 went further and removed FRouteFollower's own Ground copy
	// entirely, so there is no longer a stale field to read AT ALL - every Advance takes
	// Agent.Airframe fresh. What is left to prove is that the figure actually driving the
	// taxi is 1234, not the Piper's own 1000 or a default-constructed FGroundPerformance's -
	// so this keeps ticking and watches the SPEED the follower actually reaches.
	double MaxTaxiSpeed = 0.0;
	Ticks = 0;
	while (Agent.Phase == EAgentPhase::Taxiing && Ticks < MaxTicks)
	{
		Agent.Advance(Step, Motion, Event);
		MaxTaxiSpeed = FMath::Max(MaxTaxiSpeed, Motion.GroundSpeed);
		++Ticks;
	}

	TestTrue(TEXT("the follower taxis at the SAME ground performance the arrival was flown "
		"with (1234), not the struct default (1000) it would otherwise be capped at"),
		MaxTaxiSpeed > 1100.0);

	return true;
}

// ---------------------------------------------------------------------------------------
// (b) TAXIING -> DEPARTING -> GONE, when a departure is armed for the taxi's destination.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAgentDepartureHandoverTest,
	"Airside.Model.RoadAgent.DepartureHandover",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAgentDepartureHandoverTest::RunTest(const FString& Parameters)
{
	FAirframe Airframe;
	Airframe.Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Airframe.Climb = TestAirframes::Piper().Climb;

	if (!TestTrue(TEXT("the Meridian has take-off and climb performance to arm a departure"),
		Airframe.Chassis.Ground.Takeoff.IsSet() && Airframe.Climb.IsSet()))
	{
		return false;
	}

	// A runway pointing due east, long enough for a Meridian with room to spare - the same
	// figure Airside.Model.TakeoffRun uses.
	constexpr double RunwayLength = 100000.0;
	const FVector2D Threshold(0.0, 0.0);
	const FVector2D Direction(1.0, 0.0);

	const FRoutePlan Plan = StraightPlan(FVector2D(-20000.0, 0.0), Threshold);

	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);
	FRunwayEnd End;
	End.Threshold = Threshold;
	End.Direction = Direction;
	End.Length = RunwayLength;
	Agent.ArmDeparture(End);

	TestEqual(TEXT("arming a departure does not itself change the phase - the taxi still "
		"has to arrive"), Agent.Phase, EAgentPhase::Taxiing);

	constexpr double Step = 1.0 / 60.0;
	constexpr int32 MaxTicks = 25000; // ~417 s: Airside.Model.TakeoffRun clears within 300 s.
	int32 Ticks = 0;
	bool bSawDeparting = false;
	bool bCleared = false;
	bool bSawLinedUp = false;
	bool bSawAirborne = false;
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	EAgentEvent LastEvent = EAgentEvent::None;

	while (Ticks < MaxTicks)
	{
		const bool bContinuing = Agent.Advance(Step, Motion, Event);
		++Ticks;
		LastEvent = Event;

		if (Agent.Phase == EAgentPhase::Departing)
		{
			bSawDeparting = true;
		}
		bSawLinedUp |= (Event == EAgentEvent::LinedUp);
		bSawAirborne |= (Event == EAgentEvent::Airborne);

		if (!bContinuing)
		{
			bCleared = true;
			break;
		}
	}

	TestTrue(TEXT("the taxi arriving with a departure armed enters the Departing phase"),
		bSawDeparting);

	// THE SEAM ISSUE #105 ITEM 6 ADDS: the exact ticks UGroundTraffic::AdvanceOnce now
	// switches on instead of diffing Phase (LinedUp) or a takeoff sub-phase (Airborne) itself.
	TestTrue(TEXT("the Taxiing -> Departing tick reports EAgentEvent::LinedUp"), bSawLinedUp);
	TestTrue(TEXT("reaching the climb reports EAgentEvent::Airborne, though it is not a "
		"phase change (still Departing before and after)"), bSawAirborne);

	if (!TestTrue(TEXT("the departure eventually clears, within the bounded loop"), bCleared))
	{
		return false;
	}

	TestEqual(TEXT("Advance returning false leaves the agent in the Gone phase"),
		Agent.Phase, EAgentPhase::Gone);
	TestEqual(TEXT("and reports EAgentEvent::Gone on that same call"), LastEvent, EAgentEvent::Gone);

	// FALSE EXACTLY FROM GONE ON - not one frame early (which would drop the view while
	// still airborne) and not one frame late (which would leak an agent nothing is driving).
	TestFalse(TEXT("Advance keeps declining once the agent is Gone"),
		Agent.Advance(Step, Motion, Event));

	return true;
}

// ---------------------------------------------------------------------------------------
// (c) TAXIING -> PARKED, when the taxi ends with no departure armed: the engine keeps
// running through the post-arrival pause and stops exactly once it elapses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAgentParkedHandoverTest,
	"Airside.Model.RoadAgent.ParkedHandover",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAgentParkedHandoverTest::RunTest(const FString& Parameters)
{
	FAirframe Airframe;
	Airframe.Chassis.Ground = TestAirframes::Piper().Chassis.Ground;

	const FRoutePlan Plan = StraightPlan(FVector2D(0.0, 0.0), FVector2D(50000.0, 0.0));

	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);

	// DISTINCTIVE, not the 10 s default on ARoadNetworkActor::ShutdownPauseSeconds - proves
	// the countdown actually reads this field rather than a hard-coded figure copied from
	// the old Tick.
	constexpr double Pause = 4.0;
	Agent.ShutdownPause = Pause;

	constexpr double Step = 1.0 / 60.0;
	constexpr int32 MaxTicks = 10000;
	int32 Ticks = 0;
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;

	while (Agent.Phase == EAgentPhase::Taxiing && Ticks < MaxTicks)
	{
		Agent.Advance(Step, Motion, Event);
		++Ticks;
	}

	if (!TestEqual(TEXT("a taxi that arrives with no departure armed parks instead"),
		Agent.Phase, EAgentPhase::Parked))
	{
		return false;
	}

	// THE SEAM ISSUE #105 ITEM 6 ADDS: UGroundTraffic::AdvanceOnce's stand-claim switches on
	// this instead of diffing Phase itself.
	TestEqual(TEXT("the handover tick reports EAgentEvent::Parked"), Event, EAgentEvent::Parked);

	TestTrue(TEXT("the engine is still running the instant it parks - the chocks are not "
		"in yet"), Agent.bEngineRunning);

	// Tick once more, now safely inside the Parked branch: the follower is never advanced
	// again once parked, so without zeroing its Speed on entry, GroundSpeed would keep
	// reporting whatever the last taxiing tick left it at, for ever - a parked aircraft
	// that claims to still be rolling.
	Agent.Advance(Step, Motion, Event);
	TestEqual(TEXT("GroundSpeed reads zero once parked, not the follower's stale taxi speed"),
		Motion.GroundSpeed, 0.0);

	// Advance to just short of the pause: still running. Elapsed starts at Step, not zero -
	// the GroundSpeed check above already spent one tick of the countdown.
	double Elapsed = Step;
	while (Elapsed < Pause - Step)
	{
		Agent.Advance(Step, Motion, Event);
		Elapsed += Step;
	}

	TestTrue(TEXT("the engine is still running with the pause not yet elapsed"),
		Agent.bEngineRunning);

	// Cross the threshold.
	Ticks = 0;
	while (Agent.bEngineRunning && Ticks < 600)
	{
		Agent.Advance(Step, Motion, Event);
		++Ticks;
	}

	TestFalse(TEXT("the engine has stopped once the shutdown pause has elapsed"),
		Agent.bEngineRunning);
	TestEqual(TEXT("the aircraft stays parked, not despawned, once shut down"),
		Agent.Phase, EAgentPhase::Parked);

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #82: FRoadAgent's own invariant-preserving methods, called directly rather than
// through a whole claim pass - each one fails if the method is unwired or writes only half
// of the pair it promises to keep together.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAgentInvariantMethodsTest,
	"Airside.Model.RoadAgent.InvariantMethods",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAgentInvariantMethodsTest::RunTest(const FString& Parameters)
{
	FRoadAgent Agent;

	// BeginCrossing: CrossingRunway and CrossingPhase move together. Read back through the
	// getters, since issue #82 also made both fields private.
	FRoadSegmentId Seed;
	Seed.Index = 3;
	Agent.BeginCrossing(Seed, ECrossingPhase::Committed);
	TestTrue(TEXT("BeginCrossing arms IsCrossing()"), Agent.IsCrossing());
	TestEqual(TEXT("BeginCrossing sets the phase asked for"),
		Agent.GetCrossingPhase(), ECrossingPhase::Committed);
	TestTrue(TEXT("BeginCrossing sets the seed asked for"),
		Agent.GetCrossingRunway() == Seed);

	// Advancing a crossing (Committed -> OnStrip) is the same call with the same seed - not a
	// bare phase write - so the seed survives the transition too.
	Agent.BeginCrossing(Seed, ECrossingPhase::OnStrip);
	TestEqual(TEXT("BeginCrossing again advances the phase"),
		Agent.GetCrossingPhase(), ECrossingPhase::OnStrip);
	TestTrue(TEXT("and keeps the same seed"), Agent.GetCrossingRunway() == Seed);

	// EndCrossing: both fields go back to unset/None together.
	Agent.EndCrossing();
	TestFalse(TEXT("EndCrossing clears IsCrossing()"), Agent.IsCrossing());
	TestEqual(TEXT("EndCrossing clears the phase"), Agent.GetCrossingPhase(), ECrossingPhase::None);
	TestFalse(TEXT("EndCrossing clears the seed"), Agent.GetCrossingRunway().IsSet());

	// Refuse: the quadruple ApplyClaims used to write field by field (issue #174) - one call
	// sets all four, read back through the getters, since this is the same "private field,
	// public accessor" discipline BeginCrossing/EndCrossing established above.
	FGuidelineNodeId BlockNode;
	BlockNode.Index = 9;
	const FTrafficResource BlockResource = FTrafficResource::OfNode(BlockNode);
	Agent.Refuse(/*Step*/ 2, BlockResource, /*NewStopWithin*/ 1234.0, /*BlockerId*/ 7);
	TestEqual(TEXT("Refuse sets BlockedStep"), Agent.GetBlockedStep(), 2);
	TestTrue(TEXT("Refuse sets BlockedResource"), Agent.GetBlockedResource() == BlockResource);
	TestEqual(TEXT("Refuse sets StopWithin"), Agent.GetStopWithin(), 1234.0);
	TestEqual(TEXT("Refuse sets WaitingOn"), Agent.GetWaitingOn(), 7);

	// ClearArbitration: StopWithin, WaitingOn and BlockedStep reset; LastOverlaps is
	// deliberately NOT touched - see the declaration - so a caller that just computed it
	// this pass is not stomped by calling this afterwards.
	Agent.LastOverlaps = { 9 };
	Agent.ClearArbitration();
	TestEqual(TEXT("ClearArbitration resets StopWithin to unbounded"),
		Agent.GetStopWithin(), TNumericLimits<double>::Max());
	TestEqual(TEXT("ClearArbitration resets WaitingOn"), Agent.GetWaitingOn(), 0);
	TestEqual(TEXT("ClearArbitration resets BlockedStep"), Agent.GetBlockedStep(), INDEX_NONE);
	TestEqual(TEXT("ClearArbitration leaves LastOverlaps alone"), Agent.LastOverlaps.Num(), 1);

	// SetGoalFrom: the goal follows a plan's own last step.
	FRoutePlan Plan = StraightPlan(FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0));
	FRouteStep Step;
	Step.To.Index = 5;
	Plan.Steps.Add(Step);
	Agent.SetGoalFrom(Plan);
	TestTrue(TEXT("SetGoalFrom takes the last step's To"), Agent.GoalNode == Step.To);

	// An empty plan clears the goal rather than leaving a stale one - the ternary's other arm.
	FRoutePlan Empty;
	Agent.SetGoalFrom(Empty);
	TestFalse(TEXT("SetGoalFrom clears the goal when the plan has no steps"), Agent.GoalNode.IsSet());

	// SetGoal: a caller that already has the node, not a plan to take it from.
	FGuidelineNodeId Direct;
	Direct.Index = 11;
	Agent.SetGoal(Direct);
	TestTrue(TEXT("SetGoal takes the node directly"), Agent.GoalNode == Direct);

	// HoldRunway/ReleaseRunway: issue #174 - GroundTraffic.cpp used to assign RunwayHeld by
	// hand at every one of these call sites.
	FRoadSegmentId Strip;
	Strip.Index = 4;
	Agent.HoldRunway({ Strip });
	TestEqual(TEXT("HoldRunway holds the chain given"), Agent.RunwayHeld.Num(), 1);
	TestTrue(TEXT("HoldRunway holds the segment given"), Agent.RunwayHeld[0] == Strip);
	Agent.ReleaseRunway();
	TestEqual(TEXT("ReleaseRunway clears the chain"), Agent.RunwayHeld.Num(), 0);

	// AccrueStall/ResetStall: AdvanceOnce's own ternary used to write StalledSeconds by hand.
	Agent.AccrueStall(2.5);
	Agent.AccrueStall(1.5);
	TestEqual(TEXT("AccrueStall adds to the clock"), Agent.StalledSeconds, 4.0);
	Agent.ResetStall();
	TestEqual(TEXT("ResetStall zeroes the clock"), Agent.StalledSeconds, 0.0);

	// SetAwaitingStand/ClearAwaitingStand: "bAwaitingStand implies GoalNode set" is the
	// invariant the review named as maintained only by convention - SetAwaitingStand takes
	// the goal as a parameter so a caller cannot arm the wait without saying what it is
	// waiting from.
	FGuidelineNodeId WaitFrom;
	WaitFrom.Index = 13;
	Agent.SetAwaitingStand(WaitFrom);
	TestTrue(TEXT("SetAwaitingStand arms the wait"), Agent.bAwaitingStand);
	TestTrue(TEXT("SetAwaitingStand sets the goal it waits from"), Agent.GoalNode == WaitFrom);
	Agent.ClearAwaitingStand();
	TestFalse(TEXT("ClearAwaitingStand ends the wait"), Agent.bAwaitingStand);
	TestTrue(TEXT("ClearAwaitingStand leaves the goal alone"), Agent.GoalNode == WaitFrom);

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #83: FLandingRun, FTakeoffRun and FRouteFollower no longer keep their own copy of
// the airframe - Start and Advance take FRoadAgent::Airframe BY REFERENCE. The seam this
// test is FOR: a copy taken once at Start (the bug being fixed) would pass every other
// test in this file, because none of them change Airframe after dispatch. This one does,
// mid-roll, with no re-Start - which only a genuine by-reference read can reflect.
//
// NOT THE FOLLOWER, deliberately: FRouteFollower plans a speed PROFILE once at Start (see
// its header - "PLANS: FSpeedProfile works out what the whole route permits before the
// first frame") and that cache is untouched by a later change to Airframe by design, so
// measuring the taxi cap there would prove nothing either way. FTakeoffRun has no such
// cache - Roll reads Ground.Takeoff.Accel fresh every frame - which is exactly the case
// the "by reference, not copied" fix is for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadAgentAirframeByReferenceTest,
	"Airside.Model.RoadAgent.AirframeByReference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadAgentAirframeByReferenceTest::RunTest(const FString& Parameters)
{
	FAirframe Airframe;
	Airframe.Chassis.Ground = TestAirframes::Piper().Chassis.Ground;
	Airframe.Climb = TestAirframes::Piper().Climb;

	constexpr double RunwayLength = 100000.0;
	const FVector2D Threshold(0.0, 0.0);
	const FVector2D Direction(1.0, 0.0);
	const FRoutePlan Plan = StraightPlan(FVector2D(-20000.0, 0.0), Threshold);

	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);
	FRunwayEnd End;
	End.Threshold = Threshold;
	End.Direction = Direction;
	End.Length = RunwayLength;
	Agent.ArmDeparture(End);

	constexpr double Step = 1.0 / 60.0;
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;

	// Tick to the middle of the ROLL - past the line-up, still well short of Vr - so there is
	// runway left to measure a changed acceleration over.
	int32 Ticks = 0;
	while (Ticks < 25000
		&& !(Agent.Phase == EAgentPhase::Departing && Agent.Departure.Phase == ETakeoffPhase::Roll
			&& Agent.Departure.Speed > Airframe.Chassis.Ground.Takeoff.SpeedCap * 0.3))
	{
		Agent.Advance(Step, Motion, Event);
		++Ticks;
	}

	if (!TestTrue(TEXT("reaches the middle of the roll within the bounded loop"),
		Agent.Phase == EAgentPhase::Departing && Agent.Departure.Phase == ETakeoffPhase::Roll))
	{
		return false;
	}

	// CUT, NOT RE-ARMED: no Departure.Start, no new agent - the same FTakeoffRun that has
	// been rolling since ArmDeparture. A copy taken at Start would keep accelerating at the
	// Piper's own figure regardless of this.
	Agent.Airframe.Chassis.Ground.Takeoff.Accel = 0.01;

	constexpr int32 MeasureTicks = 30; // half a second
	const double BeforeSpeed = Agent.Departure.Speed;
	for (int32 Tick = 0; Tick < MeasureTicks; ++Tick)
	{
		Agent.Advance(Step, Motion, Event);
	}
	const double Gained = Agent.Departure.Speed - BeforeSpeed;

	TestTrue(FString::Printf(
		TEXT("a near-zero accel written to Airframe takes effect with no re-arm, proving ")
		TEXT("Advance reads it live (gained %.2f uu/s over %.1f s, was accelerating at %.0f uu/s2)"),
		Gained, MeasureTicks * Step, Agent.Airframe.Chassis.Ground.Takeoff.Accel),
		Gained < 1.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAgentPushbackHandoverTest,
	"Airside.Model.AgentPushbackHandover",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAgentPushbackHandoverTest::RunTest(const FString& Parameters)
{
	// A stand square to its taxiway: 40 m of lead-in out along +X, then the taxiway along +Y.
	// The aeroplane parked facing -X, into the terminal, which is what makes the lead-in run
	// away from it. Built by hand for the reason the other tests in this file give: the
	// agent's handovers do not need a real graph.
	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	// DELIBERATELY NOT AT THE WORLD ORIGIN. The origin is what this project's recurring
	// "posed at (0,0)" failure looks like, so a fixture that parks the aeroplane there could
	// never tell the failure from the fixture. Offset, and the check below means something.
	//
	// THE PUSH ROUTE, not the departure's: the stand at (10000, 5000) facing north, its
	// lead-in running SOUTH, and then the EAST arm of the junction - the one the taxi out does
	// not use. See PushbackPlanner.
	Plan.Polyline = { {10000.0, 5000.0}, {10000.0, 1000.0}, {18000.0, 1000.0} };
	Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);
	FRouteStep LeadIn;
	LeadIn.EndDistance = 4000.0;
	FRouteStep Taxiway;
	Taxiway.EndDistance = Plan.Length;
	Plan.Steps = { LeadIn, Taxiway };

	// AND THE TAXI OUT FROM WHERE THE PUSH ENDS - west along the same taxiway, which is the
	// direction the push leaves the aeroplane facing. Planned at dispatch in production; here
	// it is the second half of the fixture.
	FRoutePlan TaxiOut;
	TaxiOut.Result = ERouteResult::Found;
	TaxiOut.Polyline = { {18000.0, 1000.0}, {2000.0, 1000.0} };
	TaxiOut.Length = GuidelineGeom::PolylineLength(TaxiOut.Polyline);
	FRouteStep Away;
	Away.EndDistance = TaxiOut.Length;
	TaxiOut.Steps = { Away };

	// Facing NORTH on the stand, into the terminal, because the lead-in runs south.
	const double ParkedHeading = UE_DOUBLE_HALF_PI;

	FAirframe Airframe = TestAirframes::Piper();
	Airframe.PushbackNeed = EPushbackNeed::VehicleTug;

	FRoadAgent Agent;
	Agent.Phase = EAgentPhase::Parked;
	if (!TestTrue(TEXT("a parked aeroplane can be pushed"),
		Agent.StartPushback(Plan, TaxiOut, Airframe, 150.0, 30.0, 0.0)))
	{
		return false;
	}

	TestEqual(TEXT("it is manoeuvring"), Agent.Phase, EAgentPhase::Manoeuvring);

	// PUSH AND START: the engine comes alive as the manoeuvre begins, FROM COLD, and spools
	// while the tug pushes. Starting it at full RPM would be an aeroplane that was shut down
	// one frame and at governed speed the next.
	TestTrue(TEXT("the engine is running"), Agent.bEngineRunning);
	TestEqual(TEXT("from cold"), Agent.EngineRPM, 0.0, 0.0001);

	// AND IT IS POSED AT THE STAND, facing the way it parked, before any Advance at all -
	// never at the world origin, and never facing out along the line it is standing on.
	TestEqual(TEXT("posed at the stand from the start"),
		Agent.GroundPosition().Y, 5000.0, 0.01);
	TestEqual(TEXT("facing the way it parked, not the way the line points"),
		FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(Agent.LastMotion.Heading - ParkedHeading))), 0.0, 0.01);

	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	bool bPushedBack = false;
	double RPMAtHandover = -1.0;
	double SpeedDuringPush = 0.0;
	bool bEverAtOrigin = false;

	for (int32 Frame = 0; Frame < 20000; ++Frame)
	{
		Agent.Advance(1.0 / 60.0, Motion, Event);

		// THE WHEELS TURN UNDER A PUSH. Recorded rather than asserted per frame so the
		// failure names a number: a zero here is DescribeMotion missing the Manoeuvring arm,
		// which is the "stopped wheels" defect its own comment warns about.
		//
		// ABS, because a push reports a NEGATIVE ground speed since 2026-09-20 - it goes
		// backwards, and the view needs the sign to roll the wheels the right way. What this
		// asks is "did they turn at all", not "which way", and Airside.Model.
		// BackwardsPhasesReportNegativeGroundSpeed is what pins the direction.
		SpeedDuringPush = FMath::Max(SpeedDuringPush, FMath::Abs(Motion.GroundSpeed));

		bEverAtOrigin = bEverAtOrigin || Motion.Position.SizeSquared() < 1.0;

		if (Event == EAgentEvent::PushedBack)
		{
			bPushedBack = true;
			RPMAtHandover = Agent.EngineRPM;
			break;
		}
	}

	if (!TestTrue(TEXT("the push hands over"), bPushedBack))
	{
		return false;
	}

	TestEqual(TEXT("and it is taxiing after it"), Agent.Phase, EAgentPhase::Taxiing);
	TestTrue(TEXT("the wheels turned during the push"), SpeedDuringPush > 0.0);

	// THE SPOOL SURVIVES THE HANDOVER. Going through StartTaxi would write EngineRPM back to
	// zero, which is the whole reason the handover calls Follower.Start directly.
	TestTrue(FString::Printf(TEXT("the propeller kept the RPM it spooled to (%.0f)"), RPMAtHandover),
		RPMAtHandover > 0.0);

	// NOT ASSERTED: that it is STILL spooling at the handover. Whether the spool outlasts the
	// tug is a race between two authored figures - a Piper's SpoolUpSeconds is 4 s and this
	// push takes about fifty - so it is true for a jet and false for a light turboprop, and
	// pinning it here would be pinning the Piper's engine data rather than this handover. The
	// property that matters either way is the one above: the taxi inherits the spool rather
	// than restarting it, which is the whole reason this hands over through Follower.Start.

	// NO FRAME AT THE WORLD ORIGIN - this project's recurring failure, and the reason
	// LastMotion exists at all.
	TestFalse(TEXT("no frame put the aeroplane at the world origin"), bEverAtOrigin);

	// THE DEFECT THIS FEATURE REMOVES, measured at the level of the agent: the follower
	// inherits no heading error, so there is nothing left for it to slew on the spot. The push
	// finished facing the line's tangent turned about, and the taxi out leaves that same point
	// the other way, so the two agree by construction rather than by arithmetic.
	TestEqual(TEXT("the follower inherits the heading the push finished on"),
		FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(Agent.Follower.Heading - Agent.Pushback.Heading))),
		0.0, 0.01);

	// AND IT TURNED 90 DEGREES GETTING THERE, ending facing WEST - the way it will taxi -
	// rather than back out along its own lead-in.
	TestEqual(TEXT("it swung 90 degrees off its parked heading"),
		FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(Agent.Follower.Heading - ParkedHeading))), 90.0, 0.5);
	TestEqual(TEXT("which leaves it facing the way the taxi out goes"),
		FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(Agent.Follower.Heading - UE_DOUBLE_PI))), 0.0, 0.5);

	// A POWERBACK WAITS FOR THRUST, through the agent rather than through FPushbackRun: the
	// gate reads the AGENT's EngineRPM, so a threshold that never rose would hold it for ever.
	{
		FAirframe Light = TestAirframes::Piper();
		Light.PushbackNeed = EPushbackNeed::SelfManoeuvre;

		FRoadAgent Powerback;
		Powerback.Phase = EAgentPhase::Parked;
		Powerback.StartPushback(Plan, TaxiOut, Light, 200.0, 30.0,
			Light.Engine.MaxRPM * 0.6);

		FAgentMotion PowerMotion;
		EAgentEvent PowerEvent = EAgentEvent::None;
		Powerback.Advance(1.0 / 60.0, PowerMotion, PowerEvent);
		TestEqual(TEXT("a powerback has not moved on frame one - the propeller is still cold"),
			Powerback.Pushback.Travelled, 0.0, 0.0001);

		// It does move once the engine has spooled past the fraction, which is what makes
		// this a delay rather than a deadlock.
		for (int32 Frame = 0; Frame < 2000 && Powerback.Pushback.Travelled <= 0.0; ++Frame)
		{
			Powerback.Advance(1.0 / 60.0, PowerMotion, PowerEvent);
		}
		TestTrue(TEXT("and moves once it has thrust"), Powerback.Pushback.Travelled > 0.0);
		TestTrue(TEXT("by which time the propeller is turning"), Powerback.EngineRPM > 0.0);
	}

	return true;
}

#endif
