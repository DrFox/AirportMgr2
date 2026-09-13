#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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
// URoadEditFacade::OnChanged -> ARoadNetworkActor::RebuildMesh, which originally replaced
// four direct RebuildMesh() calls the facade's mutators used to make (PlaceRunway,
// ClearNetwork, Undo, Redo). Issue #77 (2026-09-13) made CommitAndNotify/NotifyChanged the
// single broadcast point for every committed edit - PlaceNode, ConnectNodes, SplitSegment,
// DeleteNode, DeleteSegment, MoveNode, AddApron, DeleteApron, PlaceEntity and DeleteEntity
// now notify too, and the 12 RebuildMesh() calls the TOOLS used to make after them are gone
// (one of them, RoadDrawTool's chaining state, used to rebuild even on a REFUSED connect -
// see RoadEditFacade.h's own comment on OnChanged). Nothing asserted that the wiring still
// fires: every other test that wants a rebuilt mesh calls Actor->RebuildMesh() itself, which
// would still pass even if the constructor's
// Facade->OnChanged.AddUObject(this, &ARoadNetworkActor::RebuildMesh) were deleted outright.
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

	return true;
}

#endif
