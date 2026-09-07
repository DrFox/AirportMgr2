#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Profiles/RoadProfile.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogM2MapProbe, Log, All);

/**
 * A PROBE, not a test of the code: it loads the level the player last saved, takes the
 * road network out of it, runs the same solve / guideline build / anchor link the actor
 * runs on load, and logs what the guideline graph and the stands look like - so a "none
 * of the routes are there in PIE" report can be read off the player's own data headlessly
 * rather than guessed at. Passes trivially when the map has no network; every line it
 * writes is evidence, the assertions are only that the pipeline ran.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStarterMapProbeTest,
	"Airside.Probe.StarterMapRoutes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStarterMapProbeTest::RunTest(const FString& Parameters)
{
	const FString MapPath = TEXT("/Game/Maps/M_Starter");
	UPackage* Package = LoadPackage(nullptr, *MapPath, LOAD_None);
	if (Package == nullptr)
	{
		AddInfo(TEXT("M_Starter not loadable in this run - nothing to probe."));
		return true;
	}

	const URoadNetwork* Saved = nullptr;
	for (TObjectIterator<URoadNetwork> It; It; ++It)
	{
		if (It->GetOutermost() == Package && It->GetSegments().Num() > 0)
		{
			Saved = *It;
			break;
		}
	}
	if (Saved == nullptr)
	{
		AddInfo(TEXT("M_Starter holds no road network with segments - nothing to probe."));
		return true;
	}

	// A COPY, so the probe cannot dirty the player's level.
	URoadNetwork* Net = DuplicateObject<URoadNetwork>(Saved, GetTransientPackage());

	int32 SegmentsAlive = 0;
	for (const FRoadSegment& Segment : Net->GetSegments()) { SegmentsAlive += Segment.bAlive ? 1 : 0; }
	int32 GuidelineNodesSaved = 0, GuidelineEdgesSaved = 0, AuthoredEdgesSaved = 0;
	for (const FGuidelineNode& N : Net->GetGuidelineNodes()) { GuidelineNodesSaved += N.bAlive ? 1 : 0; }
	for (const FGuidelineEdge& E : Net->GetGuidelineEdges()) { if (E.bAlive) { ++GuidelineEdgesSaved; AuthoredEdgesSaved += E.bDerived ? 0 : 1; } }
	UE_LOG(LogM2MapProbe, Log, TEXT("PROBE saved level: %d live segments, %d entities, %d holding-position marks; guideline graph AS SAVED: %d nodes, %d edges (%d hand-authored)"),
		SegmentsAlive, Net->GetEntities().Num(), Net->GetHoldingPositionMarks().Num(), GuidelineNodesSaved, GuidelineEdgesSaved, AuthoredEdgesSaved);

	const FRoadSolveResult Solved = FRoadNetworkSolver::SolveAll(*Net);
	FRoadGuidelineBuilder::Build(*Net, Solved);
	const int32 Joined = FAnchorLink::Build(*Net);

	int32 GuidelineNodes = 0, GuidelineEdges = 0, AuthoredEdges = 0, HoldingPositionNodes = 0;
	for (const FGuidelineNode& N : Net->GetGuidelineNodes()) { if (N.bAlive) { ++GuidelineNodes; HoldingPositionNodes += N.HoldingPositionFor.IsSet() ? 1 : 0; } }
	for (const FGuidelineEdge& E : Net->GetGuidelineEdges()) { if (E.bAlive) { ++GuidelineEdges; AuthoredEdges += E.bDerived ? 0 : 1; } }
	UE_LOG(LogM2MapProbe, Log, TEXT("PROBE after rebuild: solved %d nodes (%d failed); guideline graph %d nodes, %d edges (%d hand-authored), %d holding-position nodes; anchor links joined this pass: %d"),
		Solved.SolvedNodes, Solved.FailedNodes, GuidelineNodes, GuidelineEdges, AuthoredEdges, HoldingPositionNodes, Joined);

	// The layout itself, so a lead-in that joins nothing can be judged against where the
	// taxiways actually are rather than against a guess at the player's drawing.
	{
		int32 NodeIndex = 0;
		for (const FRoadNode& Node : Net->GetNodes())
		{
			++NodeIndex;
			if (!Node.bAlive) { continue; }
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE road node %d at (%.0f, %.0f), %d incident"), NodeIndex, Node.Position.X, Node.Position.Y, Node.Incident.Num());
		}
	}

	// Every segment: did it pass the builder's gate (alive, both ends solved, a profile with
	// guidelines) and how many derived edges did it get? This is the line that separates
	// "no profile" from "no guidelines on the profile" from "ends never solved".
	{
		int32 SegmentIndex = 0;
		for (const FRoadSegment& Segment : Net->GetSegments())
		{
			FRoadSegmentId SegmentId;
			SegmentId.Index = SegmentIndex;
			SegmentId.Generation = Segment.Generation;
			++SegmentIndex;
			if (!Segment.bAlive) { continue; }
			const URoadProfile* Profile = Segment.Profile.Get();
			int32 Derived = 0;
			for (const FGuidelineEdge& E : Net->GetGuidelineEdges()) { Derived += (E.bAlive && E.bDerived && E.DerivedFrom == SegmentId) ? 1 : 0; }
			const FRoadNode* NodeA = Net->GetNode(Segment.A);
			const FRoadNode* NodeB = Net->GetNode(Segment.B);
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE segment %d (%.0f, %.0f)->(%.0f, %.0f): solved A %d B %d, profile %s (%d guideline(s) declared), runway %d, %d derived edge(s)"),
				SegmentIndex,
				NodeA ? NodeA->Position.X : 0.0, NodeA ? NodeA->Position.Y : 0.0,
				NodeB ? NodeB->Position.X : 0.0, NodeB ? NodeB->Position.Y : 0.0,
				Segment.bSolvedA, Segment.bSolvedB,
				Profile ? *Profile->GetName() : TEXT("NULL"), Profile ? Profile->Guidelines.Num() : 0,
				Net->IsRunwaySegment(SegmentId), Derived);
		}
	}

	// Every holding position: where it is, what kind, which segment end it was derived for,
	// how far it sits from that segment's road node and where that segment's pavement cut
	// is - a runway-holding position belongs where the taxiway is its own width, at or
	// beyond the cut, never inside the corner's fillet and never on the strip. (2026-09-07:
	// six of six sat 100-900 uu inside their cuts before the cut became a floor.)
	{
		const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FGuidelineNode& Node = Nodes[Index];
			if (!Node.bAlive || Node.HoldingPosition == EHoldingPositionKind::None) { continue; }
			double FromRoadNode = -1.0, Cut = -1.0;
			FString Where = TEXT("no origin");
			if (const FRoadSegment* Segment = Node.Origin.IsSet() ? Net->GetSegment(Node.Origin.Segment) : nullptr)
			{
				const FRoadNode* RoadNode = Net->GetNode(Node.Origin.bEndA ? Segment->A : Segment->B);
				if (RoadNode) { FromRoadNode = FVector2D::Distance(Node.Position, RoadNode->Position); }
				const URoadProfile* P = Net->ProfileFor(*Segment);
				Where = FString::Printf(TEXT("segment %d end %s (%s)"), Node.Origin.Segment.Index, Node.Origin.bEndA ? TEXT("A") : TEXT("B"),
					P ? *P->GetName() : TEXT("no profile"));
				Cut = Node.Origin.bEndA ? Segment->TrimA : Segment->TrimB;
			}
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE holding position: node %d at (%.0f, %.0f) kind %d for runway seg %d, derived for %s, %.0f uu from its road node (its pavement cut is at %.0f), %d incident"),
				Index, Node.Position.X, Node.Position.Y, static_cast<int32>(Node.HoldingPosition), Node.HoldingPositionFor.Index, *Where, FromRoadNode, Cut, Node.Incident.Num());
		}
	}

	// Every stand: does its pose node exist, is it joined to anything, and can an arrival's
	// exit reach it?
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	FVector2D Threshold, Direction; double Length = 0.0;
	const bool bRunway = Net->NearestRunwayThreshold(FVector2D::ZeroVector, Threshold, Direction, Length);
	const FArrivalPlan Plan = bRunway ? ArrivalPlanner::Plan(*Net, Threshold - Direction * 1000.0, Airframe) : FArrivalPlan();
	UE_LOG(LogM2MapProbe, Log, TEXT("PROBE arrival from the nearest threshold: runway %d, plan says %s, %d usable exit(s)"),
		bRunway, *ArrivalPlanner::DescribeRefusal(Plan), Plan.ExitCount);

	int32 StandIndex = 0;
	for (const FEntityInstance& Stand : Net->GetEntities())
	{
		++StandIndex;
		if (!Stand.bAlive) { continue; }
		const FGuidelineNode* Pose = Net->GetGuidelineNode(Stand.PoseNode);
		int32 AnchorsJoined = 0;
		for (const FResolvedAnchor& Anchor : Stand.ResolvedAnchors)
		{
			const FGuidelineNode* Node = Net->GetGuidelineNode(Anchor.Node);
			AnchorsJoined += (Node != nullptr && Node->Incident.Num() > 0) ? 1 : 0;
		}
		FString Reach = TEXT("no runway");
		if (bRunway && Pose != nullptr)
		{
			// From EVERY node on the strip, not just the plan's chosen exit.
			const TArray<FGuidelineNodeId> Exits = Net->RunwayExitNodes(Threshold, Direction, Length, 2250.0, 0.0);
			int32 Reachable = 0;
			for (const FGuidelineNodeId& Exit : Exits)
			{
				FRouteQuery Q; Q.Start = Exit; Q.Goal = Stand.PoseNode; Q.Class = ETraversalClass::Aircraft;
				Reachable += RouteSearch::Find(*Net, Q).IsValid() ? 1 : 0;
			}
			Reach = FString::Printf(TEXT("reachable from %d of %d runway nodes"), Reachable, Exits.Num());
		}
		UE_LOG(LogM2MapProbe, Log, TEXT("PROBE stand %d at (%.0f, %.0f) heading %.2f: pose node %s (%d incident), %d of %d anchors joined, %s"),
			StandIndex, Stand.Position.X, Stand.Position.Y, Stand.Heading,
			Pose ? TEXT("live") : TEXT("DEAD"), Pose ? Pose->Incident.Num() : 0,
			AnchorsJoined, Stand.ResolvedAnchors.Num(), *Reach);
	}
	return true;
}

#endif
