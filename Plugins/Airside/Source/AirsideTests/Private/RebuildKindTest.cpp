#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadEditFacade.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------------------
// Issue #165: every committed edit AND every drag frame used to re-solve and re-derive the
// WHOLE airport - FRoadGuidelineBuilder::Build, FAnchorLink::Build, Plots->RebuildFrom,
// Traffic->OnGraphRebuilt, all of it, every single MoveNode call a drag makes. This measures
// the fix at the level of the composition (the actor and the facade it owns), not the model
// struct alone - a delegate rewired to the wrong overload, or a Kind that defaulted the
// wrong way, would show here and nowhere lower.
//
// TopologyRebuildCountForTest stands in for "the guideline builder / anchor links / plots /
// traffic pass ran": ARoadNetworkActor::RebuildMeshForChange gates all four behind the same
// `if (Kind == EChangeKind::Topology)`, so one counter answers for all of them without this
// test reaching into UGroundTraffic or UPlotPresenter to ask each separately.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDragNotifiesGeometryOnlyTest,
	"Airside.Present.DragNotifiesGeometryOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDragNotifiesGeometryOnlyTest::RunTest(const FString& Parameters)
{
	// NewObject, no world: matching Airside.Present.MeshRebuildsOnFacadeChange rather than a
	// spawned FAirsideTestWorld actor - what is under test is the delegate's Kind and the
	// counters it drives, neither of which needs PostRegisterAllComponents.
	ARoadNetworkActor* Actor = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Actor))
	{
		return false;
	}

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("the two nodes connect"), Actor->ConnectNodes(A, B)))
	{
		return false;
	}

	// THE CONTROL: PlaceNode and ConnectNodes are ordinary topology-changing mutators, and
	// each already ran the derived-graph pass once - without this, "exactly once" below
	// could pass on a pass that had stopped running at all.
	TestTrue(TEXT("placing and connecting nodes each ran the derived-graph pass"),
		Actor->TopologyRebuildCountForTest() >= 2);

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade))
	{
		return false;
	}

	const int32 RebuildsBeforeDrag = Actor->RebuildCountForTest();
	const int32 TopologyBeforeDrag = Actor->TopologyRebuildCountForTest();

	// THREE FRAMES OF ONE DRAG. The count is arbitrary and deliberately more than one: issue
	// #165 is specifically that every frame re-ran the whole pipeline, not merely that the
	// first one did.
	Facade->BeginInteractiveEdit(TEXT("drag node"));
	TestTrue(TEXT("drag frame 1 moves"), Actor->MoveNode(B, FVector2D(10500.0, 500.0)));
	TestTrue(TEXT("drag frame 2 moves"), Actor->MoveNode(B, FVector2D(11000.0, 900.0)));
	TestTrue(TEXT("drag frame 3 moves"), Actor->MoveNode(B, FVector2D(11500.0, 1200.0)));
	Facade->EndInteractiveEdit(/*bKeep*/ true);

	// THE SURFACE STILL TRACKS EVERY FRAME - three moves plus the commit notify
	// EndInteractiveEdit fires at the end, four rebuilds in total. A drag that stopped
	// repainting the mesh per frame would be a different, worse bug (a cursor the pavement
	// does not follow).
	TestEqual(TEXT("every drag frame still rebuilt the surface, plus one for the commit"),
		Actor->RebuildCountForTest(), RebuildsBeforeDrag + 4);

	// THE MEASUREMENT: the derived-graph pass ran exactly once for the whole drag, at
	// EndInteractiveEdit, not once per MoveNode call. This is the line that fails on
	// unpatched main, where every MoveNode notify ran the full pipeline.
	TestEqual(TEXT("but the guideline/anchor/plots/traffic pass ran exactly once, at the "
		"drag's end - not once per frame"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeDrag + 1);

	// AND A PLAIN MUTATOR STILL RUNS IT, afterwards - the control that closes the loop: the
	// pass is skipped for a drag frame specifically, not broken outright.
	const int32 TopologyBeforePlace = Actor->TopologyRebuildCountForTest();
	Actor->PlaceNode(FVector2D(90000.0, 90000.0));
	TestEqual(TEXT("and PlaceNode still runs the derived-graph pass"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforePlace + 1);

	// --- An ABANDONED drag (Escape) must still catch the derived graph up - NOT HERE ------
	//
	// A drag that moved something and then ended with EndInteractiveEdit(false) owes the derived graph
	// exactly one Topology notify (AbandonEdit only drops the undo snapshot; it does not put the node
	// back), or cancelling would leave the guideline graph, anchor links, plots and traffic pointed at
	// pre-drag positions forever. That section was this file's, and is now the table row
	// MutatorNotifiesExactlyOnce.EndInteractiveEdit.Abandon (MeshFreshnessTest.cpp), which measures the
	// same counts on a fresh world; it is not repeated here.

	// --- Review follow-up: a MOTIONLESS drag must cost NOTHING -----------------------------
	//
	// A click-release that opens and closes an interactive edit without a single successful
	// MoveNode/MoveApronCorner (the cursor never left the node, or every attempted move was
	// refused) is not a drag at all. Before issue #165, that sequence fired no notify
	// whatsoever, because MoveNode's own notify simply never happened - an unconditional
	// Topology notify at EndInteractiveEdit would be a full rebuild that never used to run.
	{
		const int32 TopologyBeforeNoOp = Actor->TopologyRebuildCountForTest();
		const int32 RebuildsBeforeNoOp = Actor->RebuildCountForTest();
		Facade->BeginInteractiveEdit(TEXT("click, no drag"));
		Facade->EndInteractiveEdit(/*bKeep*/ true);

		TestEqual(TEXT("a Begin/End with no successful move in between runs the "
			"derived-graph pass zero times"),
			Actor->TopologyRebuildCountForTest(), TopologyBeforeNoOp);
		TestEqual(TEXT("and rebuilds the surface zero times either - nothing changed at all"),
			Actor->RebuildCountForTest(), RebuildsBeforeNoOp);
	}

	// --- A BARE call outside Begin/End must catch itself up - NOT HERE ---------------------
	//
	// THE BARE-CALL TRAP. A MoveNode call with no wrapping BeginInteractiveEdit/EndInteractiveEdit has no
	// later Topology notify coming from anyone: its own notify is the only one that move will ever get, so
	// it has to be Topology, not Geometry, or the derived graph would go stale the moment a caller forgot
	// to wrap a single move in an interactive edit. That section was this file's, and is now the table row
	// MutatorNotifiesExactlyOnce.MoveNode.Bare (MeshFreshnessTest.cpp); this file's own control
	// assertions above still make bare PlaceNode/ConnectNode calls of the same shape.

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #190: the Geometry/Topology split above measured PIE - `GetWorld()` on the bare
// NewObject actor FDragNotifiesGeometryOnlyTest builds is null, and HistoryForEdit() treats a
// null world exactly like a game world, so that test's Facade always had History to open. An
// actual editor world's HistoryForEdit() returns null BY DESIGN (the editor's own transaction
// system does the Memento's job) - which used to mean MoveNode/MoveApronCorner's old test,
// `Use != nullptr && Use->IsEditing()`, was false on EVERY editor-mode drag frame, and
// EndInteractiveEdit's own `History == nullptr` early return meant nothing ever fired the one
// Topology catch-up such a drag still owes the derived graph. URoadBuildEdMode's own
// Begin/EndInteractiveEdit calls bracket a drag exactly as PIE's do; only the facade's OLD test
// for "is one open" could not see it.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDragNotifiesGeometryOnlyInEditorWorldTest,
	"Airside.Present.DragNotifiesGeometryOnlyInEditorWorld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDragNotifiesGeometryOnlyInEditorWorldTest::RunTest(const FString& Parameters)
{
	// AN ACTUAL EWorldType::Editor WORLD, not a bare NewObject actor - see this test's own
	// banner comment for why that distinction is the whole point. Same fixture
	// RoadBuildEdModeSessionTest uses for URoadBuildEdMode's own editor-world tests.
	//
	// BRACE-INITIALISED rather than parenthesised, since a local named TestWorld built with
	// arguments starting in a slash-star comment reads to Check-Architecture.ps1's rule 9 as
	// an assertion call missing its reason string. Braces call the same constructor without
	// matching that pattern.
	FAirsideTestWorld TestWorld{/*bSpawnActor=*/true, EWorldType::Editor};
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor spawned into it"), Actor)) { return false; }

	const int32 A = Actor->PlaceNode(FVector2D(0.0, 0.0));
	const int32 B = Actor->PlaceNode(FVector2D(10000.0, 0.0));
	if (!TestTrue(TEXT("the two nodes connect"), Actor->ConnectNodes(A, B)))
	{
		return false;
	}

	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade))
	{
		return false;
	}

	// THE PRECONDITION THIS TEST EXISTS TO CHECK: an editor world's HistoryForEdit() really
	// does return null here, the same as URoadBuildEdMode::GetSession's world does in the real
	// editor. If this ever started returning non-null, the rest of this test would measure
	// nothing FDragNotifiesGeometryOnlyTest was not already measuring in PIE.
	if (!TestNull(TEXT("an editor world hands out no history to open"), Facade->HistoryForEdit()))
	{
		return false;
	}

	const int32 RebuildsBeforeDrag = Actor->RebuildCountForTest();
	const int32 TopologyBeforeDrag = Actor->TopologyRebuildCountForTest();

	// THREE FRAMES OF ONE DRAG, same shape as FDragNotifiesGeometryOnlyTest's PIE case -
	// exactly what URoadBuildEditorTool's OnClickDrag calls, Begin once, MoveNode per frame,
	// End once.
	Facade->BeginInteractiveEdit(TEXT("drag node"));
	TestTrue(TEXT("drag frame 1 moves"), Actor->MoveNode(B, FVector2D(10500.0, 500.0)));
	TestTrue(TEXT("drag frame 2 moves"), Actor->MoveNode(B, FVector2D(11000.0, 900.0)));
	TestTrue(TEXT("drag frame 3 moves"), Actor->MoveNode(B, FVector2D(11500.0, 1200.0)));
	Facade->EndInteractiveEdit(/*bKeep*/ true);

	TestEqual(TEXT("every drag frame still rebuilt the surface, plus one for the commit"),
		Actor->RebuildCountForTest(), RebuildsBeforeDrag + 4);

	// THE MEASUREMENT: the derived-graph pass ran exactly once for the whole drag, at
	// EndInteractiveEdit, not once per MoveNode call and not zero times either - the shape
	// that fails on unpatched main, where an editor-world drag either ran the pass every
	// frame (the old `bMidInteractiveEdit` test never true) or never at all (EndInteractiveEdit
	// a no-op with no History to gate on).
	TestEqual(TEXT("but the guideline/anchor/plots/traffic pass ran exactly once, at the "
		"drag's end - not once per frame and not never"),
		Actor->TopologyRebuildCountForTest(), TopologyBeforeDrag + 1);

	// --- An editor undo mid-drag: PR #247's FEditorUndoClient still deactivates the tool -----
	//
	// DRIVEN, NOT RESTATED. This block used to open a drag and call EndInteractiveEdit itself - the
	// drag above again - so it called no undo and passed with the deactivation path deleted. The
	// editor's Ctrl+Z reaches the tool through URoadBuildEditorTool::DeactivateOnUndo, which hands
	// FBuildSession::OnNetworkReplaced the same call PIE's controller makes, and that deactivates the
	// active tool: FEditTool::OnDeactivate ends the drag it holds (EndInteractiveEdit(bKeep=true), as
	// OnDragEnd does). With no History in this world - asserted above - that call is the ONLY thing that
	// gives a drag cut short its one derived-graph catch-up. A real FEditTool holds the drag here and the
	// session's own OnNetworkReplaced (Adopted: the phase an undo sends) cuts it short; DeactivateOnUndo's
	// own hop to the session is Airside.Editor.UndoDeactivatesTheActiveBuildTool, which AirsideTests, with
	// no dependency on the editor module, cannot call. KEEP, NOT ABANDON: OnDeactivate's call is
	// bKeep=true because an editor Ctrl+Z does not reach into a live drag to abandon it - it ends the
	// interaction the same way releasing the mouse would.
	{
		FBuildSession Session;
		Session.SelectTool(1);                       // Taxiway: lights AirsideNode handles
		Session.SetGestureMode(EGestureMode::Edit);
		FBuildSessionTunables Tunables;

		IBuildTool* Tool = Session.GetActiveTool();
		if (!TestNotNull(TEXT("an edit tool is active"), Tool)) { return false; }
		Tool->OnDragBegin(Session.MakeContext(Actor, FVector2D(0.0, 0.0), Tunables));

		const int32 TopologyBeforeUndo = Actor->TopologyRebuildCountForTest();
		const FVector2D BeforeDrag = Actor->GetNetwork()->GetNodes()[A].Position;
		const FToolContext Mid = Session.MakeContext(Actor, FVector2D(500.0, 500.0), Tunables);
		Tool->OnDrag(Mid);
		if (!TestFalse(TEXT("the drag moved the grabbed node, or there is nothing for the undo to cut short"),
			Actor->GetNetwork()->GetNodes()[A].Position.Equals(BeforeDrag, 1.0)))
		{
			return false;
		}
		TestEqual(TEXT("and a drag frame runs no derived-graph pass of its own"),
			Actor->TopologyRebuildCountForTest(), TopologyBeforeUndo);

		Session.OnNetworkReplaced(Mid, ENetworkReplace::Adopted);

		TestEqual(TEXT("a drag cut short by the undo's deactivation still runs the derived-graph pass "
			"exactly once"),
			Actor->TopologyRebuildCountForTest(), TopologyBeforeUndo + 1);
	}

	return true;
}

