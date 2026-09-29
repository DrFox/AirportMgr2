#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiwayStrip.h"
#include "Model/TaxiwayRestriction.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"
#include "StandFixture.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkTest,
	"Airside.Model.Network",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);

	const FRoadNodeId Centre = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId East   = Net->AddNode(FVector2D(10000.0, 0.0));
	const FRoadNodeId North  = Net->AddNode(FVector2D(0.0, 10000.0));
	const FRoadNodeId West   = Net->AddNode(FVector2D(-10000.0, 0.0));

	const FRoadSegmentId ToNorth = Net->AddStraightSegment(Centre, North, Profile);
	const FRoadSegmentId ToEast  = Net->AddStraightSegment(Centre, East,  Profile);
	const FRoadSegmentId ToWest  = Net->AddStraightSegment(Centre, West,  Profile);

	TestTrue(TEXT("segments created"), ToNorth.IsSet() && ToEast.IsSet() && ToWest.IsSet());

	// Outgoing tangents at the centre node.
	const FVector2D TanEast = Net->GetOutgoingTangent(ToEast, Centre);
	TestTrue(TEXT("east tangent"), TanEast.Equals(FVector2D(1.0, 0.0), 1e-6));

	const FVector2D TanNorth = Net->GetOutgoingTangent(ToNorth, Centre);
	TestTrue(TEXT("north tangent"), TanNorth.Equals(FVector2D(0.0, 1.0), 1e-6));

	// Tangent at the far end points back toward the centre.
	const FVector2D TanBack = Net->GetOutgoingTangent(ToEast, East);
	TestTrue(TEXT("reverse tangent"), TanBack.Equals(FVector2D(-1.0, 0.0), 1e-6));

	// Incident list sorted by bearing ascending: east(0), north(UE_DOUBLE_PI/2), west(UE_DOUBLE_PI).
	const FRoadNode* CentreNode = Net->GetNode(Centre);
	TestEqual(TEXT("incident count"), CentreNode->Incident.Num(), 3);
	TestTrue(TEXT("order[0] east"),  CentreNode->Incident[0] == ToEast);
	TestTrue(TEXT("order[1] north"), CentreNode->Incident[1] == ToNorth);
	TestTrue(TEXT("order[2] west"),  CentreNode->Incident[2] == ToWest);

	// Removing a segment updates both endpoints' incident lists.
	TestTrue(TEXT("remove"), Net->RemoveSegment(ToNorth));
	TestEqual(TEXT("incident after remove"), Net->GetNode(Centre)->Incident.Num(), 2);
	TestEqual(TEXT("far node emptied"), Net->GetNode(North)->Incident.Num(), 0);
	TestNull(TEXT("segment gone"), Net->GetSegment(ToNorth));

	// Removing a node cascades to its segments.
	TestTrue(TEXT("remove centre"), Net->RemoveNode(Centre));
	TestNull(TEXT("centre gone"), Net->GetNode(Centre));
	TestNull(TEXT("east segment cascaded"), Net->GetSegment(ToEast));
	TestEqual(TEXT("east node emptied"), Net->GetNode(East)->Incident.Num(), 0);

	// Self-loops and invalid handles are rejected.
	TestFalse(TEXT("self loop rejected"), Net->AddStraightSegment(East, East, Profile).IsSet());
	TestFalse(TEXT("stale handle rejected"), Net->AddStraightSegment(Centre, East, Profile).IsSet());

	// IsSet() reports assignment, not liveness: a handle to a slot that has since been
	// removed still reads as set. Liveness is RoadSlot::IsValid's job, and the network
	// enforces it - which is why the stale-handle AddStraightSegment above was rejected.
	TestTrue(TEXT("stale handle still reads as set"), Centre.IsSet());
	TestNull(TEXT("stale handle is not live"), Net->GetNode(Centre));

	// --- GetOutgoingTangent must never return a zero vector ---
	//
	// Only A == B is rejected, so two DISTINCT nodes may sit at the same position. The
	// degenerate-control fallback then computes a zero chord, and an unguarded
	// GetSafeNormal hands back (0,0) - which collapses every edge ray and makes the node
	// silently fail to solve rather than fail loudly.
	{
		URoadNetwork* Degenerate = NewObject<URoadNetwork>(GetTransientPackage());

		// Case 1: Control sits exactly on the node, so the chord fallback is taken.
		const FRoadNodeId Origin = Degenerate->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Far    = Degenerate->AddNode(FVector2D(0.0, 5000.0));
		const FRoadSegmentId Straight =
			Degenerate->AddSegment(Origin, Far, FVector2D(0.0, 0.0), Profile);
		TestTrue(TEXT("degenerate-control segment created"), Straight.IsSet());

		const FVector2D FromControlFallback = Degenerate->GetOutgoingTangent(Straight, Origin);
		TestTrue(TEXT("control-on-node falls back to the chord"),
			FromControlFallback.Equals(FVector2D(0.0, 1.0), 1e-6));

		// Case 2: two distinct nodes at the same position - the chord is zero too.
		const FRoadNodeId Twin = Degenerate->AddNode(FVector2D(0.0, 0.0));
		const FRoadSegmentId Coincident =
			Degenerate->AddSegment(Origin, Twin, FVector2D(0.0, 0.0), Profile);
		TestTrue(TEXT("coincident segment created"), Coincident.IsSet());

		const FVector2D BothEnds[] = {
			Degenerate->GetOutgoingTangent(Coincident, Origin),
			Degenerate->GetOutgoingTangent(Coincident, Twin)
		};
		for (const FVector2D& Tangent : BothEnds)
		{
			TestTrue(TEXT("coincident-node tangent is unit length"),
				FMath::IsNearlyEqual(Tangent.Length(), 1.0, 1e-9));
		}

		// And the earlier fallbacks are unit length too, by the same contract.
		TestTrue(TEXT("chord fallback is unit length"),
			FMath::IsNearlyEqual(FromControlFallback.Length(), 1.0, 1e-9));
	}

	// --- Cut vertices are part of the model, not something callers recompute (K2) ---
	{
		URoadNetwork* CutNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* CutProfile = URoadProfile::MakeTransient(2300.0, 1500.0);

		const FRoadNodeId P = CutNet->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Q = CutNet->AddNode(FVector2D(10000.0, 0.0));
		const FRoadSegmentId Seg = CutNet->AddStraightSegment(P, Q, CutProfile);

		const FRoadSegment* Fresh = CutNet->GetSegment(Seg);
		TestFalse(TEXT("a new segment's A end is not yet solved"), Fresh->bSolvedA);
		TestFalse(TEXT("a new segment's B end is not yet solved"), Fresh->bSolvedB);
		TestTrue(TEXT("cut vertices start at zero"),
			Fresh->LeftCutA.IsZero() && Fresh->RightCutA.IsZero() &&
			Fresh->LeftCutB.IsZero() && Fresh->RightCutB.IsZero());

		// Only the solver writes these; the test stands in for it here through the same
		// WriteSegmentEndSolve mutator SolveNodeInto calls (#191), not a raw FRoadSegment*.
		FJunctionArmResult SolveA;
		SolveA.LeftCut  = FVector2D(1150.0, 1150.0);
		SolveA.RightCut = FVector2D(1150.0, -1150.0);
		CutNet->WriteSegmentEndSolve(Seg, /*bEndA=*/true, SolveA);

		FJunctionArmResult SolveB;
		SolveB.LeftCut  = FVector2D(8850.0, -1150.0);
		SolveB.RightCut = FVector2D(8850.0, 1150.0);
		CutNet->WriteSegmentEndSolve(Seg, /*bEndA=*/false, SolveB);

		const FRoadSegment* Solved = CutNet->GetSegment(Seg);
		TestTrue(TEXT("solved flags survive"), Solved->bSolvedA && Solved->bSolvedB);
		// Bitwise, not Equals(). These values are the shared truth.
		TestTrue(TEXT("left cut A stored exactly"),
			Solved->LeftCutA.X == 1150.0 && Solved->LeftCutA.Y == 1150.0);
		TestTrue(TEXT("right cut B stored exactly"),
			Solved->RightCutB.X == 8850.0 && Solved->RightCutB.Y == 1150.0);
	}

	// --- NodeIdAt/SegmentIdAt/GuidelineEdgeIdAt/ApronIdAt: dead, out-of-range, and live (#79 review) ---
	//
	// Every one of these is RoadSlot::HandleAt, already proven generically by
	// Airside.Model.SlotMap - this pins the four call sites themselves rather than trusting
	// each forwards to it correctly, since a copy-paste of the wrong array would compile and
	// silently answer for a different collection.
	{
		URoadNetwork* IdAtNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* IdAtProfile = URoadProfile::MakeTransient(2300.0, 1500.0);

		const FRoadNodeId IdAtA = IdAtNet->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId IdAtB = IdAtNet->AddNode(FVector2D(1000.0, 0.0));
		const FRoadSegmentId IdAtSeg = IdAtNet->AddStraightSegment(IdAtA, IdAtB, IdAtProfile);
		const FGuidelineNodeId IdAtGA = IdAtNet->AddGuidelineNode(FVector2D(0.0, 0.0), /*bDerived*/ false);
		const FGuidelineNodeId IdAtGB = IdAtNet->AddGuidelineNode(FVector2D(1000.0, 0.0), /*bDerived*/ false);
		FGuidelineEdge IdAtEdgeIn;
		IdAtEdgeIn.A = IdAtGA;
		IdAtEdgeIn.B = IdAtGB;
		const FGuidelineEdgeId IdAtEdge = IdAtNet->AddGuidelineEdge(MoveTemp(IdAtEdgeIn));
		const FApronId IdAtApron = IdAtNet->AddApron(FApronSurface());

		TestTrue(TEXT("fixture built"), IdAtSeg.IsSet() && IdAtEdge.IsSet() && IdAtApron.IsSet());

		// Live: the returned handle names the same slot AND the same generation as the one
		// the Add* call itself handed back - not merely "some" live handle at that index.
		TestTrue(TEXT("NodeIdAt live matches Add's own handle"), IdAtNet->NodeIdAt(IdAtA.Index) == IdAtA);
		TestEqual(TEXT("NodeIdAt live generation"), IdAtNet->NodeIdAt(IdAtA.Index).Generation, IdAtA.Generation);
		TestTrue(TEXT("SegmentIdAt live matches Add's own handle"), IdAtNet->SegmentIdAt(IdAtSeg.Index) == IdAtSeg);
		TestEqual(TEXT("SegmentIdAt live generation"), IdAtNet->SegmentIdAt(IdAtSeg.Index).Generation, IdAtSeg.Generation);
		TestTrue(TEXT("GuidelineEdgeIdAt live matches Add's own handle"), IdAtNet->GuidelineEdgeIdAt(IdAtEdge.Index) == IdAtEdge);
		TestEqual(TEXT("GuidelineEdgeIdAt live generation"), IdAtNet->GuidelineEdgeIdAt(IdAtEdge.Index).Generation, IdAtEdge.Generation);
		TestTrue(TEXT("ApronIdAt live matches Add's own handle"), IdAtNet->ApronIdAt(IdAtApron.Index) == IdAtApron);
		TestEqual(TEXT("ApronIdAt live generation"), IdAtNet->ApronIdAt(IdAtApron.Index).Generation, IdAtApron.Generation);

		// Out of range: -1 and exactly Num() (one past the last valid index).
		TestFalse(TEXT("NodeIdAt(-1) unset"), IdAtNet->NodeIdAt(-1).IsSet());
		TestFalse(TEXT("NodeIdAt(Num()) unset"), IdAtNet->NodeIdAt(IdAtNet->GetNodes().Num()).IsSet());
		TestFalse(TEXT("SegmentIdAt(-1) unset"), IdAtNet->SegmentIdAt(-1).IsSet());
		TestFalse(TEXT("SegmentIdAt(Num()) unset"), IdAtNet->SegmentIdAt(IdAtNet->GetSegments().Num()).IsSet());
		TestFalse(TEXT("GuidelineEdgeIdAt(-1) unset"), IdAtNet->GuidelineEdgeIdAt(-1).IsSet());
		TestFalse(TEXT("GuidelineEdgeIdAt(Num()) unset"),
			IdAtNet->GuidelineEdgeIdAt(IdAtNet->GetGuidelineEdges().Num()).IsSet());
		TestFalse(TEXT("ApronIdAt(-1) unset"), IdAtNet->ApronIdAt(-1).IsSet());
		TestFalse(TEXT("ApronIdAt(Num()) unset"), IdAtNet->ApronIdAt(IdAtNet->GetAprons().Num()).IsSet());

		// Dead: a valid INDEX whose slot is no longer alive.
		const int32 DeadNodeIndex = IdAtB.Index;
		const int32 DeadSegmentIndex = IdAtSeg.Index;
		const int32 DeadEdgeIndex = IdAtEdge.Index;
		const int32 DeadApronIndex = IdAtApron.Index;
		IdAtNet->RemoveGuidelineEdge(IdAtEdge);
		IdAtNet->RemoveApron(IdAtApron);
		IdAtNet->RemoveSegment(IdAtSeg);
		IdAtNet->RemoveNode(IdAtB);

		TestFalse(TEXT("NodeIdAt(dead) unset"), IdAtNet->NodeIdAt(DeadNodeIndex).IsSet());
		TestFalse(TEXT("SegmentIdAt(dead) unset"), IdAtNet->SegmentIdAt(DeadSegmentIndex).IsSet());
		TestFalse(TEXT("GuidelineEdgeIdAt(dead) unset"), IdAtNet->GuidelineEdgeIdAt(DeadEdgeIndex).IsSet());
		TestFalse(TEXT("ApronIdAt(dead) unset"), IdAtNet->ApronIdAt(DeadApronIndex).IsSet());
	}

	return true;
}

