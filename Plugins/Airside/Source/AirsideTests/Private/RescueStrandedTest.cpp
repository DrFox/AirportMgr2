#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A plane dispatched A -> B, stopped part-way and Stranded there with the pavement still under it -
	 *  the state a player's Unstick meets (spec 2026-09-29-unstick-agent). Zero on failure. */
	int32 StrandPlaneOn(UGroundTraffic& Traffic, URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		const int32 Plane = Traffic.DispatchAgent(&Net, TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
			TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
		if (Plane <= 0)
		{
			return 0;
		}
		TickUntil(Traffic, Net, 5.0, [](int32) { return true; });
		FGroundTrafficTestAccess(Traffic).Strand(Plane);
		TickUntil(Traffic, Net, 1.0, [](int32) { return true; });
		const FRoadAgent* P = Traffic.FindAgent(Plane);
		return P != nullptr && P->Phase == EAgentPhase::Stranded ? Plane : 0;
	}
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRescueStrandedRejoinsTest,
	"Airside.Model.Traffic.RescueStranded.Rejoins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRescueStrandedRejoinsTest::RunTest(const FString& Parameters)
{
	// A STRANDING WAS FINAL (GroundTrafficRebuild.cpp's Strand): Result Unreachable, and nothing
	// re-resolved it again even when the pavement came back. The player's Replan is the one thing
	// that may put it back on a line - so it must, when there is one right under it, and it must
	// then actually get where it was going rather than restart and strand again.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	TestGraph::Join(*Net, A, B);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = StrandPlaneOn(*Traffic, *Net, A, B);
	if (!TestTrue(TEXT("stranded part-way along A -> B"), Plane > 0)) { return false; }
	const FVector2D WasAt = Traffic->FindAgent(Plane)->LastMotion.Position;

	if (!TestTrue(TEXT("rescued toward its own goal"), Traffic->RescueStranded(Plane, *Net, FGuidelineNodeId()))) { return false; }
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("taxiing again"), P->Phase, EAgentPhase::Taxiing);
	TestTrue(TEXT("from where it stood - no hop, the line is under it"),
		FVector2D::Distance(P->LastMotion.Position, WasAt) < 50.0);

	TickUntil(*Traffic, *Net, 300.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		return Q != nullptr && Q->Phase == EAgentPhase::Taxiing;
	});
	P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("and it parks, not strands again"), P->Phase, EAgentPhase::Parked);
	TestTrue(TEXT("at B, where it was going"), FVector2D::Distance(P->LastMotion.Position, FVector2D(20000.0, 0.0)) < 100.0);
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRescueStrandedRefusesTest,
	"Airside.Model.Traffic.RescueStranded.Refuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRescueStrandedRefusesTest::RunTest(const FString& Parameters)
{
	// TWO REFUSALS, BOTH WITH NOTHING CHANGED: pavement too far away to hop to (the player is
	// left Despawn - a teleport across the airport is not a rescue), and an agent that is not
	// stranded at all (a moving agent's replan is ReplanAt's, which keeps Travelled; this one
	// re-seats the follower).
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	TestGraph::Join(*Net, A, B);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = StrandPlaneOn(*Traffic, *Net, A, B);
	if (!TestTrue(TEXT("stranded"), Plane > 0)) { return false; }
	const FVector2D WasAt = Traffic->FindAgent(Plane)->LastMotion.Position;

	// THE PAVEMENT GOES; a parallel line 50 m away, well past the rescue radius, is all that is left.
	for (int32 I = 0; I < Net->GetGuidelineEdges().Num(); ++I)
	{
		if (Net->GetGuidelineEdges()[I].bAlive) { Net->RemoveGuidelineEdge(Net->GuidelineEdgeIdAt(I)); }
	}
	const FGuidelineNodeId FarA = TestGraph::Node(*Net, 0.0, 5000.0);
	const FGuidelineNodeId FarB = TestGraph::Node(*Net, 20000.0, 5000.0);
	TestGraph::Join(*Net, FarA, FarB);

	TestFalse(TEXT("nothing within the radius: refused"), Traffic->RescueStranded(Plane, *Net, FarB));
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("still stranded"), P->Phase, EAgentPhase::Stranded);
	TestTrue(TEXT("still where it stood"), FVector2D::Distance(P->LastMotion.Position, WasAt) < 1.0);

	// A MOVING AGENT.
	const int32 Mover = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, FarA, FarB, ETraversalClass::Aircraft),
		TestAirframes::GroundOnly(), ETraversalClass::Aircraft, 1.0);
	if (!TestTrue(TEXT("a second, moving aircraft"), Mover > 0)) { return false; }
	TestFalse(TEXT("a taxiing agent is refused"), Traffic->RescueStranded(Mover, *Net, FGuidelineNodeId()));
	TestFalse(TEXT("an unknown id is refused"), Traffic->RescueStranded(9999, *Net, FGuidelineNodeId()));
	return true;
}

// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTrafficRescueStrandedNewGoalTest,
	"Airside.Model.Traffic.RescueStranded.NewGoal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrafficRescueStrandedNewGoalTest::RunTest(const FString& Parameters)
{
	// SEND HOME AND FIND STAND BOTH RESCUE TOWARD A GOAL THAT IS NOT THE AGENT'S OWN. The goal must
	// move the way every other goal change moves it - ReleaseGoal/TakeGoal - so the old goal's node
	// claim lets go: a stand left claimed by an aircraft that will never reach it is a stand
	// ChooseStand never offers again.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 20000.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 40000.0, 0.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 30000.0, 10000.0);
	TestGraph::Join(*Net, A, B);
	TestGraph::Join(*Net, B, C);
	TestGraph::Join(*Net, B, D);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const int32 Plane = StrandPlaneOn(*Traffic, *Net, A, C);
	if (!TestTrue(TEXT("stranded on A -> B, bound for C"), Plane > 0)) { return false; }
	TestEqual(TEXT("its goal was C"), Traffic->FindAgent(Plane)->GoalNode, C);

	if (!TestTrue(TEXT("rescued toward D"), Traffic->RescueStranded(Plane, *Net, D))) { return false; }
	TestEqual(TEXT("its goal is D now"), Traffic->FindAgent(Plane)->GoalNode, D);
	TestNotEqual(TEXT("and C is nobody's"), Traffic->HolderOfNode(C), Plane);

	TickUntil(*Traffic, *Net, 300.0, [&](int32)
	{
		const FRoadAgent* Q = Traffic->FindAgent(Plane);
		return Q != nullptr && Q->Phase == EAgentPhase::Taxiing;
	});
	const FRoadAgent* P = Traffic->FindAgent(Plane);
	TestEqual(TEXT("it parks"), P->Phase, EAgentPhase::Parked);
	TestTrue(TEXT("at D"), FVector2D::Distance(P->LastMotion.Position, FVector2D(30000.0, 10000.0)) < 100.0);
	return true;
}

#endif