// ---------------------------------------------------------------------------------------
// Issue #446: "the network changed" reached its dependants four ways - the buildings heard
// OnTopologyRebuilt, traffic a direct call made AFTER that broadcast, the controller's runway
// cache the facade's OnChanged, and ops a per-frame poll. ARoadNetworkActor::OnNetworkChanged
// is the one announcement now, after every rebuild of every kind, with traffic told first.
// Both tests measure the composition (a spawned actor, its facade and its traffic), where a
// broadcast moved above the traffic call, or a kind that skipped the announcement, would show.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNetworkChangedTrafficHearsFirstTest,
	"Airside.Present.NetworkChanged.TrafficHearsFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNetworkChangedTrafficHearsFirstTest::RunTest(const FString& Parameters)
{
	// A LISTENER THAT READS TRAFFIC IN ITS HANDLER must find the agents already re-pointed at the
	// nodes the rebuild made - before #446 the buildings' broadcast ran first, and a listener then
	// read handles the builder had freed. The rebuild summary is the observable: it is written by
	// UGroundTraffic::OnGraphRebuilt, so reading it inside the handler says which ran first.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world to spawn into"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	UGroundTraffic* Model = Actor->GetTraffic() != nullptr ? Actor->GetTraffic()->GetModel() : nullptr;
	if (!TestNotNull(TEXT("the actor owns a traffic model"), Model)) { return false; }

	// AUTHORED guidelines (bDerived false), TrafficForwarders' reason: a rebuild sweeps derived ones.
	const FGuidelineNodeId A = Net.AddGuidelineNode(FVector2D(0.0, 0.0), false);
	const FGuidelineNodeId B = Net.AddGuidelineNode(FVector2D(30000.0, 0.0), false);
	TestGraph::Join(Net, A, B, { EGuidelineDir::Bidirectional, nullptr, false });
	const FRoutePlan Plan = TestGraph::Probe(Net, A, B, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("setup: a route for the van"), Plan.IsValid())) { return false; }
	if (!TestTrue(TEXT("setup: the van is dispatched"),
		Actor->DispatchAgent(Plan, UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle))) { return false; }
	// THE CONTROL: the last rebuild (PlaceNode's) ran with no agent, so a reading of 1+ below can only be this rebuild's.
	TestEqual(TEXT("control: no rebuild has re-resolved the van yet"), Model->GetLastRebuildSummaryForTest().ReResolved, 0);

	int32 Announcements = 0;
	int32 ReResolvedWhenHeard = INDEX_NONE;
	const FDelegateHandle Handle = Actor->OnNetworkChanged.AddLambda(
		[&Announcements, &ReResolvedWhenHeard, Model](EChangeKind Kind, const URoadNetwork& Network)
		{
			++Announcements;
			ReResolvedWhenHeard = Model->GetLastRebuildSummaryForTest().ReResolved;
		});
	Actor->RebuildMesh();
	Actor->OnNetworkChanged.Remove(Handle);

	TestEqual(TEXT("one rebuild, one announcement"), Announcements, 1);
	TestTrue(TEXT("the listener found the van already re-resolved - traffic heard the rebuild BEFORE the broadcast (#446)"),
		ReResolvedWhenHeard >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNetworkChangedEveryRebuildOnceTest,
	"Airside.Present.NetworkChanged.EveryRebuildAnnouncedOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FNetworkChangedEveryRebuildOnceTest::RunTest(const FString& Parameters)
{
	// ONE ANNOUNCEMENT PER REBUILD, CARRYING THE REBUILD'S KIND - a road edit (Topology), a runway flip (Facts, #446:
	// no derived-graph pass) and a save game's load (RestoreInPlace: exactly one, Topology, after the repairs - #426's
	// path, which ops' FNetworkChangedEvent now rides). A kind that skipped the announcement, or a load that announced
	// twice (once from the restore, once from its adopt), would show as a count here.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor spawned"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	Actor->MinimumRunwayLength = 100.0;

	TArray<EChangeKind> Heard;
	const FDelegateHandle Handle = Actor->OnNetworkChanged.AddLambda(
		[&Heard](EChangeKind Kind, const URoadNetwork& Network) { Heard.Add(Kind); });

	if (!TestTrue(TEXT("setup: a runway is placed"),
		Actor->PlaceRunway(FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0), TestProfiles::Runway()))) { return false; }
	TestTrue(TEXT("a road edit is announced once, as Topology"), Heard.Num() == 1 && Heard[0] == EChangeKind::Topology);

	Heard.Reset();
	const int32 TopologyBefore = Actor->TopologyRebuildCountForTest();
	const uint32 RevisionBefore = Actor->Network->GetGuidelineRevision();
	FRunwayFacts Facts;
	Facts.Surface = EPavement::Concrete;
	if (!TestTrue(TEXT("setup: the runway is reclassified"), Actor->SetRunwayFacts(0, Facts))) { return false; }
	TestTrue(TEXT("a runway flip is announced once, as Facts"), Heard.Num() == 1 && Heard[0] == EChangeKind::Facts);
	TestEqual(TEXT("and derives no graph"), Actor->TopologyRebuildCountForTest(), TopologyBefore);
	TestNotEqual(TEXT("yet the model moved the clock the caches read - the flip is not invisible to them"),
		Actor->Network->GetGuidelineRevision(), RevisionBefore);

	Heard.Reset();
	URoadEditFacade* Facade = Actor->GetEditFacade();
	if (!TestNotNull(TEXT("the actor has a facade"), Facade)) { return false; }
	TestTrue(TEXT("setup: a load (an in-place restore that changes nothing) succeeds"),
		Facade->RestoreInPlace([](URoadNetwork&) { return true; }));
	TestTrue(TEXT("a load is announced exactly once, as Topology"), Heard.Num() == 1 && Heard[0] == EChangeKind::Topology);

	Actor->OnNetworkChanged.Remove(Handle);
	return true;
}

#endif
