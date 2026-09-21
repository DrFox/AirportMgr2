#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Model/BuildPurse.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Entities/EntityDefinition.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshFreshnessTest,
	"Airside.Present.MeshIsFreshAfterLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMeshFreshnessTest::RunTest(const FString& Parameters)
{
	// A DERIVED MESH THAT IS SAVED WILL GO STALE, SO A LOAD MUST REBUILD IT.
	//
	// UDynamicMeshComponent declares its mesh UPROPERTY(Instanced) with no Transient flag,
	// so the surface is serialised into the level whether we want it or not - and there is
	// no engine switch to decline. The mesh is DERIVED: the model is the truth and the
	// surface is a function of it. Persisting a derived value is a cache, and this one had
	// no invalidation at all.
	//
	// What that looked like to the user: open the level, the roads look right; run the
	// game, draw one road, and roads drawn in a PREVIOUS session change width and material.
	// Nothing had touched them. They had been wrong since the moment the level opened - the
	// picture on screen was the saved cache - and the first rebuild for any reason replaced
	// it with what the model actually said. Measured on the real level: 276 triangles saved
	// against 194 rebuilt. It read as "the road tool corrupts my roads" and cost six wrong
	// diagnoses, every one of them aimed at materials, because the change SHOWED as colour.
	//
	// The property below is the one that makes that impossible to reproduce: once loading
	// rebuilds, a later rebuild has nothing left to change.
	// A REAL WORLD, because the thing under test is a REGISTRATION hook. Every other test
	// here builds its actor with NewObject and no world, which is cheaper and enough for
	// them - but AActor::PostRegisterAllComponents dereferences the world, and calling the
	// override directly would only prove the body works, not that anything invokes it. That
	// is the mistake this codebase has now shipped three times: testing a list where it is
	// declared rather than where it is consumed.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to register components in"), TestWorld.World))
	{
		return false;
	}
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		return false;
	}

	URoadProfile* Taxiway = URoadProfile::MakeTransient(400.0, 200.0, 40.0);

	// Built through the NETWORK rather than the actor's PlaceNode/ConnectNodes, so every
	// handle in this test comes from one numbering. The actor's node indices are its own,
	// and mixing the two silently produced a segment between nodes that did not exist -
	// which showed up as a mesh that correctly did not change.
	//
	// The network is created on demand, so one call through the actor is what brings it
	// into being. The node it leaves behind is isolated and contributes no surface.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

	if (!TestNotNull(TEXT("the actor has a network"), Actor->Network.Get()))
	{
		return false;
	}
	URoadNetwork& Net = *Actor->Network;

	// 1. A BUILT SURFACE. This is the state a level is saved in.
	const FRoadNodeId A = Net.AddNode(FVector2D::ZeroVector);
	const FRoadNodeId B = Net.AddNode(FVector2D(5000.0, 0.0));
	Net.AddStraightSegment(A, B, Taxiway);
	Actor->RebuildMesh();

	const int32 Saved = Actor->GetPresenter()->SurfaceTriangleCountForTest();
	if (!TestTrue(TEXT("a connected pair of nodes builds a surface"), Saved > 0))
	{
		return false;
	}

	// 2. THE MODEL MOVES ON WITHOUT THE MESH. Adding straight to the network is how a load
	//    arrives: the graph is deserialised, and nothing has run the builder over it. Going
	//    through the actor would rebuild and hide exactly what is being measured.
	const FRoadNodeId C = Net.AddNode(FVector2D(5000.0, 5000.0));
	if (!TestTrue(TEXT("a second segment really was added to the model"),
		Net.AddStraightSegment(B, C, Taxiway).IsSet()))
	{
		return false;
	}

	TestEqual(TEXT("the mesh is now stale - it still shows the old model"),
		Actor->GetPresenter()->SurfaceTriangleCountForTest(), Saved);

	// 3. THE MEASUREMENT. Re-registering runs the same path a level load does, and it must
	//    leave the surface agreeing with the model.
	Actor->ReregisterAllComponents();

	const int32 AfterLoad = Actor->GetPresenter()->SurfaceTriangleCountForTest();
	TestNotEqual(TEXT("loading rebuilds the surface from the model, not the saved cache"),
		AfterLoad, Saved);

	// 4. AND THE PROPERTY THE USER ACTUALLY NEEDS: having loaded, drawing a road cannot
	//    change any road already there. This is the bug report, written as an assertion -
	//    a rebuild after a load is a no-op, so there is no moment at which old roads move.
	Actor->RebuildMesh();
	TestEqual(TEXT("and a later rebuild then changes nothing at all"),
		Actor->GetPresenter()->SurfaceTriangleCountForTest(), AfterLoad);

	return true;
}

