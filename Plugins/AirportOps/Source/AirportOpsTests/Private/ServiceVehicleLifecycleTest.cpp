#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/ServiceVehicle.h"
#include "Model/ServiceVehicleLifecycle.h"

#if WITH_DEV_AUTOMATION_TESTS

// FServiceVehicleLifecycle is Model/ - plain data and a counter, so these are world-free: no board, no traffic. What
// the BOARD does with each transition (which one it picks, when) is FuelServiceTest's, on its fixture.

namespace ServiceVehicleLifecycleTest
{
	constexpr int32 Agent = 7;
	constexpr int32 Job = 3;

	FServiceVehicle Fresh()
	{
		FEntityInstanceId Home;
		Home.Index = 2;
		Home.Generation = 1;
		return FServiceVehicleLifecycle::Create(5, TEXT("FUEL"), EServiceRole::Fuel, Home, 9000.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceVehicleLifecycleCreateTest, "AirportOps.Model.Lifecycle.CreateIsIdleAtHomeAndFull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceVehicleLifecycleCreateTest::RunTest(const FString& Parameters)
{
	// THE ONE CREATION SITE: the seeded and the bought vehicle used to be two hand copies, and a field one of them
	// forgot is a vehicle that is born wrong. Born here, it is in the one state that needs nothing else set.
	const FServiceVehicle Born = ServiceVehicleLifecycleTest::Fresh();
	TestEqual(TEXT("it keeps the id it was given"), Born.Id, 5);
	TestEqual(TEXT("its kind"), Born.TypeCode, FName(TEXT("FUEL")));
	TestEqual(TEXT("its home"), Born.Home.Index, 2);
	TestEqual(TEXT("and the cargo it was filled with"), Born.Cargo, 9000.0, 1e-9);
	TestEqual(TEXT("it is Idle - at home, not on the road"), Born.State, EServiceVehicleState::Idle);
	TestEqual(TEXT("with no agent"), Born.AgentId, 0);
	TestEqual(TEXT("no job"), Born.CurrentJob, 0);
	TestEqual(TEXT("and nothing queued"), Born.Queue.Num(), 0);
	TestTrue(TEXT("and that is a row of the invariant table"), FServiceVehicleLifecycle::Violation(Born, /*bSettled=*/true).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceVehicleLifecycleCycleTest, "AirportOps.Model.Lifecycle.WalksTheServiceCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceVehicleLifecycleCycleTest::RunTest(const FString& Parameters)
{
	// THE WHOLE CYCLE, stepped: home, out to a job, serve, decide, home, refill, home again - each transition
	// leaving a row of the table, and each moving the board's FleetRevision, which is what wakes the re-bid. The
	// revision is asserted as "moved" per step, not as a count: how many bumps a transition owes is its business.
	FServiceVehicle Vehicle = ServiceVehicleLifecycleTest::Fresh();
	uint32 Revision = 0;
	uint32 Seen = Revision;
	auto Step = [&](const TCHAR* Name, EServiceVehicleState Expected)
	{
		TestEqual(FString::Printf(TEXT("%s: the state it enters"), Name), Vehicle.State, Expected);
		TestTrue(FString::Printf(TEXT("%s: it is a row of the table - %s"), Name, *FServiceVehicleLifecycle::Violation(Vehicle)),
			FServiceVehicleLifecycle::Violation(Vehicle).IsEmpty());
		TestTrue(FString::Printf(TEXT("%s: it moved FleetRevision"), Name), Revision != Seen);
		Seen = Revision;
	};

	FServiceVehicleLifecycle Lifecycle(Vehicle, Revision);
	Lifecycle.Dispatched(ServiceVehicleLifecycleTest::Agent);
	Step(TEXT("Dispatched"), EServiceVehicleState::Deciding);
	TestEqual(TEXT("Dispatched: it drives the agent it was given"), Vehicle.AgentId, ServiceVehicleLifecycleTest::Agent);

	Lifecycle.SetOff(ServiceVehicleLifecycleTest::Job);
	Step(TEXT("SetOff"), EServiceVehicleState::ToJob);
	TestEqual(TEXT("SetOff: it holds the job"), Vehicle.CurrentJob, ServiceVehicleLifecycleTest::Job);

	Lifecycle.BeginServe(120.0);
	Step(TEXT("BeginServe"), EServiceVehicleState::Serving);
	TestEqual(TEXT("BeginServe: the pumping is dated"), Vehicle.StepEndsAt, 120.0, 1e-9);
	TestTrue(TEXT("BeginServe: it is a timed step"), FServiceVehicleLifecycle::IsTimed(Vehicle));

	Lifecycle.EndServe();
	Step(TEXT("EndServe"), EServiceVehicleState::Deciding);
	TestEqual(TEXT("EndServe: the job is let go - never 'Serving' with none"), Vehicle.CurrentJob, 0);
	TestEqual(TEXT("EndServe: it is still parked on its agent"), Vehicle.AgentId, ServiceVehicleLifecycleTest::Agent);
	TestFalse(TEXT("EndServe: no longer a timed step"), FServiceVehicleLifecycle::IsTimed(Vehicle));

	Lifecycle.HeadHome();
	Step(TEXT("HeadHome"), EServiceVehicleState::ToFacility);

	Lifecycle.LeaveRoad();
	Step(TEXT("LeaveRoad"), EServiceVehicleState::Idle);
	TestEqual(TEXT("LeaveRoad: the agent is let go"), Vehicle.AgentId, 0);

	Lifecycle.BeginFacility(300.0);
	Step(TEXT("BeginFacility"), EServiceVehicleState::AtFacility);
	TestEqual(TEXT("BeginFacility: the refill is dated"), Vehicle.StepEndsAt, 300.0, 1e-9);
	TestTrue(TEXT("BeginFacility: it is a timed step"), FServiceVehicleLifecycle::IsTimed(Vehicle));

	Lifecycle.BecomeIdle();
	Step(TEXT("BecomeIdle"), EServiceVehicleState::Idle);
	TestEqual(TEXT("BecomeIdle: the timed step is cleared"), Vehicle.StepEndsAt, 0.0, 1e-9);

	// ALREADY IDLE IS NO TRANSITION: the board calls it for every vehicle with nothing to do, and a revision moved for
	// nothing is a re-bid pass run for nothing.
	Lifecycle.BecomeIdle();
	TestEqual(TEXT("BecomeIdle from Idle changes nothing and moves nothing"), Revision, Seen);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceVehicleLifecycleReleaseTest, "AirportOps.Model.Lifecycle.ReleaseCurrentJobLeavesItDeciding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceVehicleLifecycleReleaseTest::RunTest(const FString& Parameters)
{
	// A JOB TAKEN FROM UNDER A VEHICLE - its aircraft left, a recall, a stranding - is not "the vehicle is done": it is
	// still on the road with an agent and nothing to do, which is Deciding, until the board decides. From ToJob and
	// from Serving alike.
	uint32 Revision = 0;
	for (const bool bServing : { false, true })
	{
		FServiceVehicle Vehicle = ServiceVehicleLifecycleTest::Fresh();
		FServiceVehicleLifecycle Lifecycle(Vehicle, Revision);
		Lifecycle.Dispatched(ServiceVehicleLifecycleTest::Agent);
		Lifecycle.SetOff(ServiceVehicleLifecycleTest::Job);
		if (bServing)
		{
			Lifecycle.BeginServe(50.0);
		}
		Lifecycle.ReleaseCurrentJob();
		const FString Name = bServing ? TEXT("Serving") : TEXT("ToJob");
		TestEqual(*FString::Printf(TEXT("released from %s: Deciding"), *Name), Vehicle.State, EServiceVehicleState::Deciding);
		TestEqual(*FString::Printf(TEXT("released from %s: holds no job"), *Name), Vehicle.CurrentJob, 0);
		TestEqual(*FString::Printf(TEXT("released from %s: still on its agent"), *Name), Vehicle.AgentId, ServiceVehicleLifecycleTest::Agent);
		TestTrue(*FString::Printf(TEXT("released from %s: a row of the table"), *Name), FServiceVehicleLifecycle::Violation(Vehicle).IsEmpty());
	}

	// NOTHING HELD IS NO TRANSITION: the board releases every vehicle a recall or a depot removal touches, most of which
	// hold nothing, and each must stay exactly as it was - including its FleetRevision.
	FServiceVehicle Home = ServiceVehicleLifecycleTest::Fresh();
	uint32 Before = Revision;
	FServiceVehicleLifecycle(Home, Revision).ReleaseCurrentJob();
	TestEqual(TEXT("an Idle vehicle stays Idle"), Home.State, EServiceVehicleState::Idle);
	TestEqual(TEXT("and nothing is bumped for a release that released nothing"), Revision, Before);

	FServiceVehicle Homing = ServiceVehicleLifecycleTest::Fresh();
	{
		FServiceVehicleLifecycle Lifecycle(Homing, Revision);
		Lifecycle.Dispatched(ServiceVehicleLifecycleTest::Agent);
		Lifecycle.HeadHome();
	}
	Before = Revision;
	FServiceVehicleLifecycle(Homing, Revision).ReleaseCurrentJob();
	TestEqual(TEXT("a vehicle already heading home stays ToFacility"), Homing.State, EServiceVehicleState::ToFacility);
	TestEqual(TEXT("with nothing bumped"), Revision, Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceVehicleLifecycleViolationTest, "AirportOps.Model.Lifecycle.EveryStateHasItsInvariant",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceVehicleLifecycleViolationTest::RunTest(const FString& Parameters)
{
	// THE TABLE ON EServiceVehicleState, asserted row by row and column by column: a row that holds is empty, and one
	// that does not names itself. "Serving with no job" is the illegal state issue #428 was filed for; ToFacility with
	// a job and Idle with an agent are its siblings, and none of the three was checked anywhere.
	struct FRow { EServiceVehicleState State; int32 AgentId; int32 CurrentJob; bool bHolds; const TCHAR* Why; };
	const FRow Rows[] = {
		{ EServiceVehicleState::Idle,       0, 0, true,  TEXT("Idle: at home, no agent, no job") },
		{ EServiceVehicleState::Idle,       7, 0, false, TEXT("Idle with an agent: it would be on the road at home") },
		{ EServiceVehicleState::Idle,       0, 3, false, TEXT("Idle with a job: nothing is being done for it") },
		{ EServiceVehicleState::ToJob,      7, 3, true,  TEXT("ToJob: an agent driving to a job") },
		{ EServiceVehicleState::ToJob,      0, 3, false, TEXT("ToJob with no agent: nothing is driving") },
		{ EServiceVehicleState::ToJob,      7, 0, false, TEXT("ToJob with no job: driving to nothing") },
		{ EServiceVehicleState::Serving,    7, 3, true,  TEXT("Serving: parked at the stand with its job") },
		{ EServiceVehicleState::Serving,    7, 0, false, TEXT("Serving with no job - THE WEDGE (issue #428)") },
		{ EServiceVehicleState::Serving,    0, 3, false, TEXT("Serving with no agent: nothing is at the stand") },
		{ EServiceVehicleState::ToFacility, 7, 0, true,  TEXT("ToFacility: heading home, holding no job") },
		{ EServiceVehicleState::ToFacility, 7, 3, false, TEXT("ToFacility with a job: it would be going home with a job it is not doing") },
		{ EServiceVehicleState::ToFacility, 0, 0, false, TEXT("ToFacility with no agent: heading home with nothing to drive") },
		{ EServiceVehicleState::AtFacility, 0, 0, true,  TEXT("AtFacility: at the depot, no agent - a refill is not on the road") },
		{ EServiceVehicleState::AtFacility, 7, 0, false, TEXT("AtFacility with an agent: the spec's reading, the code has never had") },
		{ EServiceVehicleState::Deciding,   7, 0, true,  TEXT("Deciding: on the road with nothing chosen") },
		{ EServiceVehicleState::Deciding,   0, 0, false, TEXT("Deciding with no agent: nothing on the road to decide for") },
		{ EServiceVehicleState::Deciding,   7, 3, false, TEXT("Deciding with a job: it has decided") },
	};
	for (const FRow& Row : Rows)
	{
		FServiceVehicle Vehicle = ServiceVehicleLifecycleTest::Fresh();
		FServiceVehicleLifecycle::SeedStateForTest(Vehicle, Row.State);
		Vehicle.AgentId = Row.AgentId;
		Vehicle.CurrentJob = Row.CurrentJob;
		const FString Reported = FServiceVehicleLifecycle::Violation(Vehicle);
		TestTrue(*FString::Printf(TEXT("%s (reported: '%s')"), Row.Why, *Reported), Reported.IsEmpty() == Row.bHolds);
	}

	// DECIDING ACROSS A STEP is the one violation the between-Steps form adds: the state is legal inside a Step, and
	// a Step that ends with one has left a decision unmade.
	FServiceVehicle Deciding = ServiceVehicleLifecycleTest::Fresh();
	FServiceVehicleLifecycle::SeedStateForTest(Deciding, EServiceVehicleState::Deciding);
	Deciding.AgentId = ServiceVehicleLifecycleTest::Agent;
	TestTrue(TEXT("Deciding holds mid-Step"), FServiceVehicleLifecycle::Violation(Deciding, /*bSettled=*/false).IsEmpty());
	TestFalse(TEXT("but never between Steps"), FServiceVehicleLifecycle::Violation(Deciding, /*bSettled=*/true).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FServiceVehicleLifecycleRestoreTest, "AirportOps.Model.Lifecycle.ResetForRestoreClearsWhatALoadClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FServiceVehicleLifecycleRestoreTest::RunTest(const FString& Parameters)
{
	// A LOAD'S VEHICLE comes back Idle at home with its cargo and nothing else: agents are never saved, and the jobs of
	// aircraft that are not restored are not either. Whatever it was doing - a serve in progress with a queue - goes.
	FServiceVehicle Vehicle = ServiceVehicleLifecycleTest::Fresh();
	uint32 Revision = 0;
	FServiceVehicleLifecycle Lifecycle(Vehicle, Revision);
	Lifecycle.Dispatched(ServiceVehicleLifecycleTest::Agent);
	Lifecycle.SetOff(ServiceVehicleLifecycleTest::Job);
	Lifecycle.BeginServe(90.0);
	Vehicle.Queue = { 8, 9 };

	const uint32 Before = Revision;
	Lifecycle.ResetForRestore();
	TestEqual(TEXT("Idle"), Vehicle.State, EServiceVehicleState::Idle);
	TestEqual(TEXT("no agent"), Vehicle.AgentId, 0);
	TestEqual(TEXT("no job"), Vehicle.CurrentJob, 0);
	TestEqual(TEXT("an empty queue"), Vehicle.Queue.Num(), 0);
	TestEqual(TEXT("no timed step"), Vehicle.StepEndsAt, 0.0, 1e-9);
	TestEqual(TEXT("its cargo is kept - the save keeps it"), Vehicle.Cargo, 9000.0, 1e-9);
	TestTrue(TEXT("and a load moves FleetRevision"), Revision != Before);
	return true;
}

#endif
