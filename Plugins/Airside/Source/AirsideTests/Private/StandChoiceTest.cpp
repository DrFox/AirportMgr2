#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTest,
	"Airside.Model.ArrivalPlanner.SkipsHeldStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTest::RunTest(const FString& Parameters)
{
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	const FGuidelineNodeId PoseA = A.Pose(A.Stands[0]);
	const FGuidelineNodeId PoseB = A.Pose(A.Stands[1]);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }
	const FAirframe Piper = TestAirframes::Piper();

	// Planner level: with the first choice held by agent 7, the plan goes to the other; with
	// both held, NoFreeStand.
	FTrafficOccupancy Occ;
	const FArrivalPlan Free = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with both free"), Free.IsValid())) { return false; }
	const FGuidelineNodeId First = Free.TaxiIn.Steps.Last().To;

	auto Hold = [&](FGuidelineNodeId Node, int32 Agent)
	{
		FTrafficClaim C; C.AgentId = Agent; C.Resource = FTrafficResource::OfNode(Node); C.Rank = 10;
		FTrafficClaim B; Occ.TryClaim(C, B);
	};
	Hold(First, 7);
	const FArrivalPlan Other = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with one held"), Other.IsValid())) { return false; }
	TestTrue(TEXT("and goes to the OTHER stand"), Other.TaxiIn.Steps.Last().To != First);
	Hold(Other.TaxiIn.Steps.Last().To, 8);
	const FArrivalPlan None = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	TestEqual(TEXT("both held is NoFreeStand"), None.Why, EArrivalRefusal::NoFreeStand);
	TestTrue(TEXT("and it has words"), !ArrivalPlanner::DescribeRefusal(None).IsEmpty());

	// Unreachable is still NoRouteToStand: remove both stands.
	A.Net->RemoveEntity(A.Stands[0]);
	A.Net->RemoveEntity(A.Stands[1]);
	TestGraph::Rebuild(*A.Net);
	TestEqual(TEXT("no stands at all is NoRouteToStand, not NoFreeStand"),
		ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ).Why, EArrivalRefusal::NoRouteToStand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTwoArrivalsTest,
	"Airside.Model.Traffic.TwoArrivalsTwoStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTwoArrivalsTest::RunTest(const FString& Parameters)
{
	// THE REPORT: "I called in 2 aircraft, they both went to the same stand." Through the
	// model, end to end: the second is dispatched once the first has vacated the runway (the
	// runway claim would refuse it earlier, for its own reason), and gets the other stand.
	// A third, once the second has vacated too, is refused for want of a stand.
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = TestAirframes::Piper();

	const int32 First = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("first dispatched"), First > 0)) { return false; }
	// UNTIL THE RUNWAY IS CLEAR, not merely until Taxiing: an aircraft that has just vacated
	// still holds the strip under the crossing rule until its tail is off it, and a landing
	// asked in that window is refused as RunwayOccupied - its own reason, not this test's.
	auto RunwayFree = [&]()
	{
		for (const FTrafficClaim& C : Traffic->GetOccupancy().GetClaims())
		{
			if (C.Resource.Kind == ETrafficResourceKind::Surface) { return false; }
		}
		return true;
	};
	if (!TestTrue(TEXT("first vacates and clears the runway"), RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(First); return P && P->Phase == EAgentPhase::Taxiing && RunwayFree(); }))) { return false; }

	TArray<EArrivalRefusal> Refusals;
	Traffic->OnArrivalRefused.AddLambda([&](EArrivalRefusal Why) { Refusals.Add(Why); });
	const int32 Second = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(FString::Printf(TEXT("second dispatched (refusals: %d)"), Refusals.Num()), Second > 0)) { return false; }
	TestTrue(TEXT("two aircraft, two stands"),
		Traffic->FindAgent(First)->GoalNode != Traffic->FindAgent(Second)->GoalNode);

	if (!TestTrue(TEXT("second vacates and clears the runway"), RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Second); return P && P->Phase == EAgentPhase::Taxiing && RunwayFree(); }))) { return false; }
	const int32 Third = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	TestEqual(TEXT("a third is refused"), Third, 0);
	TestTrue(TEXT("for want of a free stand"), Refusals.Num() > 0 && Refusals.Last() == EArrivalRefusal::NoFreeStand);
	return true;
}

#endif
