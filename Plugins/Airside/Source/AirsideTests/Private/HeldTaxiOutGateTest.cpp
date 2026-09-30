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

	/** Arrivals only, or mixed - straight onto the network, which (unlike the facade) bumps no revision. */
	void SetUse(const FGateField& F, ERunwayUse Use)
	{
		FRunwayFacts Facts = F.Net->RunwayFactsFor(F.Runway);
		Facts.Use = Use;
		F.Net->SetRunwayFacts(F.Runway, Facts);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutAsksOncePerEditTest, "Airside.Model.Traffic.HeldTaxiOut.AsksOncePerEdit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutAsksOncePerEditTest::RunTest(const FString&)
{
	using namespace HeldTaxiOutGateTest;
	const FGateField F = Build();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	// PARKED FACING SOUTH (taxied in from B), so the departure pushes back onto E and taxis out E -> J -> B.
	const int32 Id = Traffic->DispatchAgent(F.Net, TestGraph::Probe(*F.Net, F.B, F.A, ETraversalClass::Aircraft),
		TestAirframes::Piper(), ETraversalClass::Aircraft, /*ShutdownPauseSeconds*/ 0.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	for (int32 I = 0; I < 20000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I) { Traffic->Advance(1.0 / 30.0, F.Net); }
	if (!TestEqual(TEXT("departs by pushing back"), Traffic->DepartAgent(Id, *F.Net), EDepartureRefusal::None)) { return false; }
	auto PastJ = [&]()
	{
		const FRoadAgent* Agent = Traffic->FindAgent(Id);
		return Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing && Agent->LastMotion.Position.X < 1000.0
			&& Agent->LastMotion.Position.Y > -9000.0;
	};
	for (int32 Tick = 0; Tick < 30 * 300 && !PastJ(); ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	if (!TestTrue(TEXT("taxiing out, past the junction"), PastJ())) { return false; }

	// NOWHERE TO GO: the only runway takes arrivals only, and the route under the wheels is stranded - so it holds,
	// and every plan from J is refused (NoDepartureRunway).
	SetUse(F, ERunwayUse::ArrivalsOnly);
	if (!TestTrue(TEXT("stranded"), FGroundTrafficTestAccess(*Traffic).Strand(Id))) { return false; }
	const int32 Before = Traffic->TaxiOutReplanAttemptsForTest();
	for (int32 Tick = 0; Tick < 100; ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	if (!TestTrue(TEXT("it holds for a way out"), Traffic->FindAgent(Id) != nullptr && Traffic->FindAgent(Id)->IsHoldingForTaxiOut()))
	{
		return false;
	}
	TestEqual(TEXT("100 quiet substeps: it asked once, not every substep"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 1);

	// THE PLAYER'S FIX: the mode back to mixed, and a line drawn - the guideline revision moves, as every facade edit
	// moves it (a Topology rebuild). Asked again on the next substep, once, and it goes.
	SetUse(F, ERunwayUse::Mixed);
	TestGraph::Node(*F.Net, 90000.0, -90000.0);
	Traffic->Advance(1.0 / 30.0, F.Net);
	TestEqual(TEXT("the edit: asked again, once"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 2);
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	TestTrue(TEXT("and it has a way out: no longer holding"), Agent != nullptr && !Agent->IsHoldingForTaxiOut());
	TestTrue(TEXT("it is taxiing"), Agent != nullptr && Agent->Phase == EAgentPhase::Taxiing);
	for (int32 Tick = 0; Tick < 10; ++Tick) { Traffic->Advance(1.0 / 30.0, F.Net); }
	TestEqual(TEXT("and asks nothing more once it goes"), Traffic->TaxiOutReplanAttemptsForTest() - Before, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHeldTaxiOutBusyRunwayTest, "Airside.Model.Traffic.HeldTaxiOut.BusyRunwayIsNoRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FHeldTaxiOutBusyRunwayTest::RunTest(const FString&)
{
	// WHY THE GATE KEYS ON THE GRAPH ALONE (PR E, 2026-09-30): a held runway RANKS a departure's choice of runway, it
	// never refuses one - so a hold can never be waiting on a runway to free, and RunwayFreedCount is no input of its
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

#endif