/**
 * THE graph-edge call of GuidelineGeom::Sample, introduced by issue #105 item 5 to replace
 * a dozen near-identical bodies in RouteSearch/NodeReach/GuidelineOverlay/AnchorLink/
 * AnchorLinkFinder/StandLaneBuild that each fetched A/B themselves (the one documented
 * exception went with the declared entry on 2026-09-16 - see URoadNetwork::SampleGuideline's
 * own comment). Fails if SampleGuideline ever stops resolving the edge, or if bFromB stops
 * being the "walked from B" curve.
 */
// "Airside.Model.Network.SampleGuideline", not a child of the "Network" test above: that
// leaf/parent collision is exactly what RouteSearchTest.cpp's own comment warns about - UE's
// automation report tree cannot have "Network" be both a leaf and a parent, and the leaf
// silently stops running the moment a child is registered under it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkSampleGuidelineTest,
	"Airside.Model.Guideline.SampleGuideline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkSampleGuidelineTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());

	const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
	const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));

	FGuidelineEdge Edge;
	Edge.A = A;
	Edge.B = B;
	Edge.Control = FVector2D(500.0, 400.0);
	Edge.AllowedTraffic = FTrafficMask::All();
	Edge.Direction = EGuidelineDir::Bidirectional;
	const FGuidelineEdgeId EdgeId = Net->AddGuidelineEdge(MoveTemp(Edge));
	TestTrue(TEXT("edge added"), EdgeId.IsSet());

	TArray<FVector2D> Forward;
	TestTrue(TEXT("samples A-to-B"), Net->SampleGuideline(EdgeId, Forward));
	TestTrue(TEXT("more than the two endpoints"), Forward.Num() > 2);
	TestTrue(TEXT("starts at A"), Forward[0].Equals(FVector2D(0.0, 0.0), 1e-6));
	TestTrue(TEXT("ends at B"), Forward.Last().Equals(FVector2D(1000.0, 0.0), 1e-6));

	// bFromB swaps the ends handed to GuidelineGeom::Sample rather than sampling then
	// reversing - see SampleGuideline's own comment for the algebra. Both give the SAME
	// polyline, point for point, walked from the other end.
	TArray<FVector2D> Backward;
	TestTrue(TEXT("samples B-to-A"), Net->SampleGuideline(EdgeId, Backward, /*bFromB=*/true));
	TestEqual(TEXT("same number of points either direction"), Backward.Num(), Forward.Num());
	TestTrue(TEXT("starts at B"), Backward[0].Equals(FVector2D(1000.0, 0.0), 1e-6));
	TestTrue(TEXT("ends at A"), Backward.Last().Equals(FVector2D(0.0, 0.0), 1e-6));
	for (int32 Index = 0; Index < Forward.Num(); ++Index)
	{
		TestTrue(*FString::Printf(TEXT("point %d matches reversed"), Index),
			Forward[Index].Equals(Backward[Backward.Num() - 1 - Index], 1e-6));
	}

	// APPENDS, not clears - a route is built by sampling each edge into one running array
	// (RouteSearch's own comment), so a caller handing it a non-empty array must keep what
	// was already there.
	TArray<FVector2D> Appended;
	Appended.Add(FVector2D(-1.0, -1.0));
	TestTrue(TEXT("appends onto a non-empty array"), Net->SampleGuideline(EdgeId, Appended));
	TestTrue(TEXT("the pre-existing point survives"), Appended[0].Equals(FVector2D(-1.0, -1.0), 1e-6));

	TArray<FVector2D> Missing;
	TestFalse(TEXT("an unset edge id refuses"), Net->SampleGuideline(FGuidelineEdgeId(), Missing));

	Net->RemoveGuidelineEdge(EdgeId);
	TArray<FVector2D> Removed;
	TestFalse(TEXT("a removed edge id refuses"), Net->SampleGuideline(EdgeId, Removed));

	return true;
}

