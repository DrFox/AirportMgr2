#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Airframe.h"
#include "Model/JobBoard.h"
#include "Model/ServiceText.h"
#include "Model/OpsDefinition.h"
#include "Present/OpsRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The aircraft card's fuel line (2026-09-28): the load, what is left - counting down WHILE the
 * pump runs, not only at the end of a trip - and where the job has got to.
 */
namespace
{
	FServiceJob& Demand(UJobBoard& Service, EServiceJobState State, double Owed, double Delivered, int32 Trips)
	{
		FServiceJob& D = Service.AddJobForTest(1, State, EServiceRefusal::None, 0);
		D.QuantityOwed = Owed;
		D.QuantityDelivered = Delivered;
		D.Trips = Trips;
		D.TankLitres = 1000.0;
		return D;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribePumpingTest, "AirportOps.Fuel.Describe.CountsDownWhilePumping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribePumpingTest::RunTest(const FString& Parameters)
{
	UJobBoard* Service = NewObject<UJobBoard>();
	FServiceJob& D = Demand(*Service, EServiceJobState::Serving, 2900.0, 0.0, 0);
	D.TripQuantity = 1000.0;
	D.TripStartedAt = 0.0;
	D.TripEndsAt = 800.0;
	TestEqual(TEXT("halfway through the first trip, 500 L are in"),
		Service->DescribeAgent(1, 400.0, nullptr), FString(TEXT("Fuel 2,900 L · 2,400 L left · fuelling (trip 1 of 3)")));
	TestEqual(TEXT("a pump that has not moved has not delivered"),
		Service->DescribeAgent(1, 0.0, nullptr), FString(TEXT("Fuel 2,900 L · 2,900 L left · fuelling (trip 1 of 3)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribeStatesTest, "AirportOps.Fuel.Describe.EveryState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribeStatesTest::RunTest(const FString& Parameters)
{
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Underway, 1900.0, 1000.0, 1);
		TestEqual(TEXT("second trip on its way"), Service->DescribeAgent(1, 0.0, nullptr),
			FString(TEXT("Fuel 2,900 L · 1,900 L left · truck en route (trip 2 of 3)")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Open, 1900.0, 1000.0, 1);
		TestEqual(TEXT("between trips it waits for a truck"), Service->DescribeAgent(1, 0.0, nullptr),
			FString(TEXT("Fuel 2,900 L · 1,900 L left · waiting for a truck (trip 2 of 3)")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Open, 300.0, 0.0, 0);
		TestEqual(TEXT("a one-trip job names no trips"), Service->DescribeAgent(1, 0.0, nullptr),
			FString(TEXT("Fuel 300 L · 300 L left · waiting for a truck")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		Demand(*Service, EServiceJobState::Done, 0.0, 2900.0, 3);
		TestEqual(TEXT("done, in how many trips"), Service->DescribeAgent(1, 0.0, nullptr),
			FString(TEXT("Fuel 2,900 L · done in 3 trips")));
	}
	// NO "a type with no tank" CASE (#462, T8): a zero-litre aircraft has a turnaround and NO job, so a job whose litres total zero never
	// exists to be described; AirportOps.Fuel.NoLitresNoTruck pins the line the aircraft really reads.
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		FServiceJob& D = Demand(*Service, EServiceJobState::Unserviceable, 2900.0, 0.0, 0);
		D.Why = EServiceRefusal::NoDepot;
		TestEqual(TEXT("the refusal, as before"), Service->DescribeAgent(1, 0.0, nullptr),
			FString(TEXT("Fuel 2,900 L · no fuel depot")));
	}
	{
		UJobBoard* Service = NewObject<UJobBoard>();
		TestEqual(TEXT("no demand, no line"), Service->DescribeAgent(1, 0.0, nullptr), FString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribeVehicleTest, "AirportOps.Fuel.Describe.Vehicle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribeVehicleTest::RunTest(const FString& Parameters)
{
	// THE TRUCK'S OWN CARD (stage 3): selecting a vehicle's agent shows what the VEHICLE is doing -
	// the state it owns, what it carries, how much work is queued behind the current job. Through the
	// same DescribeAgent the aircraft card uses, so the inspector needs no second seam.
	UJobBoard* Service = NewObject<UJobBoard>();
	// THE GAME'S CATALOGUE, so the card names the kind the shop sold (#430) - it printed the raw code.
	UOpsRuntime::ResolveVehicleCatalogue(*Service, *GetDefault<UScenario>());
	FEntityInstanceId Depot;
	Depot.Index = 4;
	FServiceJob& Job = Service->AddJobForTest(1, EServiceJobState::Underway, EServiceRefusal::None, 0);
	Job.Stand.Index = 2;
	const int32 JobId = Job.Id;
	FServiceVehicle& Vehicle = Service->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::ToJob, 9700.0);
	Vehicle.AgentId = 7;
	Vehicle.CurrentJob = JobId;
	Vehicle.Queue = { 99 };
	TestEqual(TEXT("driving to a job"), Service->DescribeAgent(7, 0.0, nullptr),
		FString(TEXT("Bowser · to stand 2 · 9,700 L · 1 queued")));

	Vehicle.State = EServiceVehicleState::Serving;
	Vehicle.Queue.Reset();
	TestEqual(TEXT("serving, nothing behind it"), Service->DescribeAgent(7, 0.0, nullptr),
		FString(TEXT("Bowser · fuelling at stand 2 · 9,700 L")));

	Vehicle.State = EServiceVehicleState::ToFacility;
	Vehicle.CurrentJob = 0;
	TestEqual(TEXT("going home"), Service->DescribeAgent(7, 0.0, nullptr),
		FString(TEXT("Bowser · to depot 4 · 9,700 L")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribeDepotBacklogTest, "AirportOps.Fuel.Describe.DepotBacklog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribeDepotBacklogTest::RunTest(const FString& Parameters)
{
	// HOW FAR BEHIND THE DEPOT IS (user, 2026-09-28): the jobs on its vehicles, when it will have
	// cleared them - the latest promise - and how many of those promises land after the aircraft's
	// turnaround ends, i.e. make an aircraft wait. The bowser has a job under way and one queued that
	// will be four minutes late; the tow is idle.
	UJobBoard* Service = NewObject<UJobBoard>();
	// THE GAME'S CATALOGUE, so each vehicle's line names its kind as the shop sold it (#430) - it printed the raw code.
	UOpsRuntime::ResolveVehicleCatalogue(*Service, *GetDefault<UScenario>());
	FEntityInstanceId Depot;
	Depot.Index = 1;
	const double Now = 1000.0;

	int32 OnTime = 0;
	int32 Late = 0;
	{
		FServiceJob& Job = Service->AddJobForTest(1, EServiceJobState::Underway, EServiceRefusal::None, 0);
		Job.Stand.Index = 3;
		Job.QuantityOwed = 300.0;
		Job.PromisedFinish = Now + 360.0;
		OnTime = Job.Id;
	}
	{
		FServiceJob& Job = Service->AddJobForTest(2, EServiceJobState::Queued, EServiceRefusal::None, 0);
		Job.Stand.Index = 5;
		Job.QuantityOwed = 1200.0;
		Job.PromisedFinish = Now + 1260.0;
		Late = Job.Id;
	}
	Service->AddTurnaroundForTest(1, Now + 3600.0, OnTime);
	Service->AddTurnaroundForTest(2, Now + 1020.0, Late);

	const int32 BowserId = [&]
	{
		FServiceVehicle& Bowser = Service->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::ToJob, 9700.0);
		Bowser.AgentId = 7;
		Bowser.CurrentJob = OnTime;
		Bowser.Queue = { Late };
		return Bowser.Id;
	}();
	const int32 TowId = Service->AddVehicleForTest(TEXT("UTILITY"), Depot, EServiceVehicleState::Idle, 1000.0).Id;
	for (const int32 JobId : { OnTime, Late })
	{
		const_cast<FServiceJob*>(Service->GetJobs().FindByPredicate([JobId](const FServiceJob& J) { return J.Id == JobId; }))->VehicleId = BowserId;
	}

	const FDepotBacklog Backlog = Service->DescribeDepot(Depot, Now, nullptr);
	TestEqual(TEXT("the summary: how many, when it clears, how many late"), Backlog.Summary,
		FString(TEXT("2 jobs · clears in 21 min · 1 late")));
	TestEqual(TEXT("and where the backlog sits: each vehicle, then its jobs in order"), Backlog.Detail,
		FString::Printf(TEXT("Bowser #%d · to stand 3 · 9,700 L\n  stand 3 · 300 L · +6 min\n  stand 5 · 1,200 L · +21 min · late 4 min\nUtility tow #%d · at depot 1 · 1,000 L"),
			BowserId, TowId));
	TestEqual(TEXT("the counts behind the text"), Backlog.Jobs, 2);
	TestEqual(TEXT("one late"), Backlog.LateJobs, 1);

	// A DEPOT WITH A VEHICLE AND NOTHING TO DO says so; one with NO VEHICLE says what to do about it (#447: the card used to lay that over the
	// summary in its own wording, beside RefusalText's - the board owns both sentences now).
	FEntityInstanceId Idle;
	Idle.Index = 8;
	Service->AddVehicleForTest(TEXT("UTILITY"), Idle, EServiceVehicleState::Idle, 1000.0);
	TestEqual(TEXT("a depot with a vehicle and nothing to do says so"), Service->DescribeDepot(Idle, Now, nullptr).Summary, FString(TEXT("No jobs")));
	FEntityInstanceId Empty;
	Empty.Index = 9;
	TestEqual(TEXT("a depot with no vehicle says to buy one"), Service->DescribeDepot(Empty, Now, nullptr).Summary, FString(TEXT("No vehicles \u2014 buy one")));
	TestEqual(TEXT("and lists nothing under it"), Service->DescribeDepot(Empty, Now, nullptr).Detail, FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDescribeDepotSpansTest, "AirportOps.Fuel.Describe.DepotSpansUseTheClocksWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDescribeDepotSpansTest::RunTest(const FString& Parameters)
{
	// ONE WORDING OF A SPAN (#447): the depot card said "+95 min" beside the aircraft card's "1 h 35 min" for the same span. Both go through
	// GameTimeText::Duration now, so a backlog longer than an hour reads as the clock does.
	UJobBoard* Service = NewObject<UJobBoard>();
	UOpsRuntime::ResolveVehicleCatalogue(*Service, *GetDefault<UScenario>());
	FEntityInstanceId Depot;
	Depot.Index = 1;
	const double Now = 1000.0;
	FServiceJob& Job = Service->AddJobForTest(1, EServiceJobState::Underway, EServiceRefusal::None, 0);
	Job.Stand.Index = 3;
	Job.QuantityOwed = 300.0;
	Job.PromisedFinish = Now + 95.0 * 60.0;
	const int32 JobId = Job.Id;
	Service->AddTurnaroundForTest(1, Now + 60.0 * 60.0, JobId);   // the aircraft leaves at +60 min: 35 min late
	FServiceVehicle& Bowser = Service->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::ToJob, 9700.0);
	Bowser.CurrentJob = JobId;

	const FDepotBacklog Backlog = Service->DescribeDepot(Depot, Now, nullptr);
	TestTrue(*FString::Printf(TEXT("the job's promise in the clock's words: '%s'"), *Backlog.Detail), Backlog.Detail.Contains(TEXT("+1 h 35 min")));
	TestTrue(TEXT("and its lateness"), Backlog.Detail.Contains(TEXT("late 35 min")));
	TestEqual(TEXT("and the summary's"), Backlog.Summary, FString(TEXT("1 job \u00B7 clears in 1 h 35 min \u00B7 1 late")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FServiceTextBoardForwardsTest, "AirportOps.Model.ServiceText.BoardForwardsEveryLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FServiceTextBoardForwardsTest::RunTest(const FString& Parameters)
{
	// #427 moved every vehicle, job and depot text into the ServiceText namespace and left UJobBoard's names - which the
	// inspector, the shop and the alerts call - as forwarders. Each forwarder against what it forwards to, on a board with a
	// vehicle out on a job and a turnaround behind it, and each answer checked non-empty first, so a forwarder that answered
	// anything else (nothing, the other overload, another line) fails here by its name.
	UJobBoard* Board = NewObject<UJobBoard>();
	UOpsRuntime::ResolveVehicleCatalogue(*Board, *GetDefault<UScenario>());
	FEntityInstanceId Depot;
	Depot.Index = 1;
	const double Now = 1000.0;
	FServiceJob& Job = Board->AddJobForTest(1, EServiceJobState::Serving, EServiceRefusal::None, 0);
	Job.Stand.Index = 3;
	Job.QuantityOwed = 2900.0;
	Job.TripQuantity = 1000.0;
	Job.TripStartedAt = Now - 100.0;
	Job.TripEndsAt = Now + 700.0;
	Job.TankLitres = 1000.0;
	Job.PromisedFinish = Now + 600.0;
	const int32 JobId = Job.Id;
	Board->AddTurnaroundForTest(1, Now + 300.0, JobId);
	FServiceVehicle& Bowser = Board->AddVehicleForTest(TEXT("FUEL"), Depot, EServiceVehicleState::Serving, 9700.0);
	Bowser.AgentId = 7;
	Bowser.CurrentJob = JobId;
	const int32 BowserId = Bowser.Id;
	const_cast<FServiceJob*>(Board->FindJob(JobId))->VehicleId = BowserId;

	// THE AIRCRAFT'S FUEL LINE, both overloads, and the clock flag the second one carries.
	bool bForwarded = false;
	bool bDirect = false;
	const FString Line = ServiceText::DescribeAgent(*Board, 1, Now, bDirect, nullptr);
	if (!TestFalse(TEXT("the aircraft has a fuel line to forward"), Line.IsEmpty())) { return false; }
	TestEqual(TEXT("DescribeAgent forwards the aircraft's line"), Board->DescribeAgent(1, Now, nullptr), Line);
	TestEqual(TEXT("its flagged overload forwards the same line"), Board->DescribeAgent(1, Now, bForwarded, nullptr), Line);
	TestTrue(TEXT("pumping, the line moves with the clock"), bDirect);
	TestEqual(TEXT("and the overload forwards the flag"), bForwarded, bDirect);

	// THE VEHICLE'S OWN LINE, through the same seam.
	bool bUnused = false;
	const FString VehicleText = ServiceText::DescribeAgent(*Board, 7, Now, bUnused, nullptr);
	if (!TestFalse(TEXT("the vehicle has a line to forward"), VehicleText.IsEmpty())) { return false; }
	TestEqual(TEXT("DescribeAgent forwards the vehicle's line"), Board->DescribeAgent(7, Now, nullptr), VehicleText);
	const FServiceVehicle* Found = Board->FindVehicle(BowserId);
	if (!TestNotNull(TEXT("the bowser is on the board"), Found)) { return false; }
	TestEqual(TEXT("VehicleLine forwards"), Board->VehicleLine(*Found, nullptr), ServiceText::VehicleLine(*Board, *Found, nullptr));

	// THE DEPOT CARD.
	const FDepotBacklog Direct = ServiceText::DescribeDepot(*Board, Depot, Now, nullptr);
	const FDepotBacklog Forwarded = Board->DescribeDepot(Depot, Now, nullptr);
	if (!TestEqual(TEXT("the depot has its one job"), Direct.Jobs, 1)) { return false; }
	TestEqual(TEXT("DescribeDepot forwards the summary"), Forwarded.Summary, Direct.Summary);
	TestEqual(TEXT("and the detail"), Forwarded.Detail, Direct.Detail);
	TestEqual(TEXT("and the late count"), Forwarded.LateJobs, Direct.LateJobs);

	// EVERY REFUSAL'S WORDS.
	for (int32 Why = 0; Why <= static_cast<int32>(EServiceRefusal::UnknownVehicleKind); ++Why)
	{
		const EServiceRefusal Refusal = static_cast<EServiceRefusal>(Why);
		TestEqual(*FString::Printf(TEXT("RefusalText forwards refusal %d"), Why),
			FString(UJobBoard::RefusalText(Refusal)), FString(ServiceText::RefusalText(Refusal)));
	}

	// THE DEADLINE'S TURNAROUND HALF (#494 review): UJobBoard::NextDeadline is the vehicles' timed steps AND FTurnarounds::NextDeadline,
	// and #427 moved the second behind a forwarding call that nothing pinned - dropped, the board would book no wake-up for a
	// turnaround's end, and an aircraft whose service never finished would sit at its stand until some other event came. The bowser
	// here is Serving with no step booked (StepEndsAt 0, not after Now), so the turnaround staged at +300 s is the only deadline on
	// the board: the answer is that, or the forwarding is gone. Mutation-checked 2026-10-01: the Turnarounds.NextDeadline call
	// dropped from UJobBoard::NextDeadline, this went red (the board answered "none", Max double).
	TestEqual(TEXT("NextDeadline includes the staged turnaround's end - the turnarounds' half forwards"), Board->NextDeadline(Now), Now + 300.0);
	return true;
}

#endif
