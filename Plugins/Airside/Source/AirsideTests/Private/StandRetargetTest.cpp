#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandRetargetTest,
	"Airside.Model.Traffic.StandRetarget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandRetargetTest::RunTest(const FString& Parameters)
{
	// THE SECOND QUESTION OF 2026-09-07: "what happens if the reserved stand gets deleted as
	// the aircraft is coming in to land". Retarget to a free stand; failing that wait, not
	// strand; and go the moment a stand appears.
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = TestAirframes::Piper();

	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	Traffic->Advance(0.05, A.Net);   // one tick: on final
	const FGuidelineNodeId Goal0 = Traffic->FindAgent(Id)->GoalNode;
	const FEntityInstanceId Target = (Goal0 == A.Pose(A.Stands[0])) ? A.Stands[0] : A.Stands[1];
	const FEntityInstanceId Spare = (Target == A.Stands[0]) ? A.Stands[1] : A.Stands[0];

	// 1. DELETE THE STAND IT IS HEADING FOR while it is on final. It retargets to the other.
	A.Net->RemoveEntity(Target);
	TestGraph::Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestEqual(TEXT("still arriving"), P->Phase, EAgentPhase::Arriving);
		TestTrue(TEXT("its goal is now the spare stand"), P->GoalNode == A.Pose(Spare));
		TestFalse(TEXT("and it is not waiting"), P->bAwaitingStand);
		TestTrue(TEXT("the spare stand is held for it, between ticks"),
			Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(P->GoalNode), 0));
	}

	// 2. DELETE THE SPARE TOO. Nothing to retarget to: it waits, and lands anyway (v1).
	A.Net->RemoveEntity(Spare);
	TestGraph::Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestTrue(TEXT("awaiting a stand"), P->bAwaitingStand);
		TestTrue(TEXT("with a live node to wait at"), A.Net->GetGuidelineNode(P->GoalNode) != nullptr);
		TestEqual(TEXT("status says so"), InspectFacts::StatusOf(*P), FString(TEXT("No stand - waiting")));
	}
	if (!TestTrue(TEXT("it lands and stops"), RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked; }))) { return false; }
	TestTrue(TEXT("a parked waiter occupies the node it stopped at"),
		Traffic->GetOccupancy().IsHeld(FTrafficResource::OfNode(Traffic->FindAgent(Id)->GoalNode), 0));

	// 3. BUILD A STAND. The rebuild re-offers it; the aircraft goes.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	// The same spot the fixture's own first stand sat at.
	const FEntityInstanceId NewStand = A.Net->PlaceEntity(Stand, Stand->Anchors, A.ExitAt + FVector2D(9000.0, -10000.0), 0.0);
	TestGraph::Rebuild(*A.Net);
	Traffic->OnGraphRebuilt(*A.Net);
	Traffic->Advance(0.05, A.Net);   // the re-offer runs at the end of a tick
	{
		const FRoadAgent* P = Traffic->FindAgent(Id);
		TestFalse(TEXT("no longer waiting"), P->bAwaitingStand);
		TestTrue(TEXT("heading for the new stand"), P->GoalNode == A.Pose(NewStand));
		TestEqual(TEXT("taxiing again"), P->Phase, EAgentPhase::Taxiing);
	}
	TestTrue(TEXT("and it parks there"), RunUntil(*Traffic, *A.Net, 600.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Id); return P && P->Phase == EAgentPhase::Parked && P->GoalNode == A.Pose(NewStand); }));
	return true;
}

#endif