/**
 * The narrow mutators #191 put in place of the three raw *Mutable accessors, each pinning
 * the ONE invariant that accessor let a caller skip:
 *   - RelinkGuidelineEdge fixes Incident at all four nodes - the repair a raw FGuidelineEdge*
 *     writing A/B directly could bypass. Untested before this (grep found no caller in
 *     AirsideTests/), and it is now the ONLY way to move an edge's endpoints at all, since
 *     GetGuidelineEdgeMutable is private.
 *   - WriteSegmentEndSolve/ClearSegmentEndSolve write TrimA/B, the cut vertices and
 *     bSolvedA/B TOGETHER, so a segment can never report bSolved true over a stale vertex -
 *     the failure this pins is one write landing without the others.
 *   - SetGuidelineNodeOrigin and SetGuidelineNodeHoldingPosition each write their whole
 *     multi-field fact in one call, so a caller cannot leave GuidelineIndex stale beside a
 *     fresh Segment/bEndA, or a Runway HoldingPosition beside a cleared HoldingPositionFor.
 *
 * "Airside.Model.Guideline.NarrowMutators", a distinct leaf beside SampleGuideline above -
 * see that test's own comment on why "Network" cannot be reused as both leaf and parent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadNetworkNarrowMutatorsTest,
	"Airside.Model.Guideline.NarrowMutators",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadNetworkNarrowMutatorsTest::RunTest(const FString& Parameters)
{
	// --- RelinkGuidelineEdge fixes Incident at all four nodes ---
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FGuidelineNodeId A = Net->AddGuidelineNode(FVector2D(0.0, 0.0));
		const FGuidelineNodeId B = Net->AddGuidelineNode(FVector2D(1000.0, 0.0));
		const FGuidelineNodeId C = Net->AddGuidelineNode(FVector2D(0.0, 1000.0));
		const FGuidelineNodeId D = Net->AddGuidelineNode(FVector2D(1000.0, 1000.0));

		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		const FGuidelineEdgeId EdgeId = Net->AddGuidelineEdge(MoveTemp(Edge));
		TestTrue(TEXT("edge added"), EdgeId.IsSet());
		TestTrue(TEXT("A starts incident"), Net->GetGuidelineNode(A)->Incident.Contains(EdgeId));
		TestTrue(TEXT("B starts incident"), Net->GetGuidelineNode(B)->Incident.Contains(EdgeId));

		TestTrue(TEXT("relink to C/D succeeds"), Net->RelinkGuidelineEdge(EdgeId, C, D));

		// The invariant a raw FGuidelineEdge* write could skip: the OLD ends must drop the
		// edge, not just the new ones pick it up - a stale entry here is exactly the "walks
		// them assuming they are [in sync]" trap URoadNetwork::SetNodePosition warns about
		// for the road graph's own Incident list.
		TestFalse(TEXT("A no longer incident"), Net->GetGuidelineNode(A)->Incident.Contains(EdgeId));
		TestFalse(TEXT("B no longer incident"), Net->GetGuidelineNode(B)->Incident.Contains(EdgeId));
		TestTrue(TEXT("C now incident"), Net->GetGuidelineNode(C)->Incident.Contains(EdgeId));
		TestTrue(TEXT("D now incident"), Net->GetGuidelineNode(D)->Incident.Contains(EdgeId));

		const FGuidelineEdge* Relinked = Net->GetGuidelineEdge(EdgeId);
		TestEqual(TEXT("edge now points at C"), Relinked->A, C);
		TestEqual(TEXT("edge now points at D"), Relinked->B, D);
	}

	// --- WriteSegmentEndSolve / ClearSegmentEndSolve write TrimA/B, the cut vertices and
	//     bSolvedA/B together, and only for the end named ---
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);
		const FRoadNodeId P = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId Q = Net->AddNode(FVector2D(10000.0, 0.0));
		const FRoadSegmentId Seg = Net->AddStraightSegment(P, Q, Profile);

		FJunctionArmResult SolveA;
		SolveA.CutDistance = 500.0;
		SolveA.LeftCut = FVector2D(500.0, 500.0);
		SolveA.RightCut = FVector2D(500.0, -500.0);
		TestTrue(TEXT("writes A end"), Net->WriteSegmentEndSolve(Seg, /*bEndA=*/true, SolveA));

		const FRoadSegment* AfterA = Net->GetSegment(Seg);
		TestTrue(TEXT("A end reports solved"), AfterA->bSolvedA);
		TestFalse(TEXT("B end untouched"), AfterA->bSolvedB);
		TestEqual(TEXT("TrimA written"), AfterA->TrimA, 500.0);
		TestTrue(TEXT("LeftCutA written"), AfterA->LeftCutA.Equals(FVector2D(500.0, 500.0)));
		TestTrue(TEXT("RightCutA written"), AfterA->RightCutA.Equals(FVector2D(500.0, -500.0)));

		// A failed solve at this end must stop reporting it live WITHOUT zeroing the vertices
		// a mesh builder already trusted from the write above (see bSolvedA's own comment).
		TestTrue(TEXT("clears A end"), Net->ClearSegmentEndSolve(Seg, /*bEndA=*/true));
		const FRoadSegment* AfterClear = Net->GetSegment(Seg);
		TestFalse(TEXT("A end no longer reports solved"), AfterClear->bSolvedA);
		TestTrue(TEXT("LeftCutA left as the last solve wrote it"),
			AfterClear->LeftCutA.Equals(FVector2D(500.0, 500.0)));

		TestFalse(TEXT("dead segment refuses a write"),
			Net->WriteSegmentEndSolve(FRoadSegmentId(), true, SolveA));
	}

	// --- SetGuidelineNodeOrigin overwrites all three FGuidelineEndRef fields together ---
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Profile = URoadProfile::MakeTransient(2300.0, 1500.0);
		// Real segment ids, not hand-built handles (#79/#173) - Origin.Segment is compared
		// by IsSet/Generation like any other handle, and a fabricated one would not pin
		// anything about what this mutator actually does.
		const FRoadSegmentId SegA = Net->AddStraightSegment(
			Net->AddNode(FVector2D(0.0, 0.0)), Net->AddNode(FVector2D(1000.0, 0.0)), Profile);
		const FRoadSegmentId SegB = Net->AddStraightSegment(
			Net->AddNode(FVector2D(0.0, 1000.0)), Net->AddNode(FVector2D(1000.0, 1000.0)), Profile);
		const FGuidelineNodeId Node = Net->AddGuidelineNode(FVector2D(0.0, 0.0));

		FGuidelineEndRef First;
		First.Segment = SegA;
		First.bEndA = true;
		First.GuidelineIndex = 3;
		TestTrue(TEXT("sets Origin"), Net->SetGuidelineNodeOrigin(Node, First));
		TestTrue(TEXT("Origin reads back"), Net->GetGuidelineNode(Node)->Origin == First);

		// A later write must replace EVERY field, not just the ones that differ - the raw
		// pointer this replaced let a caller update Segment/bEndA and leave GuidelineIndex
		// from a slot's previous life.
		FGuidelineEndRef Second;
		Second.Segment = SegB;
		Second.bEndA = false;
		Second.GuidelineIndex = 0;
		TestTrue(TEXT("overwrites Origin"), Net->SetGuidelineNodeOrigin(Node, Second));
		const FGuidelineEndRef Read = Net->GetGuidelineNode(Node)->Origin;
		TestTrue(TEXT("Origin now the second value"), Read == Second);
		TestFalse(TEXT("stale GuidelineIndex did not survive"), Read == First);

		TestFalse(TEXT("dead node refuses a write"), Net->SetGuidelineNodeOrigin(FGuidelineNodeId(), First));
	}

	// --- SetGuidelineNodeHoldingPosition writes Kind and For together, and leaves marks
	//     alone - unlike SetIntermediateHoldingPosition, which is the player's entry point ---
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* RunwayProfile = URoadProfile::MakeTransient(4500.0, 1500.0);
		RunwayProfile->bContinuousThroughJunctions = true;
		const FRoadNodeId RP = Net->AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId RQ = Net->AddNode(FVector2D(20000.0, 0.0));
		const FRoadSegmentId Runway = Net->AddStraightSegment(RP, RQ, RunwayProfile);
		const FGuidelineNodeId Node = Net->AddGuidelineNode(FVector2D(0.0, 0.0));

		TestTrue(TEXT("sets Runway + For together"),
			Net->SetGuidelineNodeHoldingPosition(Node, EHoldingPositionKind::Runway, Runway));
		const FGuidelineNode* Set = Net->GetGuidelineNode(Node);
		TestEqual(TEXT("Kind written"), Set->HoldingPosition, EHoldingPositionKind::Runway);
		TestEqual(TEXT("For written"), Set->HoldingPositionFor, Runway);
		TestEqual(TEXT("no mark recorded - the builder owns marks itself"),
			Net->GetHoldingPositionMarks().Num(), 0);

		TestTrue(TEXT("clears Kind and For together"),
			Net->SetGuidelineNodeHoldingPosition(Node, EHoldingPositionKind::None, FRoadSegmentId()));
		const FGuidelineNode* Cleared = Net->GetGuidelineNode(Node);
		TestEqual(TEXT("Kind cleared"), Cleared->HoldingPosition, EHoldingPositionKind::None);
		TestFalse(TEXT("For cleared too - never left set beside a None kind"), Cleared->HoldingPositionFor.IsSet());

		TestFalse(TEXT("dead node refuses a write"),
			Net->SetGuidelineNodeHoldingPosition(FGuidelineNodeId(), EHoldingPositionKind::Runway, Runway));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTaxiwayStripQueryTest,
	"Airside.Model.TaxiwayStrip.Query",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiwayStripQueryTest::RunTest(const FString& Parameters)
{
	// A box 40 m wide whose near edge sits at NearY, running 40 m further from the road.
	auto BoxFrom = [](double NearY)
	{
		return TArray<FVector2D>{ { -2000.0, NearY }, { 2000.0, NearY }, { 2000.0, NearY + 4000.0 }, { -2000.0, NearY + 4000.0 } };
	};
	auto Lay = [](URoadNetwork* Net, URoadProfile* Profile, const FVector2D& Control)
	{
		const FRoadNodeId W = Net->AddNode(FVector2D(-10000.0, 0.0));
		const FRoadNodeId E = Net->AddNode(FVector2D(10000.0, 0.0));
		return Net->AddSegment(W, E, Control, Profile);
	};

	// STRAIGHT 24 m TAXIWAY: code E, pavement edge at 1200, strip edge at 1200 + 2800 = 4000.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Taxi = Lay(Net, URoadProfile::MakeTransient(2400.0, 1600.0), FVector2D::ZeroVector);
		TestEqual(TEXT("its strip is E's 28 m"), TaxiwayStrip::StripWidthOf(*Net, Taxi), 2800.0, 0.5);
		TestFalse(TEXT("flush at the strip edge: clear - float noise must not refuse it"),
			TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(4000.0)).IsSet());
		const TOptional<TaxiwayStrip::FIntrusion> OneMetre = TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(3900.0));
		if (TestTrue(TEXT("a metre inside the strip intrudes"), OneMetre.IsSet()))
		{
			TestEqual(TEXT("by a metre"), OneMetre->Depth, 100.0, 1.0);
			TestEqual(TEXT("naming the letter that set it"), static_cast<int32>(OneMetre->Letter), static_cast<int32>(EIcaoCode::E));
			TestEqual(TEXT("and what it needs"), OneMetre->Required, 2800.0, 0.5);
		}
		const TOptional<TaxiwayStrip::FIntrusion> Flush = TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(1200.0));
		TestTrue(TEXT("flush to the pavement - today's stand - intrudes by the whole strip"),
			Flush.IsSet() && FMath::IsNearlyEqual(Flush->Depth, 2800.0, 1.0));
	}

	// A BEND: control (0, 10000) puts the curve's midpoint at y 5000. A chord test would see
	// the road at y 0 and pass a box at 8900; the curve's strip edge is 5000 + 1200 + 2800 = 9000.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Lay(Net, URoadProfile::MakeTransient(2400.0, 1600.0), FVector2D(0.0, 10000.0));
		TestTrue(TEXT("the strip follows the bend, not the chord"),
			TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(8900.0)).IsSet());
		TestFalse(TEXT("and clears past it"), TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(9100.0)).IsSet());
	}

	// NO STRIP: a service road (trucks), and a runway (its own rules - out of scope).
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Road = Lay(Net, URoadProfile::MakeServiceRoadTransient(), FVector2D::ZeroVector);
		TestFalse(TEXT("a service road has no strip"), TaxiwayStrip::HasStrip(*Net, Road));
		TestFalse(TEXT("so nothing beside it intrudes"), TaxiwayStrip::WorstIntrusion(*Net, BoxFrom(400.0)).IsSet());

		URoadNetwork* RunwayNet = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* RunwayProfile = URoadProfile::MakeTransient(4600.0, 1600.0);
		RunwayProfile->bContinuousThroughJunctions = true;   // IsRunwaySegment's own rule
		const FRoadSegmentId Runway = Lay(RunwayNet, RunwayProfile, FVector2D::ZeroVector);
		TestFalse(TEXT("a runway has no taxiway strip"), TaxiwayStrip::HasStrip(*RunwayNet, Runway));
	}

	// TWO TAXIWAYS: a 12 m B along Y=0 and a 26 m F along X=6000. A box 1 m inside B's strip
	// and 10 m inside F's reports F - the deeper - so the readout names the one to fix first.
	// B strip edge at 600 + 900 = 1500 in Y; F strip edge at 6000 - 1300 - 3450 = 1250 in X.
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Lay(Net, URoadProfile::MakeTransient(1200.0, 800.0), FVector2D::ZeroVector);
		const FRoadNodeId S = Net->AddNode(FVector2D(6000.0, -10000.0));
		const FRoadNodeId N = Net->AddNode(FVector2D(6000.0, 10000.0));
		const FRoadSegmentId Wide = Net->AddSegment(S, N, FVector2D(6000.0, 0.0), URoadProfile::MakeTransient(2600.0, 1733.0));
		const TArray<FVector2D> Box{ { -2000.0, 1400.0 }, { 2250.0, 1400.0 }, { 2250.0, 5400.0 }, { -2000.0, 5400.0 } };
		const TOptional<TaxiwayStrip::FIntrusion> Worst = TaxiwayStrip::WorstIntrusion(*Net, Box);
		if (TestTrue(TEXT("the box intrudes"), Worst.IsSet()))
		{
			TestTrue(TEXT("the deeper intrusion wins"), Worst->Taxiway == Wide);
			TestEqual(TEXT("by 10 m"), Worst->Depth, 1000.0, 1.0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayStripReloadedProfileTest, "Airside.Model.TaxiwayStrip.ReloadedSegmentHasAStrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayStripReloadedProfileTest::RunTest(const FString&)
{
	// A SEGMENT RELOADED FROM A SAVED LEVEL carries a null Profile when it was laid with the
	// actor's transient fallback (RoadSurfacePresenter's DefaultProfile comment); ProfileFor
	// repairs it. The strip must be read through the same accessor, or a reloaded map has none.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	Net->DefaultProfile = URoadProfile::MakeTransient(2400.0, 1600.0);
	const FRoadSegmentId Taxi = Net->AddSegment(Net->AddNode({ -10000.0, 0.0 }), Net->AddNode({ 10000.0, 0.0 }),
		FVector2D::ZeroVector, nullptr);
	TestEqual(TEXT("the reloaded taxiway still has E's strip"), TaxiwayStrip::StripWidthOf(*Net, Taxi), 2800.0, 0.5);
	const TArray<FVector2D> Flush{ { -2000.0, 1200.0 }, { 2000.0, 1200.0 }, { 2000.0, 5200.0 }, { -2000.0, 5200.0 } };
	TestTrue(TEXT("and a stand flush to it intrudes"), TaxiwayStrip::WorstIntrusion(*Net, Flush).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayStripFootprintTest, "Airside.Model.TaxiwayStrip.Footprint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayStripFootprintTest::RunTest(const FString&)
{
	// THE GROUND A SEGMENT COVERS, which every stage-3 placement judges: the strip query
	// measures it, and a new taxiway's own strip is this with a wider half.
	{
		TaxiwayStrip::FSegmentShape Straight;
		Straight.A = { 0.0, 0.0 };
		Straight.B = { 20000.0, 0.0 };
		Straight.Control = (Straight.A + Straight.B) * 0.5;
		Straight.HalfWidth = 300.0;
		const TArray<FVector2D> Poly = TaxiwayStrip::FootprintOf(Straight);
		TestEqual(TEXT("both edges, every sample - the curve's own sampling, not a special case"),
			Poly.Num(), 2 * (GuidelineGeom::DefaultSamples + 1));
		TestTrue(TEXT("counter-clockwise, so a triangulator or a winding test reads it the right way up"),
			RoadGeom::PolygonArea(Poly) > 0.0);
		TestEqual(TEXT("200 m by 6 m of ground"), RoadGeom::PolygonArea(Poly), 20000.0 * 600.0, 20000.0 * 600.0 * 0.001);
	}
	{
		// A BEND: every edge point is HalfWidth off the centreline, measured against the SAME
		// Eval samples - an offset built from differenced samples would drift on the bend.
		TaxiwayStrip::FSegmentShape Bend;
		Bend.A = { -10000.0, 0.0 };
		Bend.Control = { 0.0, 10000.0 };
		Bend.B = { 10000.0, 0.0 };
		Bend.HalfWidth = 300.0;
		const TArray<FVector2D> Poly = TaxiwayStrip::FootprintOf(Bend);
		TestTrue(TEXT("the bend is CCW too"), RoadGeom::PolygonArea(Poly) > 0.0);
		TArray<FVector2D> Centre;
		for (int32 S = 0; S <= GuidelineGeom::DefaultSamples; ++S)
		{
			Centre.Add(GuidelineGeom::Eval(Bend.A, Bend.Control, Bend.B, static_cast<double>(S) / GuidelineGeom::DefaultSamples));
		}
		double Worst = 0.0;
		for (const FVector2D& P : Poly)
		{
			double Nearest = DBL_MAX;
			for (int32 C = 0; C + 1 < Centre.Num(); ++C)
			{
				const double T = RoadGeom::ClosestPointOnSegment(Centre[C], Centre[C + 1], P);
				Nearest = FMath::Min(Nearest, FVector2D::Distance(P, Centre[C] + (Centre[C + 1] - Centre[C]) * T));
			}
			Worst = FMath::Max(Worst, FMath::Abs(Nearest - Bend.HalfWidth));
		}
		// ONE CENTIMETRE: the chord between two samples sits inside the curve by its sagitta's
		// slope, ~0.7 uu at this 100 m radius (2026-09-29); an edge built off anything but the
		// analytic normal misses by metres.
		TestTrue(FString::Printf(TEXT("every edge point is HalfWidth off the sampled centreline (worst %.3f uu off)"), Worst),
			Worst <= 1.0);
	}
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Net->DefaultProfile = URoadProfile::MakeTransient(2400.0, 1600.0);
		const FRoadSegmentId Seg = Net->AddSegment(Net->AddNode({ 0.0, 0.0 }), Net->AddNode({ 10000.0, 0.0 }),
			{ 5000.0, 0.0 }, nullptr);
		TaxiwayStrip::FSegmentShape Shape;
		TestTrue(TEXT("a live segment has a shape"), TaxiwayStrip::ShapeOf(*Net, Seg, Shape));
		TestEqual(TEXT("its half-width is read through ProfileFor"), Shape.HalfWidth, 1200.0, 0.01);
		TestFalse(TEXT("a dead handle has none"), TaxiwayStrip::ShapeOf(*Net, FRoadSegmentId(), Shape));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayStripSegmentJudgeTest, "Airside.Model.TaxiwayStrip.SegmentJudge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayStripSegmentJudgeTest::RunTest(const FString&)
{
	using namespace TaxiwayStrip;

	// A 24 m (code E) taxiway W-E along y 0 in two pieces, meeting at a node at the origin:
	// pavement edge 1200, strip edge 1200 + 2800 = 4000 off the centreline.
	struct FFixture
	{
		URoadNetwork* Net = nullptr;
		FRoadNodeId W, Mid, E;
		FRoadSegmentId West, East;
	};
	auto MakeTaxiway = []()
	{
		FFixture F;
		F.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = URoadProfile::MakeTransient(2400.0, 1600.0);
		F.W = F.Net->AddNode({ -10000.0, 0.0 });
		F.Mid = F.Net->AddNode({ 0.0, 0.0 });
		F.E = F.Net->AddNode({ 10000.0, 0.0 });
		F.West = F.Net->AddStraightSegment(F.W, F.Mid, Taxi);
		F.East = F.Net->AddStraightSegment(F.Mid, F.E, Taxi);
		return F;
	};
	auto Shape = [](const FVector2D& A, const FVector2D& B, double HalfWidth)
	{
		FSegmentShape S;
		S.A = A;
		S.B = B;
		S.Control = (A + B) * 0.5;
		S.HalfWidth = HalfWidth;
		return S;
	};
	auto AtNode = [](const URoadNetwork& Net, FRoadNodeId Node)
	{
		FSegmentEnd End;
		End.Node = Node;
		End.At = Net.GetNode(Node)->Position;
		return End;
	};
	auto Free = [](const FVector2D& At)
	{
		FSegmentEnd End;
		End.At = At;
		return End;
	};
	auto Dir = [](double Degrees)
	{
		return FVector2D(FMath::Cos(FMath::DegreesToRadians(Degrees)), FMath::Sin(FMath::DegreesToRadians(Degrees)));
	};
	constexpr double Road = 300.0;   // a 6 m service road's half

	{
		const FFixture F = MakeTaxiway();
		const FVector2D To(0.0, 10000.0);
		const FStripVerdict V = JudgeSegment(*F.Net, Shape({ 0.0, 0.0 }, To, Road), false, AtNode(*F.Net, F.Mid), Free(To));
		TestFalse(FString::Printf(TEXT("right-angle road meeting the taxiway at a node is allowed (%s)"), *V.Text), V.bRefused);
	}
	{
		const FFixture F = MakeTaxiway();
		const FVector2D To = Dir(20.0) * 10000.0;
		const FStripVerdict V = JudgeSegment(*F.Net, Shape({ 0.0, 0.0 }, To, Road), false, AtNode(*F.Net, F.Mid), Free(To));
		TestTrue(TEXT("a road meeting at 20 degrees runs along the strip - refused"), V.bRefused);
		TestTrue(FString::Printf(TEXT("and says why in the strip's own words (%s)"), *V.Text), V.Text.Contains(TEXT("clearance strip")));
	}
	{
		const FFixture F = MakeTaxiway();
		const FStripVerdict V = JudgeSegment(*F.Net, Shape({ -5000.0, 2000.0 }, { 5000.0, 2000.0 }, Road), false,
			Free({ -5000.0, 2000.0 }), Free({ 5000.0, 2000.0 }));
		TestTrue(TEXT("a road alongside, 20 m off, meeting nothing - refused"), V.bRefused);
		TestTrue(FString::Printf(TEXT("inside the clearance strip (%s)"), *V.Text), V.Text.Contains(TEXT("clearance strip")));
	}
	{
		const FFixture F = MakeTaxiway();
		const FStripVerdict V = JudgeSegment(*F.Net, Shape({ 3000.0, -8000.0 }, { 3000.0, 8000.0 }, Road), false,
			Free({ 3000.0, -8000.0 }), Free({ 3000.0, 8000.0 }));
		TestTrue(TEXT("a road across the taxiway with free ends - refused"), V.bRefused);
		TestTrue(FString::Printf(TEXT("naming the missing junction (%s)"), *V.Text), V.Text.Contains(TEXT("junction")));
	}
	{
		// RULING 3: extending a taxiway straight on from its end node is a meeting, or the rule
		// would forbid lengthening one.
		const FFixture F = MakeTaxiway();
		const FVector2D To(20000.0, 0.0);
		const FStripVerdict V = JudgeSegment(*F.Net, Shape({ 10000.0, 0.0 }, To, 1200.0), true, AtNode(*F.Net, F.E), Free(To));
		TestFalse(FString::Printf(TEXT("a taxiway continuing straight on is allowed (%s)"), *V.Text), V.bRefused);

		// A BEND AT THE DEAD END, 45 degrees off straight: it heads away from the only arm there,
		// so it runs along no strip. The plan's 150-degree band refused it while admitting a 90.
		const FVector2D Bent = FVector2D(10000.0, 0.0) + Dir(45.0) * 10000.0;
		const FStripVerdict B = JudgeSegment(*F.Net, Shape({ 10000.0, 0.0 }, Bent, 1200.0), true, AtNode(*F.Net, F.E), Free(Bent));
		TestFalse(FString::Printf(TEXT("a taxiway chained on with a 45-degree bend is allowed (%s)"), *B.Text), B.bRefused);
		const FVector2D Back = FVector2D(10000.0, 0.0) + Dir(160.0) * 10000.0;
		const FStripVerdict R = JudgeSegment(*F.Net, Shape({ 10000.0, 0.0 }, Back, Road), false, AtNode(*F.Net, F.E), Free(Back));
		TestTrue(TEXT("but one doubling back 20 degrees off its arm is refused"), R.bRefused);
	}
	{
		// REVIEW FOCUS 2: a node where two taxiways join - the road must meet EACH at an allowed
		// angle. Square to the W-E taxiway, 15 degrees off the one leaving at 105 degrees.
		FFixture F = MakeTaxiway();
		const FRoadNodeId Far = F.Net->AddNode(Dir(105.0) * 10000.0);
		F.Net->AddStraightSegment(F.Mid, Far, URoadProfile::MakeTransient(2400.0, 1600.0));
		const FVector2D To(0.0, 10000.0);
		const FStripVerdict V = JudgeSegment(*F.Net, Shape({ 0.0, 0.0 }, To, Road), false, AtNode(*F.Net, F.Mid), Free(To));
		TestTrue(TEXT("square to one taxiway but 15 degrees off the other - refused"), V.bRefused);
		TestTrue(FString::Printf(TEXT("naming the angle (%s)"), *V.Text), V.Text.Contains(TEXT("15 degrees")));
	}
	{
		// A TAXIWAY IS A CHAIN, NOT A SEGMENT (spec: "meets the TAXIWAY"): a road square at node M
		// is exempt from the 10 m piece beyond M's neighbour too, which sits well inside the
		// keep-out of the road's footprint. Exempting only M's own arms refused this.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = URoadProfile::MakeTransient(2400.0, 1600.0);
		const FRoadNodeId W = Net->AddNode({ -10000.0, 0.0 });
		const FRoadNodeId M = Net->AddNode({ 0.0, 0.0 });
		const FRoadNodeId P = Net->AddNode({ 1000.0, 0.0 });
		const FRoadNodeId E = Net->AddNode({ 10000.0, 0.0 });
		Net->AddStraightSegment(W, M, Taxi);
		Net->AddStraightSegment(M, P, Taxi);
		Net->AddStraightSegment(P, E, Taxi);
		const FVector2D To(0.0, 10000.0);
		const FStripVerdict V = JudgeSegment(*Net, Shape({ 0.0, 0.0 }, To, Road), false, AtNode(*Net, M), Free(To));
		TestFalse(FString::Printf(TEXT("a square join is exempt from the whole straight chain (%s)"), *V.Text), V.bRefused);

		// BUT A CHAIN ENDS AT A CORNER: an L-shaped taxiway, and a road square to its west leg
		// 20 m short of the corner runs alongside the north leg inside its strip.
		URoadNetwork* L = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId LW = L->AddNode({ -10000.0, 0.0 });
		const FRoadNodeId LC = L->AddNode({ 0.0, 0.0 });
		const FRoadNodeId LN = L->AddNode({ 0.0, 10000.0 });
		const FRoadNodeId LJ = L->AddNode({ -2000.0, 0.0 });
		L->AddStraightSegment(LW, LJ, Taxi);
		L->AddStraightSegment(LJ, LC, Taxi);
		L->AddStraightSegment(LC, LN, Taxi);
		const FVector2D Up(-2000.0, 10000.0);
		const FStripVerdict Corner = JudgeSegment(*L, Shape({ -2000.0, 0.0 }, Up, Road), false, AtNode(*L, LJ), Free(Up));
		TestTrue(TEXT("a road 20 m beside the far leg of an L is refused - the chain stops at the corner"), Corner.bRefused);
	}
	{
		// THE CHAIN IS EXEMPT ONLY NEAR THE JOIN (final review 1): a 16-gon perimeter taxiway turns
		// 22.5 degrees at every node, within the chain's per-node limit, so an unbounded walk made
		// the whole ring one taxiway - and a road square to it from inside ran across the far
		// side unrefused.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = URoadProfile::MakeTransient(2400.0, 1600.0);
		TArray<FRoadNodeId> Ring;
		// 16, NOT THE REVIEW'S 12: a 12-gon's corners are exactly 150 degrees, which the per-node
		// limit admits or not by the last bit of a double - this test must not depend on that.
		for (int32 K = 0; K < 16; ++K)
		{
			Ring.Add(Net->AddNode(Dir(22.5 * K) * 15000.0));
		}
		for (int32 K = 0; K < 16; ++K)
		{
			Net->AddStraightSegment(Ring[K], Ring[(K + 1) % 16], Taxi);
		}
		const FVector2D Across(-20000.0, 3000.0);
		const FStripVerdict V = JudgeSegment(*Net, Shape(Dir(0.0) * 15000.0, Across, Road), false, AtNode(*Net, Ring[0]), Free(Across));
		TestTrue(TEXT("a road square to a ring taxiway is refused where it crosses the far side"), V.bRefused);
	}
	{
		// A SEGMENT SNAP ON A CURVE IS JUDGED ON THE CHORDS THE SPLIT WILL MAKE (final review 4):
		// SplitSegment replaces the curve with two straight halves, so the commit sees chord
		// tangents. 80 degrees to this quarter-arc's tangent at its midpoint is ~53 to one chord.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Arc = Net->AddSegment(Net->AddNode({ 0.0, 0.0 }), Net->AddNode({ 10000.0, 10000.0 }),
			{ 10000.0, 0.0 }, URoadProfile::MakeTransient(2400.0, 1600.0));
		FSegmentEnd OnArc;
		OnArc.Segment = Arc;
		OnArc.At = { 7500.0, 2500.0 };   // Eval at T 0.5, tangent at 45 degrees
		const FVector2D From = OnArc.At + Dir(125.0) * 6000.0;
		const FStripVerdict V = JudgeSegment(*Net, Shape(From, OnArc.At, Road), false, Free(From), OnArc);
		TestTrue(FString::Printf(TEXT("80 degrees to the curve but 53 to a chord is refused (%s)"), *V.Text), V.bRefused);
		const FVector2D Square = OnArc.At + Dir(135.0) * 6000.0;
		const FStripVerdict S = JudgeSegment(*Net, Shape(Square, OnArc.At, Road), false, Free(Square), OnArc);
		TestFalse(FString::Printf(TEXT("and one 63 degrees to both chords is allowed (%s)"), *S.Text), S.bRefused);
	}
	{
		// A SEGMENT SNAP: the end lands mid-segment on a taxiway that will be split there. The
		// met segment is named by its ORIGINAL id (Review Focus 1), and square is allowed.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadSegmentId Taxi = Net->AddStraightSegment(Net->AddNode({ -10000.0, 0.0 }), Net->AddNode({ 10000.0, 0.0 }),
			URoadProfile::MakeTransient(2400.0, 1600.0));
		FSegmentEnd OnTaxiway;
		OnTaxiway.Segment = Taxi;
		OnTaxiway.At = { 2000.0, 0.0 };
		const FVector2D From(2000.0, 10000.0);
		const FStripVerdict Square = JudgeSegment(*Net, Shape(From, { 2000.0, 0.0 }, Road), false, Free(From), OnTaxiway);
		TestFalse(FString::Printf(TEXT("a road ending square on a taxiway mid-segment is allowed (%s)"), *Square.Text), Square.bRefused);
		const FVector2D Shallow = FVector2D(2000.0, 0.0) + Dir(160.0) * 10000.0;
		const FStripVerdict Slant = JudgeSegment(*Net, Shape(Shallow, { 2000.0, 0.0 }, Road), false, Free(Shallow), OnTaxiway);
		TestTrue(TEXT("and one ending on it at 20 degrees is refused"), Slant.bRefused);
	}
	{
		// RULING 5: a new F taxiway parallel to a B. At 20 m its own pavement is in B's strip;
		// at 40 m B's 9 m strip is CLEAR of it - only the F's 34.5 m strip, looking back,
		// swallows B's pavement (4000 - 1300 - 3450 = -750: 7.5 m past B's centreline).
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Net->AddStraightSegment(Net->AddNode({ -10000.0, 0.0 }), Net->AddNode({ 10000.0, 0.0 }),
			URoadProfile::MakeTransient(1200.0, 800.0));
		const FStripVerdict Near = JudgeSegment(*Net, Shape({ -10000.0, 2000.0 }, { 10000.0, 2000.0 }, 1300.0), true,
			Free({ -10000.0, 2000.0 }), Free({ 10000.0, 2000.0 }));
		TestTrue(TEXT("an F taxiway 20 m from a B is refused"), Near.bRefused);
		const FStripVerdict Far = JudgeSegment(*Net, Shape({ -10000.0, 4000.0 }, { 10000.0, 4000.0 }, 1300.0), true,
			Free({ -10000.0, 4000.0 }), Free({ 10000.0, 4000.0 }));
		TestTrue(TEXT("at 40 m, clear of B's strip, its OWN strip still swallows B - refused"), Far.bRefused);
		TestTrue(FString::Printf(TEXT("naming what it would contain (%s)"), *Far.Text), Far.Text.Contains(TEXT("would contain a taxiway")));
		const FStripVerdict Clear = JudgeSegment(*Net, Shape({ -10000.0, 6000.0 }, { 10000.0, 6000.0 }, 1300.0), true,
			Free({ -10000.0, 6000.0 }), Free({ 10000.0, 6000.0 }));
		TestFalse(FString::Printf(TEXT("at 60 m both strips are clear - allowed (%s)"), *Clear.Text), Clear.bRefused);
	}
	{
		// A NEW TAXIWAY'S STRIP OVER A STAND already built: a stand 30 m off, clear of nothing
		// yet, is inside a new E taxiway's 40 m keep-out.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
		const FEntityInstanceId Stand = ServiceLinkFixture::PlaceStand(*Net, *Def, FVector2D(0.0, 5000.0), 0.0);
		FRoadNetworkTestAccess(*Net).SetEntityOutlineForTest(Stand, { { -2000.0, 3000.0 }, { 2000.0, 3000.0 }, { 2000.0, 7000.0 }, { -2000.0, 7000.0 } });
		const FStripVerdict V = JudgeSegment(*Net, Shape({ -10000.0, 0.0 }, { 10000.0, 0.0 }, 1200.0), true,
			Free({ -10000.0, 0.0 }), Free({ 10000.0, 0.0 }));
		TestTrue(TEXT("a taxiway whose strip would contain a stand is refused"), V.bRefused);
		// BY NUMBER (strip stage 5): entity 0 is stand 1.
		const int32 Number = Net->GetEntity(Stand)->StandNumber;
		TestTrue(TEXT("the stand's number is not its index, or the line below measures nothing"), Number != Stand.Index);
		TestTrue(FString::Printf(TEXT("naming the stand by number (%s)"), *V.Text), V.Text.Contains(FString::Printf(TEXT("stand %d"), Number)));
	}
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		Net->AddStraightSegment(Net->AddNode({ -10000.0, 50000.0 }), Net->AddNode({ 10000.0, 50000.0 }),
			URoadProfile::MakeServiceRoadTransient());
		const FStripVerdict V = JudgeSegment(*Net, Shape({ -5000.0, 2000.0 }, { 5000.0, 2000.0 }, Road), false,
			Free({ -5000.0, 2000.0 }), Free({ 5000.0, 2000.0 }));
		TestFalse(TEXT("a service road with no taxiway near is allowed"), V.bRefused);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiwayRestrictionTest, "Airside.Model.TaxiwayRestriction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FTaxiwayRestrictionTest::RunTest(const FString&)
{
	// STAGE 6: A TAXIWAY WHOSE STRIP HAS GROWN OVER SOMETHING OPERATES AT THE LARGEST LETTER
	// WHOSE STRIP IS CLEAR (spec "When a taxiway is upgraded"). Every reach below is derived from
	// IcaoCode::TaxiwayStripFor, never typed, so a re-tuned clearance moves the fixture with it.
	constexpr double Pavement = 2600.0;   // 26 m: Code F
	const double Half = 0.5 * Pavement;
	const double ReachF = Half + IcaoCode::TaxiwayStripFor(EIcaoCode::F, Pavement);
	const double ReachE = Half + IcaoCode::TaxiwayStripFor(EIcaoCode::E, Pavement);
	URoadProfile* RoadProfile = URoadProfile::MakeServiceRoadTransient();
	const double RoadHalf = RoadProfile->GetMaxHalfWidth();
	// The road's near edge between E's reach and F's: inside F's strip, clear of E's.
	const double RoadY = 0.5 * (ReachE + ReachF) + RoadHalf;

	struct FFixture
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId Taxi, Road;
	};
	auto Make = [&](bool bWithRoad)
	{
		FFixture F;
		F.Net = NewObject<URoadNetwork>(GetTransientPackage());
		F.Taxi = F.Net->AddStraightSegment(F.Net->AddNode({ -20000.0, 0.0 }), F.Net->AddNode({ 20000.0, 0.0 }),
			URoadProfile::MakeTransient(Pavement, 1733.0));
		if (bWithRoad)
		{
			// AddStraightSegment, not the facade: stage 3 would refuse to LAY this road. It is
			// what an upgrade (or a map from before stage 3) leaves behind.
			F.Road = F.Net->AddStraightSegment(F.Net->AddNode({ -3000.0, RoadY }), F.Net->AddNode({ 3000.0, RoadY }), RoadProfile);
		}
		return F;
	};

	{
		const FFixture F = Make(true);
		TaxiwayRestriction::FObstruction Worst;
		const TOptional<EIcaoCode> Letter = TaxiwayRestriction::RestrictionOf(*F.Net, F.Taxi, &Worst);
		if (TestTrue(TEXT("a service road inside F's strip restricts the taxiway"), Letter.IsSet()))
		{
			TestEqual(TEXT("to E - the largest letter whose strip is clear of it"),
				static_cast<int32>(Letter.GetValue()), static_cast<int32>(EIcaoCode::E));
		}
		TestEqual(TEXT("and names the road as the obstruction"),
			static_cast<int32>(Worst.Kind), static_cast<int32>(TaxiwayRestriction::FObstruction::EKind::Road));
		TestEqual(TEXT("by its segment index"), Worst.Index, F.Road.Index);

		TestEqual(TEXT("Apply counts one restricted taxiway"), TaxiwayRestriction::Apply(*F.Net), 1);
		TestEqual(TEXT("and writes E onto it"), static_cast<int32>(F.Net->GetSegment(F.Taxi)->RestrictedLetter),
			static_cast<int32>(EIcaoCode::E));
		TestEqual(TEXT("the road itself carries no restriction"), static_cast<int32>(F.Net->GetSegment(F.Road)->RestrictedLetter),
			static_cast<int32>(TaxiwayRestriction::Unrestricted));

		// REVIEW FOCUS 3: the obstruction removed, the restriction lifts on the next pass.
		F.Net->RemoveSegment(F.Road);
		TestEqual(TEXT("with the road deleted nothing is restricted"), TaxiwayRestriction::Apply(*F.Net), 0);
		TestEqual(TEXT("and the taxiway reads unrestricted again"), static_cast<int32>(F.Net->GetSegment(F.Taxi)->RestrictedLetter),
			static_cast<int32>(TaxiwayRestriction::Unrestricted));
	}
	{
		const FFixture F = Make(false);
		TestFalse(TEXT("nothing in the strip: unrestricted"), TaxiwayRestriction::RestrictionOf(*F.Net, F.Taxi).IsSet());
	}
	{
		// REVIEW FOCUS 2: split far from the road - each half is judged on its own ground.
		const FFixture F = Make(true);
		const FRoadNodeId Cut = F.Net->SplitSegment(F.Taxi, { 12000.0, 0.0 });
		if (TestTrue(TEXT("the split made a node"), Cut.IsSet()))
		{
			FRoadSegmentId Near, Far;
			for (const FRoadSegmentId Arm : F.Net->GetNode(Cut)->Incident)
			{
				const FRoadNode* Other = F.Net->GetNode(F.Net->GetOtherEnd(Arm, Cut));
				(Other->Position.X < 0.0 ? Near : Far) = Arm;
			}
			TestTrue(TEXT("the half beside the road is restricted"), TaxiwayRestriction::RestrictionOf(*F.Net, Near).IsSet());
			TestFalse(TEXT("the half 90 m past it is not"), TaxiwayRestriction::RestrictionOf(*F.Net, Far).IsSet());
		}
	}
	{
		// RULING 3: a stand in the strip CLOSES (StandAdmission), it never lowers the letter.
		const FFixture F = Make(false);
		UEntityDefinition* Def = UEntityDefinition::MakeStandTransient(EIcaoCode::B);
		const FEntityInstanceId Stand = ServiceLinkFixture::PlaceStand(*F.Net, *Def, FVector2D(0.0, RoadY + 2000.0), 0.0);
		FRoadNetworkTestAccess(*F.Net).SetEntityOutlineForTest(Stand,
			{ { -2000.0, RoadY - RoadHalf }, { 2000.0, RoadY - RoadHalf }, { 2000.0, RoadY + 4000.0 }, { -2000.0, RoadY + 4000.0 } });
		TestTrue(TEXT("the stand IS inside the F strip (or this proves nothing)"),
			TaxiwayStrip::WorstIntrusion(*F.Net, F.Net->GetEntity(Stand)->Outline).IsSet());
		TestFalse(TEXT("but a stand never restricts the taxiway"), TaxiwayRestriction::RestrictionOf(*F.Net, F.Taxi).IsSet());
	}
	{
		// A ROAD THAT MEETS THE TAXIWAY - square, at a node - may cross its strip (stage 3's
		// exemption, reused): a T-junction is not an obstruction.
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadProfile* Taxi = URoadProfile::MakeTransient(Pavement, 1733.0);
		const FRoadNodeId Mid = Net->AddNode({ 0.0, 0.0 });
		const FRoadSegmentId West = Net->AddStraightSegment(Net->AddNode({ -20000.0, 0.0 }), Mid, Taxi);
		const FRoadSegmentId East = Net->AddStraightSegment(Mid, Net->AddNode({ 20000.0, 0.0 }), Taxi);
		Net->AddStraightSegment(Mid, Net->AddNode({ 0.0, 20000.0 }), RoadProfile);
		TestFalse(TEXT("a road meeting square at a node does not restrict the piece it meets"),
			TaxiwayRestriction::RestrictionOf(*Net, West).IsSet());
		TestFalse(TEXT("nor the other piece of the same taxiway"), TaxiwayRestriction::RestrictionOf(*Net, East).IsSet());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
