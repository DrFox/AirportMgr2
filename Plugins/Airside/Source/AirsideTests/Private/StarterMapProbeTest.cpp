#include "CoreMinimal.h"
#include "Build/AnchorLink.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/RunwayMarkingBuilder.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Entities/AircraftType.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/RunwayAdmission.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"
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

	// FUEL READINESS, PER ENTITY, so "the truck never comes" is answered by a grep rather
	// than by a PIE session: which stands can be fuelled at all, and which depots can
	// dispatch. A stand whose Fuel anchor joins nothing is a stand no fuel service can ever
	// serve; a depot whose pose joins nothing cannot send a truck anywhere.
	{
		int32 StandsWithJoinedFuel = 0, StandsTotal = 0, DepotsJoined = 0, DepotsTotal = 0;
		for (int32 Index = 0; Index < Net->GetEntities().Num(); ++Index)
		{
			const FEntityInstance& Instance = Net->GetEntities()[Index];
			if (!Instance.bAlive) { continue; }

			const FEntityInstanceId Id = Net->EntityIdAt(Index);
			const FGuidelineNode* Pose = Net->GetGuidelineNode(Instance.PoseNode);
			const bool bPoseJoined = Pose != nullptr && Pose->Incident.Num() > 0;

			if (Instance.PoseRole != EServiceRole::Aircraft)
			{
				++DepotsTotal;
				DepotsJoined += bPoseJoined ? 1 : 0;
				UE_LOG(LogM2MapProbe, Log,
					TEXT("PROBE depot %d at (%.0f, %.0f): pose %s a road, %d truck(s)"),
					Index, Instance.Position.X, Instance.Position.Y,
					bPoseJoined ? TEXT("joins") : TEXT("JOINS NO"), Instance.Trucks);
				continue;
			}

			++StandsTotal;
			for (const FName FuelId : Net->GetAnchorIdsForRole(Id, EServiceRole::Fuel))
			{
				const FResolvedAnchor* Anchor = Net->FindResolvedAnchor(Id, FuelId);

				// THE WALK, not the incident count. A hydrant is always spurred to its own
				// service lane, so counting edges here would report every stand ever placed as
				// on a road - which is the exact shape of the defect this probe caught in
				// StandFuel.IsSet() before it.
				const bool bJoined = Anchor != nullptr && Net->IsServiceNodeConnected(Anchor->Node);
				StandsWithJoinedFuel += bJoined ? 1 : 0;
				UE_LOG(LogM2MapProbe, Log, TEXT("PROBE stand %d anchor '%s' (Fuel): %s a road"),
					Index, *FuelId.ToString(), bJoined ? TEXT("joins") : TEXT("JOINS NO"));
			}

			// THE LANE ITSELF, so "the truck never comes" can be read off one line: a stand
			// whose lane reaches no road wants a service road drawn near it, which is a
			// different repair from a stand whose definition has no hydrant at all.
			bool bLaneConnected = false;
			int32 LaneEdges = 0;
			for (const FGuidelineEdge& Edge : Net->GetGuidelineEdges())
			{
				LaneEdges += (Edge.bAlive && Edge.ServiceLoopOwner == Id) ? 1 : 0;
			}
			for (const FResolvedAnchor& Anchor : Instance.ResolvedAnchors)
			{
				bLaneConnected = bLaneConnected || Net->IsServiceNodeConnected(Anchor.Node);
			}
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE stand %d service lane: %d edge(s), %s a road"),
				Index, LaneEdges, bLaneConnected ? TEXT("reaches") : TEXT("REACHES NO"));
		}
		UE_LOG(LogM2MapProbe, Log,
			TEXT("PROBE fuel readiness: %d of %d stand fuel anchor(s) on a road, %d of %d depot(s) on a road"),
			StandsWithJoinedFuel, StandsTotal, DepotsJoined, DepotsTotal);
	}

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

	// Every runway chain: what it is (facts, width, length), what it admits the default
	// airframe to do, and what the marking builder paints for it. This is the line to read
	// when a runway on the player's level "has no numbers" or "refuses the Piper".
	{
		FRoadMeshBuffers Markings;
		FRunwayMarkingCensus Census;
		const int32 Painted = FRunwayMarkingBuilder::Build(*Net, 0.0, Markings, &Census);
		UE_LOG(LogM2MapProbe, Log, TEXT("PROBE runway markings: %d runway(s), %d triangle(s): %d threshold stripes, %d designator strokes, %d centreline dashes, %d aiming bars, %d touchdown stripes, %d side stripes, %d grass markers"),
			Painted, Markings.Indices.Num() / 3, Census.ThresholdStripes, Census.DesignatorStrokes, Census.CentrelineDashes,
			Census.AimingPointBars, Census.TouchdownStripes, Census.SideStripes, Census.GrassMarkers);
		TSet<int32> Seen;
		for (int32 Index = 0; Index < Net->GetSegments().Num(); ++Index)
		{
			const FRoadSegment& Segment = Net->GetSegments()[Index];
			if (!Segment.bAlive || Seen.Contains(Index)) { continue; }
			FRoadSegmentId Id; Id.Index = Index; Id.Generation = Segment.Generation;
			if (!Net->IsRunwaySegment(Id)) { continue; }
			const TArray<FRoadSegmentId> Chain = Net->RunwayChain(Id);
			for (const FRoadSegmentId& Member : Chain) { Seen.Add(Member.Index); }
			FVector2D ChainThreshold, ChainDirection; double ChainLength = 0.0;
			if (const FRoadNode* A = Net->GetNode(Segment.A)) { Net->RunwayExtentAt(A->Position, ChainThreshold, ChainDirection, ChainLength); }
			const URoadProfile* Profile = Net->ProfileFor(Segment);
			const FRunwayFacts Facts = Net->RunwayFactsFor(Id);
			const FRunwayAdmission Landing = RunwayAdmission::Check(*Net, Id, Airframe, true);
			const FRunwayAdmission Takeoff = RunwayAdmission::Check(*Net, Id, Airframe, false);
			UE_LOG(LogM2MapProbe, Log, TEXT("PROBE runway %s from segment %d: %d segment(s), %.0f uu long, %.0f uu wide, %s, %s approach; default airframe landing: %s; take-off: %s"),
				*RunwayDesignator::ToPairText(ChainDirection), Index, Chain.Num(), ChainLength, Profile ? Profile->GetTotalWidth() : 0.0,
				RunwaySurfaceName(Facts.Surface), RunwayApproachName(Facts.Approach),
				*(Landing.IsAdmitted() ? FString(TEXT("admitted")) : RunwayAdmission::Describe(Landing)),
				*(Takeoff.IsAdmitted() ? FString(TEXT("admitted")) : RunwayAdmission::Describe(Takeoff)));
		}
	}

	FVector2D Threshold, Direction; double Length = 0.0; FRoadSegmentId RunwaySeed;
	const bool bRunway = Net->NearestRunwayThreshold(FVector2D::ZeroVector, Threshold, Direction, Length, &RunwaySeed);
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
			const TArray<FGuidelineNodeId> Exits = Net->RunwayExitNodes(RunwaySeed, Threshold, Direction, 0.0);
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

	// WHICH AIRCRAFT THIS FIELD CAN ACTUALLY TAKE, per type, with the reason when it cannot.
	//
	// Added 2026-09-11 because the offer inbox greyed out every Accept and the refusal was
	// only ever rendered on screen - the log could not tell "no stand free" from "the runway
	// is 900 m short", and those have completely different fixes. ArrivalPlanner::Plan with
	// no occupancy is the same question the inbox asks, so this cannot drift from it.
	// EVERY AUTHORED TYPE, loaded from the registry - not TObjectIterator, which sees only
	// what happens to be in memory. The first version of this probe reported on the A320
	// alone, because M_Starter's stand references it and nothing had pulled the others in;
	// "which aircraft can this field take" answered for one aircraft is not an answer.
	TArray<FAssetData> TypeAssets;
	FAssetRegistryModule& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	Registry.Get().SearchAllAssets(true);
	Registry.Get().GetAssetsByClass(UAircraftType::StaticClass()->GetClassPathName(), TypeAssets);

	for (const FAssetData& Asset : TypeAssets)
	{
		const UAircraftType* Type = Cast<UAircraftType>(Asset.GetAsset());
		if (Type == nullptr || Type->HasAnyFlags(RF_ClassDefaultObject))
		{
			continue;
		}
		const FAirframe TypeAirframe = Type->Airframe();
		const FArrivalPlan TypePlan = ArrivalPlanner::Plan(*Net, FVector2D::ZeroVector, TypeAirframe, nullptr);
		UE_LOG(LogM2MapProbe, Log,
			TEXT("PROBE admits %s (%.0f uu span, %.0f uu published landing): %s%s needs %.0f uu of %.0f uu"),
			*Type->GetName(), TypeAirframe.Wingspan, TypeAirframe.Requirements.LandingFieldLength,
			TypePlan.IsValid() ? TEXT("YES") : TEXT("NO - "),
			TypePlan.IsValid() ? TEXT("") : *ArrivalPlanner::DescribeRefusal(TypePlan),
			TypePlan.Needed, TypePlan.RunwayLength);
	}
	return true;
}

#endif
