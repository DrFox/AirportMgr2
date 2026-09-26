#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/ReverseTurn.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/VehicleFit.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// REVERSE TURNS, DERIVED (spec 2026-09-26 §3). Every network here is derived the way a rebuild
// derives it - the production design vehicles, Wide tier (the rig's) - so the pull-past and the
// fillet are measured against the vehicle that will actually back through them.

namespace ReverseTurnBuild
{
	struct FJunction
	{
		URoadNetwork* Net = nullptr;
		FRoadNodeId Node;
		FRoadNodeId PullPastFar;
		FRoadNodeId BayFar;
		FRoadSegmentId PullPast;
		FRoadSegmentId Bay;
	};

	/**
	 * A Wide T at the origin with a pull-past arm east to (PullPastLength,0) and one reverse turn
	 * recorded from it. A 90 degree bay runs north to (0,4500), the approach from the west; a
	 * straight bay (bStraight) runs OPPOSITE the pull-past, west to (-4500,0), so the approach
	 * comes in from the south instead.
	 */
	FJunction Tee(double PullPastLength, bool bStraight)
	{
		FJunction Out;
		const TArray<URoadProfile*> Tiers = TestProfiles::ServiceTiers();
		URoadProfile* Wide = Tiers.Num() == 3 ? Tiers[2] : nullptr;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		Out.Node = Out.Net->AddNode(FVector2D::ZeroVector);
		Out.PullPastFar = Out.Net->AddNode(FVector2D(PullPastLength, 0.0));
		// 45 m BAYS: the Wide corners cut each arm back ~19 m, and the bay must hold the rig's chain
		// beyond that (LayReverseTurn's bay check).
		Out.BayFar = Out.Net->AddNode(bStraight ? FVector2D(-4500.0, 0.0) : FVector2D(0.0, 4500.0));
		const FRoadNodeId Approach = Out.Net->AddNode(bStraight ? FVector2D(0.0, -4000.0) : FVector2D(-4000.0, 0.0));
		Out.PullPast = Out.Net->AddStraightSegment(Out.Node, Out.PullPastFar, Wide);
		Out.Bay = Out.Net->AddStraightSegment(Out.Node, Out.BayFar, Wide);
		Out.Net->AddStraightSegment(Approach, Out.Node, Wide);
		FReverseTurn Turn;
		Turn.Node = Out.Node;
		Turn.FromFar = Out.PullPastFar;
		Turn.IntoFar = Out.BayFar;
		Out.Net->AddReverseTurn(Turn);
		const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
		TestGraph::Derive(*Out.Net, &Designs);
		return Out;
	}

	/** The pull-past lane's end at its far node: where a vehicle stops before backing in. */
	FGuidelineNodeId StopNode(const FJunction& J)
	{
		for (const FGuidelineEdge& Edge : J.Net->GetGuidelineEdges())
		{
			// The segment runs Node -> PullPastFar (A -> B), so the lane leaving Node is AToB.
			if (Edge.bAlive && Edge.DerivedFrom == J.PullPast && Edge.Direction == EGuidelineDir::AToB)
			{
				return Edge.B;
			}
		}
		return FGuidelineNodeId();
	}