// ---------------------------------------------------------------------------------------
// URoadEditFacade::OnChanged -> ARoadNetworkActor::RebuildMeshForChange (RebuildMesh() itself
// until issue #165 gave OnChanged an EChangeKind and moved the binding to the new forwarder -
// see RoadNetworkActor.cpp's constructor comment), which originally replaced four direct
// RebuildMesh() calls the facade's mutators used to make (PlaceRunway, ClearNetwork, Undo,
// Redo). Issue #77 (2026-09-13) made CommitAndNotify/NotifyChanged the single broadcast point
// for every committed edit - PlaceNode, ConnectNodes, SplitSegment, DeleteNode, DeleteSegment,
// MoveNode, AddApron, DeleteApron, PlaceEntity and DeleteEntity now notify too, and the 12
// RebuildMesh() calls the TOOLS used to make after them are gone (one of them, RoadDrawTool's
// chaining state, used to rebuild even on a REFUSED connect - see RoadEditFacade.h's own
// comment on OnChanged). Nothing asserted that the wiring still fires: every other test that
// wants a rebuilt mesh calls Actor->RebuildMesh() itself, which would still pass even if the
// constructor's Facade->OnChanged.AddUObject(this, &ARoadNetworkActor::RebuildMeshForChange)
// were deleted outright.
//
// NewObject, no world - matching Airside.Present.NetworkActor rather than
// Airside.Present.ArrivalDispatch's spawned actor: what is under test here is the delegate
// firing and the presenter rebuilding, neither of which needs PostRegisterAllComponents (that
// is MeshIsFreshAfterLoad's own reason for a real world, above).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMeshRebuildsOnFacadeChangeTest,
	"Airside.Present.MeshRebuildsOnFacadeChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMeshRebuildsOnFacadeChangeTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	// One isolated node, purely to bring the network into being - see MeshIsFreshAfterLoad's
	// own comment on PlaceNode for why this is how every test here does it. Captured: the
	// #77 section below reconnects to this exact node rather than assuming its index.
	const int32 FirstNode = Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	const int32 Before = Actor->GetPresenter()->SurfaceTriangleCountForTest();

	URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
	Runway->bContinuousThroughJunctions = true;

	// Below the default 50000 uu MinimumRunwayLength - PlaceRunway refuses under it, and
	// this test wants a short runway, not a realistic one.
	Actor->MinimumRunwayLength = 100.0;

	// PLACERUNWAY IS ONE OF THE FOUR. No Actor->RebuildMesh() call anywhere in this test -
	// if the OnChanged binding were missing, this would place a runway into the model and
	// leave the mesh exactly as stale as Before.
	TestTrue(TEXT("a runway is placed"),
		Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), Runway));

	const int32 AfterPlace = Actor->GetPresenter()->SurfaceTriangleCountForTest();
	TestTrue(TEXT("PlaceRunway's OnChanged broadcast rebuilt the mesh with no explicit "
		"RebuildMesh() call - the triangle count actually changed"), AfterPlace != Before);
	TestTrue(TEXT("and the rebuild produced real triangles, not an empty buffer"),
		AfterPlace > 0);

	// UNDO IS ANOTHER OF THE FOUR. Same absence of an explicit rebuild call: if Undo's own
	// OnChanged.Broadcast() were missing, the mesh would still show the runway after the
	// model itself had taken it back.
	TestTrue(TEXT("the placement undoes"), Actor->Undo());

	const int32 AfterUndo = Actor->GetPresenter()->SurfaceTriangleCountForTest();
	TestEqual(TEXT("Undo's OnChanged broadcast rebuilt the mesh back to what it was before "
		"the runway existed, with no explicit RebuildMesh() call either"),
		AfterUndo, Before);

	// --- Issue #77: the ten mutators that used to leave notification to the TOOL ---------
	//
	// RebuildCountForTest, not the triangle buffer, is the signal from here on: PlaceNode on
	// an isolated point and MoveNode both leave the triangle count exactly where it was (a
	// lone node draws nothing; a move keeps the same segment and profile, just shifted
	// vertices), so a test that only compared triangle counts could pass while the broadcast
	// itself was missing. A count survives both cases.
	// EXACTLY ONE rebuild per successful call, not merely "at least one" - a mutator that
	// notified twice for one edit would pass a "!=" check just as happily, and issue #77 is
	// specifically about there being ONE broadcast point per commit.
	const int32 RebuildsBeforePlaceNode = Actor->RebuildCountForTest();
	const int32 SecondNode = Actor->PlaceNode(FVector2D(2000.0, 2000.0));
	TestTrue(TEXT("a second node is placed"), SecondNode != INDEX_NONE);
	TestEqual(TEXT("PlaceNode's OnChanged broadcast rebuilt the mesh exactly once, with no "
		"explicit RebuildMesh() call from the tool"),
		Actor->RebuildCountForTest(), RebuildsBeforePlaceNode + 1);

	// A profile is needed for ConnectNodes to succeed at all - Actor has none authored, and
	// ResolveProfile's content-default fallback is exactly what every other tool-level test
	// here relies on, so this does not set one explicitly; if that ever stops resolving, the
	// TestTrue on ConnectNodes below fails loudly rather than this test silently skipping it.
	const int32 RebuildsBeforeConnect = Actor->RebuildCountForTest();
	TestTrue(TEXT("the two placed nodes connect"), Actor->ConnectNodes(FirstNode, SecondNode));
	TestEqual(TEXT("ConnectNodes's OnChanged broadcast rebuilt the mesh exactly once, with no "
		"explicit RebuildMesh() call from the tool"),
		Actor->RebuildCountForTest(), RebuildsBeforeConnect + 1);

	// A REFUSAL must rebuild ZERO times - the split-brain issue #77 fixed included a tool
	// that rebuilt even when its own ConnectNodes call was refused (RoadDrawTool's chaining
	// state). A node cannot connect to itself; nothing about the model changes.
	const int32 RebuildsBeforeRefusedConnect = Actor->RebuildCountForTest();
	TestFalse(TEXT("a node cannot connect to itself"), Actor->ConnectNodes(SecondNode, SecondNode));
	TestEqual(TEXT("the refused ConnectNodes rebuilt nothing"),
		Actor->RebuildCountForTest(), RebuildsBeforeRefusedConnect);

	const int32 RebuildsBeforeMove = Actor->RebuildCountForTest();
	TestTrue(TEXT("the second node moves"), Actor->MoveNode(SecondNode, FVector2D(2500.0, 1800.0)));
	TestEqual(TEXT("MoveNode's OnChanged broadcast rebuilt the mesh exactly once, with no "
		"explicit RebuildMesh() call from the tool - the same per-frame notification a drag "
		"relies on"), Actor->RebuildCountForTest(), RebuildsBeforeMove + 1);

	const int32 RebuildsBeforeDelete = Actor->RebuildCountForTest();
	TestTrue(TEXT("the second node deletes"), Actor->DeleteNode(SecondNode));
	TestEqual(TEXT("DeleteNode's OnChanged broadcast rebuilt the mesh exactly once, with no "
		"explicit RebuildMesh() call from the tool"),
		Actor->RebuildCountForTest(), RebuildsBeforeDelete + 1);

	// --- Issue #125: ConnectGuidelines/DisconnectGuideline must commit too --------------
	//
	// Two disjoint roads, far from everything else on this network, so their facing derived
	// ends are guaranteed not to be linked to each other already - the same fixture shape
	// GuidelineDrawToolTest's LinkFixture uses.
	const int32 LinkA0 = Actor->PlaceNode(FVector2D(50000.0, 50000.0));
	const int32 LinkA1 = Actor->PlaceNode(FVector2D(56000.0, 50000.0));
	Actor->ConnectNodes(LinkA0, LinkA1);
	const int32 LinkB0 = Actor->PlaceNode(FVector2D(70000.0, 50000.0));
	const int32 LinkB1 = Actor->PlaceNode(FVector2D(76000.0, 50000.0));
	Actor->ConnectNodes(LinkB0, LinkB1);

	auto NearestGuidelineIndex = [Actor](const FVector2D& Where) -> int32
	{
		const TArray<FGuidelineNode>& Nodes = Actor->Network->GetGuidelineNodes();
		int32 Best = INDEX_NONE;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive) { continue; }
			const double Distance = FVector2D::Distance(Nodes[Index].Position, Where);
			if (Distance < BestDistance) { BestDistance = Distance; Best = Index; }
		}
		return Best;
	};
	const int32 LinkLeft = NearestGuidelineIndex(FVector2D(56000.0, 50000.0));
	const int32 LinkRight = NearestGuidelineIndex(FVector2D(70000.0, 50000.0));
	if (!TestTrue(TEXT("found a guideline node either side of the gap"),
		LinkLeft != INDEX_NONE && LinkRight != INDEX_NONE && LinkLeft != LinkRight))
	{
		return false;
	}

	// CONFIRMED, 2026-09-12 review of PR #122: ConnectGuidelines opened an FRoadEditScope and
	// fell off the end without committing it, so ~FRoadEditScope called AbandonEdit - no undo
	// step pushed for a hand-drawn link, and OnChanged never fired, so nothing here would have
	// caught it: no explicit RebuildMesh() call anywhere in this test, exactly like every
	// other mutator above.
	const int32 RebuildsBeforeLink = Actor->RebuildCountForTest();
	const int32 LinkEdge = Actor->ConnectGuidelines(LinkLeft, LinkRight);
	TestTrue(TEXT("the guideline link is made"), LinkEdge != INDEX_NONE);
	TestEqual(TEXT("ConnectGuidelines's OnChanged broadcast rebuilt the mesh exactly once (#125)"),
		Actor->RebuildCountForTest(), RebuildsBeforeLink + 1);

	TestTrue(TEXT("and the hand-drawn link undoes"), Actor->Undo());
	TestEqual(TEXT("Undo's OnChanged broadcast rebuilt the mesh again - there was an undo step "
		"to take, which is exactly what #125 says was missing"),
		Actor->RebuildCountForTest(), RebuildsBeforeLink + 2);

	// Relinked so DisconnectGuideline has something to remove. RE-RESOLVED, not reused: the
	// undo above rebuilt the mesh, and derived guideline nodes are freed and reallocated on
	// every rebuild (URoadNetwork's own "by position, never by handle" rule for this graph -
	// see UGroundTraffic::OnGraphRebuilt) - the OLD indices are not guaranteed to still name
	// the same two nodes.
	const int32 RelinkLeft = NearestGuidelineIndex(FVector2D(56000.0, 50000.0));
	const int32 RelinkRight = NearestGuidelineIndex(FVector2D(70000.0, 50000.0));
	const int32 RelinkEdge = Actor->ConnectGuidelines(RelinkLeft, RelinkRight);
	if (!TestTrue(TEXT("relinked for the disconnect half"), RelinkEdge != INDEX_NONE))
	{
		return false;
	}

	// THE SAME DEFECT, THE SAME FIX: DisconnectGuideline returned RemoveGuidelineEdge's
	// result directly with the scope still open, so a SUCCESSFUL removal was abandoned too.
	const int32 RebuildsBeforeUnlink = Actor->RebuildCountForTest();
	TestTrue(TEXT("the guideline link is removed"), Actor->DisconnectGuideline(RelinkEdge));
	TestEqual(TEXT("DisconnectGuideline's OnChanged broadcast rebuilt the mesh exactly once (#125)"),
		Actor->RebuildCountForTest(), RebuildsBeforeUnlink + 1);

	TestTrue(TEXT("and the disconnect undoes"), Actor->Undo());
	TestEqual(TEXT("whose Undo rebuilt the mesh once more"),
		Actor->RebuildCountForTest(), RebuildsBeforeUnlink + 2);

	// --- Issue #179: SetIntermediateHoldingPosition must commit through CommitAndNotify too --
	//
	// A fresh, isolated taxiway: the guideline nodes above have all been rebuilt at least
	// once by this point (reallocated by position, not handle - see the reconnect comment
	// above), so a new pair keeps this section from depending on any of that history.
	const int32 HoldA = Actor->PlaceNode(FVector2D(100000.0, 100000.0));
	const int32 HoldB = Actor->PlaceNode(FVector2D(106000.0, 100000.0));
	TestTrue(TEXT("the holding-position taxiway connects"), Actor->ConnectNodes(HoldA, HoldB));
	const int32 HoldNode = NearestGuidelineIndex(FVector2D(106000.0, 100000.0));
	if (!TestTrue(TEXT("found a guideline node to toggle a holding position at"), HoldNode != INDEX_NONE))
	{
		return false;
	}

	// EXACTLY ONE, the same claim as every mutator above: before #179 this committed its
	// scope with no OnChanged.Broadcast() at all, on the reasoning (now wrong - see
	// RoadEditFacade.h) that a holding position changes no mesh.
	const int32 RebuildsBeforeHold = Actor->RebuildCountForTest();
	TestTrue(TEXT("the holding position is set"), Actor->SetIntermediateHoldingPosition(HoldNode, true));
	TestEqual(TEXT("SetIntermediateHoldingPosition's OnChanged broadcast rebuilt the mesh exactly "
		"once (#179) - the HoldingPaint layer derives from this flag and was left stale before"),
		Actor->RebuildCountForTest(), RebuildsBeforeHold + 1);

	const int32 RebuildsBeforeHoldClear = Actor->RebuildCountForTest();
	TestTrue(TEXT("the holding position clears"), Actor->SetIntermediateHoldingPosition(HoldNode, false));
	TestEqual(TEXT("and clearing it notifies too, exactly once"),
		Actor->RebuildCountForTest(), RebuildsBeforeHoldClear + 1);

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #179: MeshRebuildsOnFacadeChange above counts REBUILDS, which proves OnChanged fired
// but not that the HoldingPaint layer itself changed shape - a mutator could notify Topology
// and still leave this one layer untouched if RebuildMarkings' own wiring were wrong.
// HoldingPositionMarkingTest.cpp already measures FHoldingPositionMarkingBuilder::Build
// directly, in isolation from the facade - which is exactly why it could not have caught
// #179: the builder was always correct once asked. This is the composition test the issue
// asked for: drive the toggle the way FHoldingPointTool::OnClick does, through the actor,
// and read the HoldingPaint component the level actually renders.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoldingPositionMeshFollowsToggleTest,
	"Airside.Present.HoldingPositionMeshFollowsToggle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoldingPositionMeshFollowsToggleTest::RunTest(const FString& Parameters)
{
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
	if (!TestTrue(TEXT("the taxiway connects"), Actor->ConnectNodes(A, B)))
	{
		return false;
	}

	// The B end's guideline node, found by POSITION rather than assumed index - the same
	// lookup MeshRebuildsOnFacadeChange uses for its own guideline fixtures, since a
	// rebuild reallocates these nodes.
	const TArray<FGuidelineNode>& GuidelineNodes = Actor->Network->GetGuidelineNodes();
	int32 HoldNode = INDEX_NONE;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 Index = 0; Index < GuidelineNodes.Num(); ++Index)
	{
		if (!GuidelineNodes[Index].bAlive) { continue; }
		const double Distance = FVector2D::Distance(GuidelineNodes[Index].Position, FVector2D(6000.0, 0.0));
		if (Distance < BestDistance) { BestDistance = Distance; HoldNode = Index; }
	}
	if (!TestTrue(TEXT("found a guideline node to toggle"), HoldNode != INDEX_NONE))
	{
		return false;
	}

	const int32 Before = Actor->GetPresenter()->HoldingPaintTriangleCountForTest();

	TestTrue(TEXT("the holding position is set"), Actor->SetIntermediateHoldingPosition(HoldNode, true));
	const int32 AfterSet = Actor->GetPresenter()->HoldingPaintTriangleCountForTest();
	TestTrue(TEXT("setting an intermediate holding position paints the HoldingPaint layer with no "
		"unrelated edit in between - issue #179's whole point"), AfterSet > Before);

	TestTrue(TEXT("the holding position clears"), Actor->SetIntermediateHoldingPosition(HoldNode, false));
	const int32 AfterClear = Actor->GetPresenter()->HoldingPaintTriangleCountForTest();
	TestEqual(TEXT("clearing it repaints the layer back to the prior triangle count"),
		AfterClear, Before);

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #194 (2026-09-21 test-suite review). MeshRebuildsOnFacadeChange above pins "exactly
// one rebuild" for 8 of URoadEditFacade's mutators - PlaceNode, ConnectNodes (success and
// refused), MoveNode, DeleteNode, ConnectGuidelines, DisconnectGuideline and Undo, plus the
// two SetIntermediateHoldingPosition calls #179 added - out of roughly twenty the class
// commits a scope for. A silent regression in any of the REST (SplitSegment, DeleteSegment,
// PlaceRunway, SetRunwayFacts, AddApron, DeleteApron, MoveApronCorner, PlaceEntity,
// PlaceEntityInPlot, DeleteEntity, MergeNodes, ClearNetwork, Redo, and both branches of
// EndInteractiveEdit) would leave the mesh, the guideline overlay or the undo stack stale
// with nothing here to say so.
//
// ONE TABLE, not another dozen IMPLEMENT_SIMPLE_AUTOMATION_TESTs: IMPLEMENT_COMPLEX_
// AUTOMATION_TEST's GetTests below names every row, so a failure reads
// "...MutatorNotifiesExactlyOnce.DeleteApron" rather than a line number in a growing file.
// The leaf names are their own, distinct from every PrettyName already registered above and
// in BuildPurseTest.cpp - the automation tree drops a BARE name once a DOTTED child of it
// exists (project memory), which "Airside.Present.MutatorNotifiesExactlyOnce" cannot trigger
// because it is never registered as a bare test anywhere but here.
//
// EACH CASE OWNS A FRESH FAirsideTestWorld, never a network shared across rows: an earlier
// row's undo stack, or its guideline nodes (reallocated BY POSITION on every rebuild - see
// MeshRebuildsOnFacadeChange's own #125 section above), must not leak into a later row's
// assertion.
//
// EChangeKind (issue #165/#179) DEFAULTS TO Topology for both NotifyChanged and
// CommitAndNotify (RoadEditFacade.h), so TopologyRebuildCountForTest moving in step with
// RebuildCountForTest already IS the assertion for every row below except the three
// RoadEditTarget.h's own EChangeKind comment names as exceptions: MoveNode and
// MoveApronCorner mid-drag (Geometry), and SetIntermediateHoldingPosition (Markings) - those
// three check that Topology does NOT move.
//
// REFUSALS MOSTLY REUSE AN INDEX NOTHING HAS EVER PLACED, on a network that may not even
// exist yet: MakeLiveNodeId/MakeLiveSegmentId/GuidelineNodeIdAt/ApronIdAt/EntityIdAt all
// answer "not live" the same way for a null network as for a dead slot (RoadEditFacade.cpp),
// so RefusesWithNoRebuild below needs no fixture at all for most rows. The rows whose
// interesting guard fires BEFORE or INSTEAD OF that liveness check (ConnectNodes joining
// itself, PlaceRunway's null-profile guard, PlaceEntity's missing-definition guard,
// PlaceEntityInPlot's empty reservation, DisconnectGuideline's derived-edge guard,
// SetRunwayFacts on a non-runway segment) build just enough graph to reach the guard the
// source actually names, rather than settling for the same liveness check every other row
// already covers.
namespace
{
	/** A refusal that must not touch RebuildCountForTest at all. */
	bool RefusesWithNoRebuild(FAutomationTestBase& T, const TCHAR* What,
		TFunctionRef<bool(ARoadNetworkActor&)> Call)
	{
		FAirsideTestWorld World;
		if (!T.TestNotNull(TEXT("actor constructed"), World.Actor)) { return false; }
		const int32 Before = World.Actor->RebuildCountForTest();
		T.TestFalse(What, Call(*World.Actor));
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), World.Actor->RebuildCountForTest(), Before);
		return true;
	}

	/** As above, for a mutator that answers INDEX_NONE rather than false. */
	bool RefusesIndexWithNoRebuild(FAutomationTestBase& T, const TCHAR* What,
		TFunctionRef<int32(ARoadNetworkActor&)> Call)
	{
		FAirsideTestWorld World;
		if (!T.TestNotNull(TEXT("actor constructed"), World.Actor)) { return false; }
		const int32 Before = World.Actor->RebuildCountForTest();
		T.TestEqual(What, Call(*World.Actor), static_cast<int32>(INDEX_NONE));
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), World.Actor->RebuildCountForTest(), Before);
		return true;
	}

	/** A purse that affords nothing, for EndInteractiveEdit's revert branch - the ONLY row
	 *  here that needs one. Not FRecordingPurse (BuildPurseTest.cpp, a different
	 *  translation unit's own anonymous namespace - and this one is a UNITY build, where two
	 *  same-named anonymous-namespace symbols in different files collide once concatenated,
	 *  the whole reason AirsideTestFixtures.h exists): nothing here reads what was charged,
	 *  only that a drag that added pavement it cannot pay for is reverted rather than left
	 *  standing unpaid for. */
	class FAlwaysBrokePurse : public IBuildPurse
	{
	public:
		virtual bool CanAfford(const FBuildQuote&) const override { return false; }
		virtual int32 Charge(const FBuildQuote&) override { return INDEX_NONE; }
		virtual void Reverse(int32) override {}
		virtual void Credit(const FBuildQuote&) override {}
		virtual FText Describe(const FBuildQuote&) const override { return FText(); }
	};

	/** The alive guideline node nearest Where, by POSITION - a road rebuild reallocates
	 *  these, so nothing here may assume an index. NOT the identically-shaped lambda already
	 *  local to MeshRebuildsOnFacadeChangeTest above: that one is block-scoped to its own
	 *  function and this is a free function in the file's anonymous namespace, so the two
	 *  cannot collide, but a SECOND local of this same name inside a function below would
	 *  shadow this one - every case here calls this instead of redeclaring its own. */
	int32 NearestGuidelineNodeIndex(const URoadNetwork& Network, const FVector2D& Where)
	{
		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		int32 Best = INDEX_NONE;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive) { continue; }
			const double Distance = FVector2D::Distance(Nodes[Index].Position, Where);
			if (Distance < BestDistance) { BestDistance = Distance; Best = Index; }
		}
		return Best;
	}

	bool Case_PlaceNode(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a node is placed"), Actor->PlaceNode(FVector2D(1000.0, 1000.0)) != INDEX_NONE);
		T.TestEqual(TEXT("PlaceNode notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology, like every plain CommitAndNotify"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_ConnectNodesSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the pair connects"), Actor->ConnectNodes(A, B));
		T.TestEqual(TEXT("ConnectNodes notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_ConnectNodesRefused(FAutomationTestBase& T)
	{
		// FromIndex == ToIndex short-circuits before MakeLiveNodeId ever runs, so this needs
		// no placed node at all - MeshRebuildsOnFacadeChange above already proves the same
		// guard refuses a REAL node joining itself; this row only measures the notify count.
		return RefusesWithNoRebuild(T, TEXT("a node cannot join itself"),
			[](ARoadNetworkActor& Actor) { return Actor.ConnectNodes(0, 0); });
	}

	bool Case_MoveNodeBare(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a bare move (no Begin/EndInteractiveEdit wrapping it) succeeds"),
			Actor->MoveNode(B, FVector2D(6500.0, 300.0)));
		T.TestEqual(TEXT("and notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);

		// PR #195's rule: a move with no surrounding drag has no EndInteractiveEdit coming to
		// catch the derived graph up later, so it has to do the whole job itself - Topology,
		// not Geometry (RoadEditFacade.cpp's own "BARE-CALL TRAP" comment on MoveNode).
		T.TestEqual(TEXT("as Topology, because nothing will ever catch this frame up otherwise"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_MoveNodeMidDrag(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		Actor->BeginInteractiveEdit(TEXT("drag node"));
		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a move while a drag is open succeeds"),
			Actor->MoveNode(B, FVector2D(6500.0, 300.0)));
		T.TestEqual(TEXT("and notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);

		// THE OTHER HALF OF THE SAME RULE: EndInteractiveEdit has not closed yet, so its one
		// Topology notify is still coming - this frame only has to rebuild the surface.
		T.TestEqual(TEXT("as Geometry, not Topology - EndInteractiveEdit still owes the catch-up"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore);

		Actor->EndInteractiveEdit(true);
		return true;
	}

	bool Case_MoveNodeRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses MoveNode"),
			[](ARoadNetworkActor& Actor) { return Actor.MoveNode(999, FVector2D::ZeroVector); });
	}

	bool Case_SplitSegmentSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the segment splits"), Actor->SplitSegment(0, FVector2D(3000.0, 0.0)) != INDEX_NONE);
		T.TestEqual(TEXT("SplitSegment notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_SplitSegmentRefused(FAutomationTestBase& T)
	{
		return RefusesIndexWithNoRebuild(T, TEXT("an index nothing placed refuses SplitSegment"),
			[](ARoadNetworkActor& Actor) { return Actor.SplitSegment(999, FVector2D::ZeroVector); });
	}

	bool Case_DeleteNodeSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a leaf node deletes"), Actor->DeleteNode(B));
		T.TestEqual(TEXT("DeleteNode notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_DeleteNodeRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses DeleteNode"),
			[](ARoadNetworkActor& Actor) { return Actor.DeleteNode(999); });
	}

	bool Case_DeleteSegmentSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the segment deletes"), Actor->DeleteSegment(0));
		T.TestEqual(TEXT("DeleteSegment notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_DeleteSegmentRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses DeleteSegment"),
			[](ARoadNetworkActor& Actor) { return Actor.DeleteSegment(999); });
	}

	bool Case_MergeNodesSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		// TWO BARE NODES, NEITHER CONNECTED: no incident arms on either side, so
		// RoadPlacement::NodeCornersFit (judged AFTER the merge - see MergeNodes' own
		// comment on why it cannot be judged before) has no corner to fail. This measures
		// the notify, not the corner-fit machinery MergeNodes already has its own coverage
		// for elsewhere.
		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(500.0, 500.0));

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("two bare nodes merge"), Actor->MergeNodes(A, B));
		T.TestEqual(TEXT("MergeNodes notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology - a node disappeared, the graph's shape changed"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_MergeNodesRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses MergeNodes"),
			[](ARoadNetworkActor& Actor) { return Actor.MergeNodes(0, 1); });
	}

	bool Case_PlaceRunwaySuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		// PlaceRunway's FIRST guard is `Owner.Network == nullptr` - silent, and reached on
		// a fresh actor before EVER creating one (unlike PlaceNode/ConnectNodes, it has no
		// EnsureNetwork() of its own). One isolated node brings the network into being, the
		// same reason MeshRebuildsOnFacadeChange above places one before its own PlaceRunway.
		Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		// Below the default 50000 uu minimum - MeshRebuildsOnFacadeChange's own comment on
		// PlaceRunway above explains why a short test strip needs this rather than a
		// half-kilometre one.
		Actor->MinimumRunwayLength = 100.0;

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a runway is placed"),
			Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), Runway));
		T.TestEqual(TEXT("PlaceRunway notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_PlaceRunwayRefused(FAutomationTestBase& T)
	{
		// A null profile refuses before anything else IT LOGS - the guard exists
		// specifically so a taxiway is never laid at runway length and called one
		// (PlaceRunway's own comment) - but a network still has to exist first for that
		// guard to be the one this row actually reaches (see Case_PlaceRunwaySuccess).
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		T.TestFalse(TEXT("no runway profile refuses PlaceRunway"),
			Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(60000.0, 0.0), nullptr));
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), Actor->RebuildCountForTest(), RebuildsBefore);
		return true;
	}

	bool Case_SetRunwayFactsSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(-100000.0, -100000.0));

		URoadProfile* Runway = URoadProfile::MakeTransient(4500.0, 1500.0, 450.0);
		Runway->bContinuousThroughJunctions = true;
		Actor->MinimumRunwayLength = 100.0;
		if (!T.TestTrue(TEXT("a runway is placed"),
			Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), Runway))) { return false; }

		// A GENUINE CHANGE, not PlaceRunway's own default facts (Tarmac/Visual) - SetRunwayFacts
		// silently skips its own notify when the facts already match (see its "already so, but
		// no edit" comment), which would wrongly pass this row for the wrong reason.
		FRunwayFacts Facts;
		Facts.Surface = ERunwaySurface::Concrete;
		Facts.Approach = ERunwayApproach::Precision;

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the runway is reclassified"), Actor->SetRunwayFacts(0, Facts));
		T.TestEqual(TEXT("SetRunwayFacts notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_SetRunwayFactsRefused(FAutomationTestBase& T)
	{
		// AN ORDINARY TAXIWAY, not an invalid index: SetRunwayFacts' own guard is
		// !Network->IsRunwaySegment(Segment), which a live but non-runway segment reaches
		// where an out-of-range index never would.
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		T.TestFalse(TEXT("a taxiway is not a runway"), Actor->SetRunwayFacts(0, FRunwayFacts()));
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), Actor->RebuildCountForTest(), RebuildsBefore);
		return true;
	}

	bool Case_AddApronSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0),
			FVector2D(1000.0, 1000.0), FVector2D(0.0, 1000.0) };

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the apron is added"), Actor->AddApron(Outline) != INDEX_NONE);
		T.TestEqual(TEXT("AddApron notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_AddApronRefused(FAutomationTestBase& T)
	{
		// Fewer than three corners refuses before EnsureNetwork even runs.
		return RefusesIndexWithNoRebuild(T, TEXT("two corners refuse AddApron"),
			[](ARoadNetworkActor& Actor)
			{
				return Actor.AddApron({ FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0) });
			});
	}

	bool Case_DeleteApronSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0),
			FVector2D(1000.0, 1000.0), FVector2D(0.0, 1000.0) };
		Actor->AddApron(Outline);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the apron deletes"), Actor->DeleteApron(0));
		T.TestEqual(TEXT("DeleteApron notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_DeleteApronRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses DeleteApron"),
			[](ARoadNetworkActor& Actor) { return Actor.DeleteApron(999); });
	}

	bool Case_MoveApronCornerSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0),
			FVector2D(1000.0, 1000.0), FVector2D(0.0, 1000.0) };
		Actor->AddApron(Outline);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a corner moves"), Actor->MoveApronCorner(0, 0, FVector2D(-100.0, -100.0)));
		T.TestEqual(TEXT("MoveApronCorner notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);

		// A BARE call (no BeginInteractiveEdit wrapping it) opens and closes its own tiny
		// edit, so - the same BARE-CALL TRAP as MoveNode above - this is the only notify the
		// drag will ever get and has to be Topology, even though an apron corner is not in
		// the road graph at all (MoveApronCorner's own comment).
		T.TestEqual(TEXT("as Topology, the bare-call case"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_MoveApronCornerRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses MoveApronCorner"),
			[](ARoadNetworkActor& Actor) { return Actor.MoveApronCorner(999, 0, FVector2D::ZeroVector); });
	}

	bool Case_PlaceEntitySuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a stand is placed"),
			Actor->PlaceEntity(FVector2D::ZeroVector, 0.0, EPlaceableEntity::Stand) != INDEX_NONE);
		T.TestEqual(TEXT("PlaceEntity notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_PlaceEntityRefused(FAutomationTestBase& T)
	{
		// NEITHER the actor's own override NOR the content set's default: with either alone
		// present the resolver still finds a stand and this would wrongly place, not refuse -
		// FuelDepotPlaceToolTest's own "no depot definition anywhere" block clears the same
		// setting for the same reason and restores it straight after.
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		UAirsideSettings* Settings = GetMutableDefault<UAirsideSettings>();
		const TSoftObjectPtr<UAirsideContent> Configured = Settings->Content;
		Settings->Content.Reset();
		ON_SCOPE_EXIT { Settings->Content = Configured; };

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		T.TestTrue(TEXT("no definition anywhere refuses PlaceEntity"),
			Actor->PlaceEntity(FVector2D::ZeroVector, 0.0, EPlaceableEntity::Stand) == INDEX_NONE);
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), Actor->RebuildCountForTest(), RebuildsBefore);
		return true;
	}

	bool Case_PlaceEntityInPlotSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

		const TArray<FVector2D> Outline = {
			FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0),
			FVector2D(1200.0, 1200.0), FVector2D(0.0, 1200.0) };

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("a depot plot is placed"),
			Actor->PlaceEntityInPlot(Outline, FVector2D(0.0, 0.0), FVector2D(1200.0, 0.0),
				{ EDepotModule::Shed }, EPlaceableEntity::FuelDepot) != INDEX_NONE);
		T.TestEqual(TEXT("PlaceEntityInPlot notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_PlaceEntityInPlotRefused(FAutomationTestBase& T)
	{
		// A SHALLOW PLOT, not a missing definition: 100 uu deep is well under a bay's depth,
		// so ReserveForPlot's solve reserves nothing at all - the ONE refusal #182 left
		// PlaceEntityInPlot with, the same shape FPlotEmptyReservationIsRefusedByOneEvaluator
		// Test (PlotPlaceToolTest.cpp) drives through the tool instead. A resolvable
		// definition is set so this row measures THIS guard, not the definition lookup
		// Case_PlaceEntityRefused already covers.
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->FuelDepotDefinition = UEntityDefinition::MakeFuelDepotTransient();

		const TArray<FVector2D> Shallow = {
			FVector2D(0.0, 0.0), FVector2D(1500.0, 0.0),
			FVector2D(1500.0, 100.0), FVector2D(0.0, 100.0) };

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		T.TestTrue(TEXT("a plot too shallow to hold anything reserves nothing"),
			Actor->PlaceEntityInPlot(Shallow, FVector2D(0.0, 0.0), FVector2D(1500.0, 0.0),
				{ EDepotModule::Shed }, EPlaceableEntity::FuelDepot) == INDEX_NONE);
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), Actor->RebuildCountForTest(), RebuildsBefore);
		return true;
	}

	bool Case_DeleteEntitySuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->StandDefinition = UEntityDefinition::MakeStandTransient();
		Actor->PlaceEntity(FVector2D::ZeroVector, 0.0, EPlaceableEntity::Stand);

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the stand deletes"), Actor->DeleteEntity(0));
		T.TestEqual(TEXT("DeleteEntity notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_DeleteEntityRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses DeleteEntity"),
			[](ARoadNetworkActor& Actor) { return Actor.DeleteEntity(999); });
	}

	bool Case_ConnectGuidelinesSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		// Two disjoint roads - the same shape MeshRebuildsOnFacadeChange's #125 section
		// above uses - so their facing derived guideline nodes are guaranteed not linked
		// already.
		const int32 LinkA0 = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 LinkA1 = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(LinkA0, LinkA1);
		const int32 LinkB0 = Actor->PlaceNode(FVector2D(20000.0, 0.0));
		const int32 LinkB1 = Actor->PlaceNode(FVector2D(26000.0, 0.0));
		Actor->ConnectNodes(LinkB0, LinkB1);

		const int32 LinkLeft = NearestGuidelineNodeIndex(*Actor->GetNetwork(), FVector2D(6000.0, 0.0));
		const int32 LinkRight = NearestGuidelineNodeIndex(*Actor->GetNetwork(), FVector2D(20000.0, 0.0));
		if (!T.TestTrue(TEXT("found a guideline node either side of the gap"),
			LinkLeft != INDEX_NONE && LinkRight != INDEX_NONE && LinkLeft != LinkRight)) { return false; }

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the guideline link is made"), Actor->ConnectGuidelines(LinkLeft, LinkRight) != INDEX_NONE);
		T.TestEqual(TEXT("ConnectGuidelines notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_ConnectGuidelinesRefused(FAutomationTestBase& T)
	{
		return RefusesIndexWithNoRebuild(T, TEXT("an index nothing placed refuses ConnectGuidelines"),
			[](ARoadNetworkActor& Actor) { return Actor.ConnectGuidelines(0, 0); });
	}

	bool Case_DisconnectGuidelineSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 LinkA0 = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 LinkA1 = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(LinkA0, LinkA1);
		const int32 LinkB0 = Actor->PlaceNode(FVector2D(20000.0, 0.0));
		const int32 LinkB1 = Actor->PlaceNode(FVector2D(26000.0, 0.0));
		Actor->ConnectNodes(LinkB0, LinkB1);

		const int32 LinkLeft = NearestGuidelineNodeIndex(*Actor->GetNetwork(), FVector2D(6000.0, 0.0));
		const int32 LinkRight = NearestGuidelineNodeIndex(*Actor->GetNetwork(), FVector2D(20000.0, 0.0));
		const int32 Edge = Actor->ConnectGuidelines(LinkLeft, LinkRight);
		if (!T.TestTrue(TEXT("the hand-drawn link is made"), Edge != INDEX_NONE)) { return false; }

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the link removes"), Actor->DisconnectGuideline(Edge));
		T.TestEqual(TEXT("DisconnectGuideline notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_DisconnectGuidelineRefused(FAutomationTestBase& T)
	{
		// A DERIVED edge, not an invalid index: ConnectNodes derives one along the road it
		// just laid, and DisconnectGuideline's own guard refuses it because the next rebuild
		// would put it straight back - obeying would be indistinguishable from ignoring the
		// click (DisconnectGuideline's own comment).
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		if (!T.TestTrue(TEXT("the pair connects, deriving a guideline edge along it"),
			Actor->ConnectNodes(A, B))) { return false; }

		const TArray<FGuidelineEdge>& Edges = Actor->GetNetwork()->GetGuidelineEdges();
		if (!T.TestTrue(TEXT("connecting a road derives at least one guideline edge"), Edges.Num() > 0))
		{
			return false;
		}
		if (!T.TestTrue(TEXT("and it is derived - the guard this row measures"), Edges[0].bDerived))
		{
			return false;
		}

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		T.TestFalse(TEXT("a derived guideline cannot be disconnected"), Actor->DisconnectGuideline(0));
		T.TestEqual(TEXT("and nothing rebuilt for the refusal"), Actor->RebuildCountForTest(), RebuildsBefore);
		return true;
	}

	bool Case_SetIntermediateHoldingPositionSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);
		const int32 HoldNode = NearestGuidelineNodeIndex(*Actor->GetNetwork(), FVector2D(6000.0, 0.0));
		if (!T.TestTrue(TEXT("found a guideline node to toggle"), HoldNode != INDEX_NONE)) { return false; }

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the holding position is set"), Actor->SetIntermediateHoldingPosition(HoldNode, true));
		T.TestEqual(TEXT("SetIntermediateHoldingPosition notifies exactly once"),
			Actor->RebuildCountForTest(), RebuildsBefore + 1);

		// ISSUE #179's OWN POINT: Markings, not Topology - the flag changes neither the
		// pavement nor the graph's shape, and routing it through Topology reallocates the
		// very node whose flag was just set (RoadEditTarget.h's own EChangeKind comment).
		T.TestEqual(TEXT("as Markings, not Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore);
		return true;
	}

	bool Case_SetIntermediateHoldingPositionRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("an index nothing placed refuses SetIntermediateHoldingPosition"),
			[](ARoadNetworkActor& Actor) { return Actor.SetIntermediateHoldingPosition(999, true); });
	}

	bool Case_ClearNetwork(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(0.0, 0.0));

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		Actor->ClearNetwork();
		T.TestEqual(TEXT("ClearNetwork notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_UndoSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(0.0, 0.0));

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the placement undoes"), Actor->Undo());
		T.TestEqual(TEXT("Undo notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_UndoRefused(FAutomationTestBase& T)
	{
		// Nothing has ever been placed, so Travel's own null-network guard refuses before it
		// even asks the (also empty) undo stack.
		return RefusesWithNoRebuild(T, TEXT("a fresh actor has nothing to undo"),
			[](ARoadNetworkActor& Actor) { return Actor.Undo(); });
	}

	bool Case_RedoSuccess(FAutomationTestBase& T)
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		Actor->PlaceNode(FVector2D(0.0, 0.0));
		Actor->Undo();

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		T.TestTrue(TEXT("the placement redoes"), Actor->Redo());
		T.TestEqual(TEXT("Redo notifies exactly once"), Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology"), Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	bool Case_RedoRefused(FAutomationTestBase& T)
	{
		return RefusesWithNoRebuild(T, TEXT("a fresh actor has nothing to redo"),
			[](ARoadNetworkActor& Actor) { return Actor.Redo(); });
	}

	bool Case_EndInteractiveEditRevert(FAutomationTestBase& T)
	{
		// THE CANNOT-AFFORD BRANCH (RoadEditFacade.cpp's EndInteractiveEdit, "REVERTED, NOT
		// ABANDONED"). The drag has to add pavement it cannot pay for, which needs BOTH a
		// nonzero price (PriceTheTaxiway's own trick, BuildPurseTest.cpp) and a purse that
		// actually says no - a null purse builds free and never reaches this branch at all.
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }
		if (URoadProfile* Profile = Actor->ResolveProfile()) { Profile->CostPerMetre = 300.0; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(1000.0, 0.0));
		if (!T.TestTrue(TEXT("the taxiway connects"), Actor->ConnectNodes(A, B))) { return false; }

		FAlwaysBrokePurse Purse;
		Actor->GetEditFacade()->SetPurse(&Purse);

		Actor->BeginInteractiveEdit(TEXT("drag node"));
		if (!T.TestTrue(TEXT("the drag itself moves the node"),
			Actor->MoveNode(B, FVector2D(20000.0, 0.0)))) { return false; }

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		Actor->EndInteractiveEdit(true);
		T.TestEqual(TEXT("the revert's own NotifyChanged() fires exactly once"),
			Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology, NotifyChanged's default"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);

		// AND THE REASON THE BRANCH EXISTS: the node is back where the drag started, not left
		// out at 200 m unpaid for (RevertEdit, not merely refused - BuildPurseDragRevertsWhenBroke
		// Test in BuildPurseTest.cpp measures the same thing from the purse's side).
		T.TestEqual(TEXT("the node reverted to where the drag began"),
			Actor->GetNetwork()->GetNodes()[B].Position.X, 1000.0, 1e-6);
		return true;
	}

	bool Case_EndInteractiveEditAbandon(FAutomationTestBase& T)
	{
		// THE bKeep=false BRANCH AFTER A REAL MOVE ("LATENT STALENESS ON ABANDON",
		// RoadEditFacade.cpp's EndInteractiveEdit). AbandonEdit drops only the undo snapshot -
		// it does NOT put the node back - so the derived graph is left pointed at the
		// PRE-drag state unless this fires; the guard is bGeometryChangedDuringEdit, set by
		// MoveNode's own Geometry notify just above.
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = World.Actor;
		if (!T.TestNotNull(TEXT("actor constructed"), Actor)) { return false; }

		const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
		const int32 B = Actor->PlaceNode(FVector2D(6000.0, 0.0));
		Actor->ConnectNodes(A, B);

		Actor->BeginInteractiveEdit(TEXT("drag node"));
		if (!T.TestTrue(TEXT("the drag moves the node"),
			Actor->MoveNode(B, FVector2D(6500.0, 300.0)))) { return false; }

		const int32 RebuildsBefore = Actor->RebuildCountForTest();
		const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
		Actor->EndInteractiveEdit(false);
		T.TestEqual(TEXT("abandoning after a real move fires exactly one catch-up notify"),
			Actor->RebuildCountForTest(), RebuildsBefore + 1);
		T.TestEqual(TEXT("as Topology - the one notify a drag cancelled this way will ever get"),
			Actor->TopologyRebuildCountForTest(), TopologyBefore + 1);
		return true;
	}

	struct FMutatorCase
	{
		const TCHAR* Name;
		bool (*Run)(FAutomationTestBase&);
	};

	// ORDER MATCHES THE ISSUE'S OWN LIST, success then its refusal where this table drives
	// one - so a diff against #194 reads top to bottom instead of hunting for a name.
	//
	// EVERY ROW WITH A SIBLING GETS A SUFFIX OF ITS OWN, success included - "ConnectNodes"
	// alongside "ConnectNodes.Refused" is the SAME bare-parent-vs-dotted-child collision
	// project memory warns about, one level down: the first run of this exact table (2026-09-21)
	// silently dropped 18 success rows this way and still reported green, which is what sent
	// this comment back to be written. PlaceNode and ClearNetwork have no sibling and keep
	// their bare names.
	const TArray<FMutatorCase>& GetMutatorCases()
	{
		static const TArray<FMutatorCase> Cases = {
			{ TEXT("PlaceNode"), &Case_PlaceNode },
			{ TEXT("ConnectNodes.Success"), &Case_ConnectNodesSuccess },
			{ TEXT("ConnectNodes.Refused"), &Case_ConnectNodesRefused },
			{ TEXT("MoveNode.Bare"), &Case_MoveNodeBare },
			{ TEXT("MoveNode.MidDrag"), &Case_MoveNodeMidDrag },
			{ TEXT("MoveNode.Refused"), &Case_MoveNodeRefused },
			{ TEXT("SplitSegment.Success"), &Case_SplitSegmentSuccess },
			{ TEXT("SplitSegment.Refused"), &Case_SplitSegmentRefused },
			{ TEXT("DeleteNode.Success"), &Case_DeleteNodeSuccess },
			{ TEXT("DeleteNode.Refused"), &Case_DeleteNodeRefused },
			{ TEXT("DeleteSegment.Success"), &Case_DeleteSegmentSuccess },
			{ TEXT("DeleteSegment.Refused"), &Case_DeleteSegmentRefused },
			{ TEXT("MergeNodes.Success"), &Case_MergeNodesSuccess },
			{ TEXT("MergeNodes.Refused"), &Case_MergeNodesRefused },
			{ TEXT("PlaceRunway.Success"), &Case_PlaceRunwaySuccess },
			{ TEXT("PlaceRunway.Refused"), &Case_PlaceRunwayRefused },
			{ TEXT("SetRunwayFacts.Success"), &Case_SetRunwayFactsSuccess },
			{ TEXT("SetRunwayFacts.Refused"), &Case_SetRunwayFactsRefused },
			{ TEXT("AddApron.Success"), &Case_AddApronSuccess },
			{ TEXT("AddApron.Refused"), &Case_AddApronRefused },
			{ TEXT("DeleteApron.Success"), &Case_DeleteApronSuccess },
			{ TEXT("DeleteApron.Refused"), &Case_DeleteApronRefused },
			{ TEXT("MoveApronCorner.Success"), &Case_MoveApronCornerSuccess },
			{ TEXT("MoveApronCorner.Refused"), &Case_MoveApronCornerRefused },
			{ TEXT("PlaceEntity.Success"), &Case_PlaceEntitySuccess },
			{ TEXT("PlaceEntity.Refused"), &Case_PlaceEntityRefused },
			{ TEXT("PlaceEntityInPlot.Success"), &Case_PlaceEntityInPlotSuccess },
			{ TEXT("PlaceEntityInPlot.Refused"), &Case_PlaceEntityInPlotRefused },
			{ TEXT("DeleteEntity.Success"), &Case_DeleteEntitySuccess },
			{ TEXT("DeleteEntity.Refused"), &Case_DeleteEntityRefused },
			{ TEXT("ConnectGuidelines.Success"), &Case_ConnectGuidelinesSuccess },
			{ TEXT("ConnectGuidelines.Refused"), &Case_ConnectGuidelinesRefused },
			{ TEXT("DisconnectGuideline.Success"), &Case_DisconnectGuidelineSuccess },
			{ TEXT("DisconnectGuideline.Refused"), &Case_DisconnectGuidelineRefused },
			{ TEXT("SetIntermediateHoldingPosition.Success"), &Case_SetIntermediateHoldingPositionSuccess },
			{ TEXT("SetIntermediateHoldingPosition.Refused"), &Case_SetIntermediateHoldingPositionRefused },
			{ TEXT("ClearNetwork"), &Case_ClearNetwork },
			{ TEXT("Undo.Success"), &Case_UndoSuccess },
			{ TEXT("Undo.Refused"), &Case_UndoRefused },
			{ TEXT("Redo.Success"), &Case_RedoSuccess },
			{ TEXT("Redo.Refused"), &Case_RedoRefused },
			{ TEXT("EndInteractiveEdit.Revert"), &Case_EndInteractiveEditRevert },
			{ TEXT("EndInteractiveEdit.Abandon"), &Case_EndInteractiveEditAbandon },
		};
		return Cases;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FMutatorNotifiesExactlyOnceTest,
	"Airside.Present.MutatorNotifiesExactlyOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

void FMutatorNotifiesExactlyOnceTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const FMutatorCase& Case : GetMutatorCases())
	{
		OutBeautifiedNames.Add(Case.Name);
		OutTestCommands.Add(Case.Name);
	}
}

bool FMutatorNotifiesExactlyOnceTest::RunTest(const FString& Parameters)
{
	for (const FMutatorCase& Case : GetMutatorCases())
	{
		if (Parameters == Case.Name)
		{
			return Case.Run(*this);
		}
	}

	// A MUTATOR NOT DRIVEN, NAMED RATHER THAN SILENT (this file's own convention, see the
	// PlanNodeDeletion note below): reaching here means GetTests and this dispatch have
	// drifted apart, which is exactly the "lists that must agree" failure CLAUDE.md warns
	// about, so it fails loudly instead of quietly reporting a pass for nothing run.
	AddError(FString::Printf(TEXT("Airside.Present.MutatorNotifiesExactlyOnce: no case named '%s'"), *Parameters));
	return false;
}

// NOT DRIVEN HERE, AND SAID SO RATHER THAN LEFT SILENT (this section's own rule): PlanNodeDeletion
// is a query, not a mutator - it commits no scope and calls CommitAndNotify never, so it has
// no notify for this table to measure at all (its own cache-hit contract is pinned instead by
// #166, where it lives). ResolveProfileFor, ResolveDepotKits, GetEntityDefinition and the rest
// of IRoadEditTarget's read-only surface are the same story. ClearHistory (issue #191, added
// to the header after this table was first drafted) is the same shape again: it deliberately
// skips EnsureHistory and calls neither Edit.Commit() nor NotifyChanged - see its own comment.

#endif
