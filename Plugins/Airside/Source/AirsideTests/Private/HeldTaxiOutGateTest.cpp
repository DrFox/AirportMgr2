#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * OPS BATCH 3 PR E, the Airside half: a departure holding for a way to the runway used to plan one EVERY SUBSTEP
 * for as long as it held (FindNearestNode + DeparturePlanner::PlanAny), and a stand's held-or-not changed through
 * the claim pass with no counter a poller could key on. Both are counted here.
 */
namespace HeldTaxiOutGateTest
{
	/**
	 * PushbackDepartTest's field (a runway along y 0, a junction J with a far arm E, a stand A south of it),
	 * built again rather than reached across the unity build into that file's anonymous namespace.
	 */
	struct FGateField
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId Runway;
		FGuidelineNodeId A, B, C, J, E;
	};

	FGateField Build()
	{
		FGateField F;
		F.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *F.Net;
		URoadProfile* Runway = TestProfiles::Runway();
		const FRoadNodeId RA = Net.AddNode(FVector2D(-50000.0, 0.0));
		const FRoadNodeId RM = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RB = Net.AddNode(FVector2D(50000.0, 0.0));
		F.Runway = Net.AddStraightSegment(RA, RM, Runway);
		Net.AddStraightSegment(RM, RB, Runway);

		F.B = TestGraph::Node(Net, 0.0, 0.0);
		F.J = TestGraph::Node(Net, 0.0, -10000.0);
		F.A = TestGraph::Node(Net, 0.0, -20000.0);
		F.C = TestGraph::Node(Net, 0.0, -40000.0);
		F.E = TestGraph::Node(Net, 20000.0, -10000.0);
		TestGraph::FJoinOptions Options;
		Options.bDerived = false;
		TestGraph::Join(Net, F.A, F.J, Options);
		TestGraph::Join(Net, F.J, F.B, Options);
		TestGraph::Join(Net, F.J, F.E, Options);
		TestGraph::Join(Net, F.C, F.A, Options);
		return F;
	}

	/**
	 * One aeroplane holding for a way to the runway, with no line to the runway at all: parked facing south (taxied
	 * in from B), departed by pushing back onto E, taxiing out E -> J -> B, then - once past J - its route stranded
	 * and B, the only line onto the runway, deleted. A GRAPH EDIT, not a facts write: the refusal is a genuine NoRoute
	 * and the fix below is a genuine line drawn, so the test moves only what the gate reads (the review's I1 - an
	 * earlier version set the runway's mode straight onto URoadNetwork, the one write that moves no revision).
	 * Returns the agent id, or 0.
	 */
	int32 HoldWithNoWayOut(const FGateField& F, UGroundTraffic& Traffic)
	{
		const int32 Id = Traffic.DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.B, F.A, ETraversalClass::Aircraft),
			TestAirframes::Piper(), ETraversalClass::Aircraft, /*ShutdownPauseSeconds*/ 0.0);
		if (Id <= 0)
		{
			return 0;
		}
		for (int32 I = 0; I < 20000 && Traffic.FindAgent(Id)->Phase != EAgentPhase::Parked; ++I) { Traffic.Advance(1.0 / 30.0, F.Net); }
		if (Traffic.DepartAgent(Id, *F.Net) != EDepartureRefusal::None)
		{
			return 0;
		}
		auto PastJ = [&]()
		{
			const FRoadAgent* Agent = Traffic.FindAgent(Id);
			return Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && Agent->LastMotion.Position.X < 1000.0
				&& Agent->LastMotion.Position.Y > -9000.0;
		};
		for (int32 Tick = 0; Tick < 30 * 300 && !PastJ(); ++Tick) { Traffic.Advance(1.0 / 30.0, F.Net); }
		if (!PastJ() || !FGroundTrafficTestAccess(Traffic).Strand(Id))
		{
			return 0;
		}
		F.Net->RemoveGuidelineNode(F.B);
		// Into the hold: the stranded route ends, the taxi-complete guard holds, and the first replan is refused.
		for (int32 Tick = 0; Tick < 5; ++Tick) { Traffic.Advance(1.0 / 30.0, F.Net); }
		const FRoadAgent* Agent = Traffic.FindAgent(Id);
		return Agent != nullptr && Agent->IsHoldingForTaxiOut() ? Id : 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutAsksOncePerEditTest, "Airside.Model.Traffic.HeldTaxiOut.AsksOncePerEdit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutAsksOncePerEditTest::RunTest(const FString&)
{
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Before = Traffic->TaxiOutReplanAttemptsForTest();
	const int32 Id = HoldWithNoWayOut(F, *Traffic);
	if (!TestTrue(TEXT("it holds for a way out, with no line to the runway"), Id > 0)) { return false; }
	const int32 Refused = Traffic->TaxiOutReplanAttemptsForTest() - Before;
	TestEqual(TEXT("asked once on entering the hold, and refused"), Refused, 1);

	for (int32 Tick = 0; Tick < 100; ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	TestTrue(TEXT("still holding"), Traffic->FindAgent(Id) != nullptr && Traffic->FindAgent(Id)->IsHoldingForTaxiOut());
	TestEqual(TEXT("100 quiet substeps: it asked once, not every substep"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);

	// THE PLAYER'S FIX: a line to the runway again. The guideline revision moves because the graph did - the input
	// the gate reads - and it is asked again on the next substep, once, and goes.
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*F.Net, F.J, TestGraph::Node(*F.Net, 0.0, 0.0), Options);
	Traffic->Advance(1.0 / 30.0, F.Net);
	TestEqual(TEXT("the edit: asked again, once"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 2);
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	TestTrue(TEXT("and it has a way out: no longer holding"), Agent != nullptr && !Agent->IsHoldingForTaxiOut());
	TestTrue(TEXT("it is taxiing"), Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing);
	for (int32 Tick = 0; Tick < 10; ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	TestEqual(TEXT("and asks nothing more once it goes"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutNewNetworkTest, "Airside.Model.Traffic.HeldTaxiOut.ANewNetworkAsksAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutNewNetworkTest::RunTest(const FString&)
{
	// A NEW NETWORK OBJECT AT THE SAME REVISION NUMBER: two deterministic Build()s with the same edit made to each
	// count their guideline revisions identically, so a refusal remembered against the first matches the second by
	// number. Only the gate's network identity tells them apart - without it the hold would never be asked again.
	using namespace HeldTaxiOutGateTest;
	const FGateField First = Build();
	const FGateField Second = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = HoldWithNoWayOut(First, *Traffic);
	if (!TestTrue(TEXT("holding on the first network"), Id > 0)) { return false; }
	Second.Net->RemoveGuidelineNode(Second.B);
	if (!TestEqual(TEXT("the two networks stand at the same guideline revision - the case under test"),
		Second.Net->GetGuidelineRevision(), First.Net->GetGuidelineRevision())) { return false; }

	const int32 Before = Traffic->TaxiOutReplanAttemptsForTest();
	Traffic->Advance(1.0 / 30.0, First.Net);
	TestEqual(TEXT("the same network, nothing moved: not asked"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 0);
	Traffic->Advance(1.0 / 30.0, Second.Net);
	TestEqual(TEXT("the other network, same number: asked again"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);
	Traffic->Advance(1.0 / 30.0, Second.Net);
	TestEqual(TEXT("and then remembered against it"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutBusyRunwayTest, "Airside.Model.Traffic.HeldTaxiOut.BusyRunwayIsNoRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutBusyRunwayTest::RunTest(const FString&)
{
	// WHY THE GATE KEYS ON THE GRAPH ALONE (PR E, 2026-09-30): a held runway RANKS a departure's choice of runway, it
	// never refuses one - so a hold can never be waiting on a runway to free, and no runway-freed signal is an input of its
	// failure. If PlanAny ever starts refusing a busy strip, this goes red, and the gate must take the counter.
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	FTrafficOccupancy Occupancy;
	for (const FTrafficResource& Surface : F.Net->RunwaySurfaces(F.Runway))
	{
		Occupancy.Assert(FTrafficClaim::Make(99, Surface, /*bOccupied*/ true, 2));
	}
	if (!TestTrue(TEXT("the whole strip is held"), Occupancy.IsAnyHeld(F.Net->RunwaySurfaces(F.Runway), 0, false))) { return false; }
	const FDeparturePlan Plan = DeparturePlanner::PlanAny(*F.Net, F.J, TestAirframes::Piper(), ETraversalClass::Aircraft, &Occupancy);
	TestTrue(FString::Printf(TEXT("a held runway is still planned to: %s"), *DeparturePlanner::Describe(Plan)), Plan.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStandHoldsChurnTest, "Airside.Model.Traffic.StandHolds.ChurnIsCounted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FStandHoldsChurnTest::RunTest(const FString&)
{
	// A BODY ON A STAND, put there and taken off by the claim pass - the one change to a stand's holder no revision
	// sees (PR D: "a stand freed by claim churn moves no revision"). The inspector's stand card keys on this count.
	const FTestAirport Field = FTestAirport::Build(TestAirframes::Piper());
	URoadNetwork* Net = Field.Net;
	const FGuidelineNodeId Pose = Net->GetEntity(Field.Stands[0])->PoseNode;
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	Traffic->Advance(0.05, Net);
	const uint32 Changes = Traffic->StandHoldChangeCount();
	const uint32 Revision = Traffic->OccupancyRevision();

	Traffic->OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Pose), /*bOccupied*/ true, 2));
	Traffic->Advance(0.05, Net);
	TestEqual(TEXT("a body arrives on the stand: counted"), Traffic->StandHoldChangeCount() - Changes, 1u);
	Traffic->Advance(0.05, Net);
	TestEqual(TEXT("and it standing there is no further change"), Traffic->StandHoldChangeCount() - Changes, 1u);
	Traffic->OccupancyForTest().ReleaseAll(77);
	Traffic->Advance(0.05, Net);
	TestEqual(TEXT("it leaves: counted"), Traffic->StandHoldChangeCount() - Changes, 2u);
	TestEqual(TEXT("and neither moved OccupancyRevision - which is why the count exists"), Traffic->OccupancyRevision(), Revision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutRestartKeepsPoseAndEngineTest, "Airside.Model.Traffic.HeldTaxiOut.RestartKeepsPoseAndEngine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutRestartKeepsPoseAndEngineTest::RunTest(const FString&)
{
	// ISSUE #429: A TAXIING AEROPLANE THAT HELD FOR A WAY OUT RESTARTS THROUGH THE ROUTE-CHANGE SEAM, where it used to
	// call FRoadAgent::ResumeTaxiOut - RestartTaxi with its own heading, and its own copy of "keep the spool". Its
	// steps survive the move: from rest WHERE it holds, FACING the way it holds, its engine carried on rather than
	// started cold, and the goal moved to the new runway entry. One substep after the player's fix, so the agent has
	// had exactly one substep of motion from rest - a pose from anywhere else, or a propeller wound back to zero,
	// shows as more than a substep's worth.
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Id = HoldWithNoWayOut(F, *Traffic);
	if (!TestTrue(TEXT("it holds for a way out"), Id > 0)) { return false; }
	const FRoadAgent* Held = Traffic->FindAgent(Id);
	const FVector2D HeldAt = Held->LastMotion.Position;
	const double HeldHeading = Held->LastMotion.Heading;
	const FGuidelineNodeId HeldGoal = Held->GoalNode;
	if (!TestTrue(TEXT("precondition: its engine is running at speed while it holds"),
		Held->bEngineRunning && Held->GetEngineRPM() > 0.0)) { return false; }
	const double HeldRPM = Held->GetEngineRPM();

	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*F.Net, F.J, TestGraph::Node(*F.Net, 0.0, 0.0), Options);
	Traffic->Advance(1.0 / 30.0, F.Net);

	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestTrue(TEXT("it restarted: taxiing and no longer holding"),
		Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && !Agent->IsHoldingForTaxiOut())) { return false; }
	TestTrue(FString::Printf(TEXT("from where it held (%.1f uu away after one substep from rest)"),
		FVector2D::Distance(Agent->LastMotion.Position, HeldAt)),
		FVector2D::Distance(Agent->LastMotion.Position, HeldAt) < 20.0);
	TestTrue(FString::Printf(TEXT("facing the way it held (%.1f deg off)"),
		FMath::RadiansToDegrees(FMath::Abs(FMath::UnwindRadians(Agent->LastMotion.Heading - HeldHeading)))),
		FMath::Abs(FMath::UnwindRadians(Agent->LastMotion.Heading - HeldHeading)) < FMath::DegreesToRadians(5.0));
	TestTrue(FString::Printf(TEXT("its engine carried on, not started cold (%.0f RPM, was %.0f)"), Agent->GetEngineRPM(), HeldRPM),
		Agent->bEngineRunning && Agent->GetEngineRPM() >= HeldRPM - 1.0);
	TestTrue(TEXT("and its goal moved to the runway entry the new route ends at"),
		Agent->GoalNode.IsSet() && Agent->GoalNode != HeldGoal
		&& Agent->GoalNode == Agent->Follower.Plan.Steps.Last().To);
	return true;
}

#endif