	FRoutePlan Probe(const URoadNetwork& Net, FGuidelineNodeId From, FGuidelineNodeId To)
	{
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, From, To, 0.0, ETraversalClass::GroundVehicle);
		return RouteSearch::Find(Net, Query);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReverseTurnEdgeTest, "Airside.Build.ReverseTurn.EdgeIsFlaggedAndStartsAtTheStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnEdgeTest::RunTest(const FString& Parameters)
{
	// A 90 DEGREE BAY: the route from the stop to the bay's end exists, is ALL reverse leg, and
	// begins at the very node the pull-past lane ends at - so a vehicle arriving along that lane
	// arms the reverse where it stops. And the bay has a way out, forwards, back to the junction.
	const ReverseTurnBuild::FJunction J = ReverseTurnBuild::Tee(4000.0, false);
	if (!TestNotNull(TEXT("the content set has its service tiers"), J.Net)) { return false; }
	const FGuidelineNodeId End = J.Net->GetReverseTurnEnd(0);
	if (!TestTrue(TEXT("the reverse turn was laid"), End.IsSet())) { return false; }
	const FGuidelineNodeId Stop = ReverseTurnBuild::StopNode(J);
	const FRoutePlan In = ReverseTurnBuild::Probe(*J.Net, Stop, End);
	TestTrue(TEXT("stop -> bay end routes"), In.IsValid());
	bool bAllReverse = In.Steps.Num() > 0;
	for (const FRouteStep& Step : In.Steps)
	{
		bAllReverse &= Step.bReverseLeg;
	}
	TestTrue(TEXT("every step of it is reverse leg"), bAllReverse);
	TestTrue(TEXT("and it starts at the stop"), In.Start == Stop);

	// Out: from the bay end, forwards, to the approach arm's far end.
	bool bExitForward = false;
	for (const FGuidelineEdge& Edge : J.Net->GetGuidelineEdges())
	{
		bExitForward |= Edge.bAlive && Edge.A == End && !Edge.bReverseLeg;
	}
	TestTrue(TEXT("a forward exit leaves the bay end"), bExitForward);

	// THE BAY END IS IN THE BAY: past the fillet, on the bay arm's side of the junction.
	const FVector2D EndAt = J.Net->GetGuidelineNode(End)->Position;
	TestTrue(FString::Printf(TEXT("the bay end (%.0f,%.0f) is up the bay arm"), EndAt.X, EndAt.Y), EndAt.Y > 3500.0 && FMath::Abs(EndAt.X) < 400.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReverseTurnStraightTest, "Airside.Build.ReverseTurn.StraightBayIsOneLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnStraightTest::RunTest(const FString& Parameters)
{
	// OPPOSITE ARMS: the reverse leg is one straight line - the pull-past lane carried on
	// through the junction into the bay - so a chain that stopped in line backs in in line.
	const ReverseTurnBuild::FJunction J = ReverseTurnBuild::Tee(4000.0, true);
	if (!TestNotNull(TEXT("the content set has its service tiers"), J.Net)) { return false; }
	const FGuidelineNodeId End = J.Net->GetReverseTurnEnd(0);
	if (!TestTrue(TEXT("the straight bay was laid"), End.IsSet())) { return false; }
	const FRoutePlan In = ReverseTurnBuild::Probe(*J.Net, ReverseTurnBuild::StopNode(J), End);
	if (!TestTrue(TEXT("it routes"), In.IsValid())) { return false; }
	double Worst = 0.0;
	for (const FVector2D& Point : In.Polyline)
	{
		Worst = FMath::Max(Worst, FMath::Abs(Point.Y - In.Polyline[0].Y));
	}
	TestTrue(FString::Printf(TEXT("every vertex on the pull-past lane's line (worst %.3f uu off)"), Worst), Worst < 0.5);
	TestTrue(TEXT("and it ends west of the junction, in the bay"), In.Polyline.Last().X < -3500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReverseTurnPullPastTest, "Airside.Build.ReverseTurn.PullPastHoldsTheChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnPullPastTest::RunTest(const FString& Parameters)
{
	// AN 8 M PULL-PAST CANNOT HOLD THE RIG (chain 13.4 m): not laid - the stopped trailer would
	// still be in the corner - but the record is KEPT, because lengthening the arm fixes it.
	const ReverseTurnBuild::FJunction J = ReverseTurnBuild::Tee(800.0, true);
	if (!TestNotNull(TEXT("the content set has its service tiers"), J.Net)) { return false; }
	TestFalse(TEXT("not laid"), J.Net->GetReverseTurnEnd(0).IsSet());
	TestEqual(TEXT("the record is kept"), J.Net->GetReverseTurns().Num(), 1);
	bool bAnyReverse = false;
	for (const FGuidelineEdge& Edge : J.Net->GetGuidelineEdges())
	{
		bAnyReverse |= Edge.bAlive && Edge.bReverseLeg;
	}
	TestFalse(TEXT("and no reverse edge was laid"), bAnyReverse);
	TestTrue(TEXT("the rig's chain is what it is measured against"),
		VehicleFit::ChainLength(UAirsideSettings::ResolveRigVehicle()) > 800.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReverseTurnRebuildTest, "Airside.Build.ReverseTurn.SurvivesRebuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnRebuildTest::RunTest(const FString& Parameters)
{
	// RE-DERIVED, NOT KEPT: every rebuild lays it again on fresh nodes, in the same place, and the
	// end handle it reports is the fresh one.
	const ReverseTurnBuild::FJunction J = ReverseTurnBuild::Tee(4000.0, false);
	if (!TestNotNull(TEXT("the content set has its service tiers"), J.Net)) { return false; }
	const FGuidelineNode* First = J.Net->GetGuidelineNode(J.Net->GetReverseTurnEnd(0));
	if (!TestNotNull(TEXT("laid on the first derivation"), First)) { return false; }
	const FVector2D Before = First->Position;
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	TestGraph::Derive(*J.Net, &Designs);
	TestGraph::Derive(*J.Net, &Designs);
	const FGuidelineNodeId End = J.Net->GetReverseTurnEnd(0);
	const FGuidelineNode* After = J.Net->GetGuidelineNode(End);
	if (!TestNotNull(TEXT("still laid, on a live node"), After)) { return false; }
	TestTrue(TEXT("in the same place"), FVector2D::Distance(Before, After->Position) < 0.01);
	TestTrue(TEXT("and it still routes"), ReverseTurnBuild::Probe(*J.Net, ReverseTurnBuild::StopNode(J), End).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReverseTurnDeadArmTest, "Airside.Build.ReverseTurn.DeadArmDropsTheRecord",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnDeadArmTest::RunTest(const FString& Parameters)
{
	// THE BAY ROAD DELETED: nothing left to back into, so the next rebuild drops the record.
	const ReverseTurnBuild::FJunction J = ReverseTurnBuild::Tee(4000.0, false);
	if (!TestNotNull(TEXT("the content set has its service tiers"), J.Net)) { return false; }
	J.Net->RemoveSegment(J.Bay);
	const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
	TestGraph::Derive(*J.Net, &Designs);
	TestEqual(TEXT("the record is gone"), J.Net->GetReverseTurns().Num(), 0);
	TestFalse(TEXT("and reports no end"), J.Net->GetReverseTurnEnd(0).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReverseTurnCopyTest, "Airside.Build.ReverseTurn.CopyCarriesTheRecord",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FReverseTurnCopyTest::RunTest(const FString& Parameters)
{
	// URoadNetwork::CopyFrom copies every array by hand (#166): the ghost preview's scratch network
	// must carry the permission too, or a ghost rebuild would lay the junction without its bay.
	const ReverseTurnBuild::FJunction J = ReverseTurnBuild::Tee(4000.0, false);
	if (!TestNotNull(TEXT("the content set has its service tiers"), J.Net)) { return false; }
	URoadNetwork* Copy = NewObject<URoadNetwork>(GetTransientPackage());
	Copy->CopyFrom(*J.Net);
	TestEqual(TEXT("the record is copied"), Copy->GetReverseTurns().Num(), 1);
	TestTrue(TEXT("and its end"), Copy->GetReverseTurnEnd(0) == J.Net->GetReverseTurnEnd(0));
	return true;
}

#endif
