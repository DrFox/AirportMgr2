#include "CoreMinimal.h"
#include "Entities/AircraftType.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"

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
	FAirframe Airframe;
	Airframe.Ground = UAircraftType::PiperMeridianGround();
	Airframe.Climb = UAircraftType::PiperMeridianClimb();
	Airframe.Approach = UAircraftType::PiperMeridianApproach();
	Airframe.Engine = UAircraftType::PiperMeridianEngine();

	// DISTINCTIVE, not authored: 1000 is both the Piper's own Taxi.SpeedCap and what a
	// default-constructed FGroundPerformance carries, so leaving the figure alone could not
	// tell "the follower got the airframe's ground performance" apart from "the follower
	// never got it and is still running on the struct default" - the exact defect issue #27
	// was. 1234 belongs to neither.
	Airframe.Ground.Taxi.SpeedCap = 1234.0;

	const double Needed = FLandingRun::RequiredLandingDistance(
		Airframe.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
	const double RunwayLength = Needed * 1.5;

	const FVector2D Threshold(0.0, 0.0);
	const FVector2D Direction(1.0, 0.0);
	const double VacateAt = RunwayLength * 0.5;

	const FRoutePlan TaxiIn = StraightPlan(
		FVector2D(VacateAt, -1000.0), FVector2D(VacateAt, -50000.0));

	FRoadAgent Agent;
	if (!TestTrue(TEXT("a runway this long accepts the arrival"),
		Agent.StartArrival(Threshold, Direction, RunwayLength, Airframe, VacateAt, TaxiIn)))
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
	while (Agent.Phase == EAgentPhase::Arriving && Ticks < MaxTicks)
	{
		Agent.Advance(Step, Motion);
		++Ticks;
	}

	if (!TestEqual(TEXT("the arrival hands over to taxiing within the bounded loop"),
		Agent.Phase, EAgentPhase::Taxiing))
	{
		return false;
	}

	// THE MEASUREMENT. Issue #27 was the VACATED handover reading Follower.Ground instead
	// of the airframe's - which FAirframe now makes structurally impossible, because
	// FRoadAgent::Advance starts the follower from Airframe.Ground and nothing else ever
	// writes to it.
	TestEqual(TEXT("the follower taxis on the SAME ground performance the arrival was flown "
		"with, not the struct default it would otherwise start with"),
		Agent.Follower.Ground.Taxi.SpeedCap, 1234.0);

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
	Airframe.Ground = UAircraftType::PiperMeridianGround();
	Airframe.Climb = UAircraftType::PiperMeridianClimb();

	if (!TestTrue(TEXT("the Meridian has take-off and climb performance to arm a departure"),
		Airframe.Ground.Takeoff.IsSet() && Airframe.Climb.IsSet()))
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
	Agent.ArmDeparture(Threshold, Direction, RunwayLength);

	TestEqual(TEXT("arming a departure does not itself change the phase - the taxi still "
		"has to arrive"), Agent.Phase, EAgentPhase::Taxiing);

	constexpr double Step = 1.0 / 60.0;
	constexpr int32 MaxTicks = 25000; // ~417 s: Airside.Model.TakeoffRun clears within 300 s.
	int32 Ticks = 0;
	bool bSawDeparting = false;
	bool bCleared = false;
	FAgentMotion Motion;

	while (Ticks < MaxTicks)
	{
		const bool bContinuing = Agent.Advance(Step, Motion);
		++Ticks;

		if (Agent.Phase == EAgentPhase::Departing)
		{
			bSawDeparting = true;
		}

		if (!bContinuing)
		{
			bCleared = true;
			break;
		}
	}

	TestTrue(TEXT("the taxi arriving with a departure armed enters the Departing phase"),
		bSawDeparting);

	if (!TestTrue(TEXT("the departure eventually clears, within the bounded loop"), bCleared))
	{
		return false;
	}

	TestEqual(TEXT("Advance returning false leaves the agent in the Gone phase"),
		Agent.Phase, EAgentPhase::Gone);

	// FALSE EXACTLY FROM GONE ON - not one frame early (which would drop the view while
	// still airborne) and not one frame late (which would leak an agent nothing is driving).
	TestFalse(TEXT("Advance keeps declining once the agent is Gone"),
		Agent.Advance(Step, Motion));

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
	Airframe.Ground = UAircraftType::PiperMeridianGround();

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

	while (Agent.Phase == EAgentPhase::Taxiing && Ticks < MaxTicks)
	{
		Agent.Advance(Step, Motion);
		++Ticks;
	}

	if (!TestEqual(TEXT("a taxi that arrives with no departure armed parks instead"),
		Agent.Phase, EAgentPhase::Parked))
	{
		return false;
	}

	TestTrue(TEXT("the engine is still running the instant it parks - the chocks are not "
		"in yet"), Agent.bEngineRunning);

	// Tick once more, now safely inside the Parked branch: the follower is never advanced
	// again once parked, so without zeroing its Speed on entry, GroundSpeed would keep
	// reporting whatever the last taxiing tick left it at, for ever - a parked aircraft
	// that claims to still be rolling.
	Agent.Advance(Step, Motion);
	TestEqual(TEXT("GroundSpeed reads zero once parked, not the follower's stale taxi speed"),
		Motion.GroundSpeed, 0.0);

	// Advance to just short of the pause: still running. Elapsed starts at Step, not zero -
	// the GroundSpeed check above already spent one tick of the countdown.
	double Elapsed = Step;
	while (Elapsed < Pause - Step)
	{
		Agent.Advance(Step, Motion);
		Elapsed += Step;
	}

	TestTrue(TEXT("the engine is still running with the pause not yet elapsed"),
		Agent.bEngineRunning);

	// Cross the threshold.
	Ticks = 0;
	while (Agent.bEngineRunning && Ticks < 600)
	{
		Agent.Advance(Step, Motion);
		++Ticks;
	}

	TestFalse(TEXT("the engine has stopped once the shutdown pause has elapsed"),
		Agent.bEngineRunning);
	TestEqual(TEXT("the aircraft stays parked, not despawned, once shut down"),
		Agent.Phase, EAgentPhase::Parked);

	return true;
}

#endif
