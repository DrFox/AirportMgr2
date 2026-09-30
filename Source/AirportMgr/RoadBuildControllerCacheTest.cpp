#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadNetwork.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * HasRunway USED TO RE-WALK THE NETWORK EVERY CALL, on the stated ground that "a cache would
 * need invalidating on every edit... for a saving nobody would measure" (its own old comment,
 * quoted in the fix). That premise stopped holding once it became one of roughly thirty rows
 * UBuildBarWidget::RefreshState asks every tick (issue #187) - this measures the cache
 * directly, the same way FPlayerTickBuildsOneContextTest measures issue #167's, rather than
 * trusting a trace of the call sites by eye.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHasRunwayCachesUntilTopologyChangesTest,
	"AirportMgr.Actions.HasRunwayCachesUntilTopologyChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHasRunwayCachesUntilTopologyChangesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	// Binds the cache invalidation exactly as BeginPlay would - see SetTargetForTest's own
	// comment on why this bypasses the level search rather than skipping the binding.
	C->SetTargetForTest(Target);

	C->HasRunway();
	const int32 AfterFirstCall = C->HasRunwayRecomputeCountForTest();
	C->HasRunway();
	C->HasRunway();
	TestEqual(TEXT("repeated queries with nothing changed do not re-walk the network"),
		C->HasRunwayRecomputeCountForTest(), AfterFirstCall);

	// A TOPOLOGY EDIT, not a runway - the cache is invalidated by the KIND of change, not by
	// whether this particular edit happened to add a runway. PlaceNode's CommitAndNotify
	// notifies EChangeKind::Topology by construction (every scope-committing mutator except
	// MoveNode does - see URoadEditFacade's own class comment).
	Target->PlaceNode(FVector2D(0.0, 0.0));

	C->HasRunway();
	TestTrue(TEXT("a topology change invalidates the cache, so the next query re-walks it"),
		C->HasRunwayRecomputeCountForTest() > AfterFirstCall);

	return true;
}

namespace
{
	/** Registry index of the tool with this Id, or INDEX_NONE. LoadRetires prefix: unity build. */
	int32 LoadRetiresToolIndex(const TCHAR* Id)
	{
		for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
		{
			if (ToolRegistry()[Index].Id == FName(Id)) { return Index; }
		}
		return INDEX_NONE;
	}

	/** A click straight into the active tool - the controller's own click path needs a viewport a test has not got. */
	void LoadRetiresClick(ARoadBuildController& C, ARoadNetworkActor& Target, const FVector2D& At)
	{
		FToolContext Click;
		Click.Target = &Target;
		Click.Cursor = At;
		C.GetActiveTool()->OnClick(Click);
	}
}

/**
 * #426 PIN: A LOAD IS ANNOUNCED. UOpsRuntime::LoadFromSlot replaced the airport in place and told nobody, while Undo,
 * Redo and Clear each deactivated the tool and retired the caches by hand in the controller. So after a load the
 * taxiway tool still chained from a slot INDEX into the pre-load graph, the readout served the pre-load answer, and
 * HasRunway - which gates Land - stayed false over a loaded airport with a runway. Driven through the RUNTIME, not the
 * controller's QuickLoad: a load from a menu, or any future replace-the-airport path, must reach the driver the same way.
 *
 * THE SAVED AIRPORT HAS A BARE NODE IN SLOT 0, and the chain starts from a node it placed in slot 0 of the session's
 * graph: a tool abandoned AFTER the load would cancel into the loaded graph and delete that node (FRoadChainingState::
 * OnCancel removes a bare node its chain created, by index). So the tool must let go while the old graph is live.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadRetiresToolAndCachesTest,
	"AirportMgr.Actions.LoadRetiresTheToolAndCaches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLoadRetiresToolAndCachesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	// THE SAVED AIRPORT: a bare node first (slot 0), then a runway.
	const int32 Bare = Target->PlaceNode(FVector2D(0.0, 30000.0));
	if (!TestEqual(TEXT("the saved airport's bare node is slot 0"), Bare, 0)) { return false; }
	Target->MinimumRunwayLength = 100.0;
	Target->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), TestProfiles::Runway());
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Target);
	const FString Slot = TEXT("AirportMgrTest_LoadRetires");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

	// THE SESSION IT IS LOADED OVER: no runway, and the taxiway tool chaining from a node its first click placed.
	Target->ClearNetwork();
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Target);
	C->InitInputSystem();

	const int32 Taxiway = LoadRetiresToolIndex(TEXT("Taxiway"));
	if (!TestTrue(TEXT("the registry names a Taxiway tool"), Taxiway != INDEX_NONE)) { return false; }
	C->SelectTool(Taxiway);
	LoadRetiresClick(*C, *Target, FVector2D(-20000.0, 0.0));
	if (!TestFalse(TEXT("the tool is mid-chain"), C->GetActiveTool()->IsIdle())) { return false; }

	// A SELECTION BY INDEX, as the Select tool leaves one: stand slot 0 of the session's graph.
	FSelection Picked;
	Picked.Kind = ESelectionKind::Stand;
	Picked.Id = 0;
	C->SelectForTest(Picked);
	if (!TestTrue(TEXT("something is selected before the load"), C->GetSelection().IsSet())) { return false; }

	// EVERY CACHE WARM AND PROVEN WARM, after the click (which itself invalidates them): the checks below then measure
	// the load, not an earlier edit.
	if (!TestFalse(TEXT("no runway before the load"), C->HasRunway())) { return false; }
	const int32 RunwayWalks = C->HasRunwayRecomputeCountForTest();
	C->PlayerTickForTest(1.0f / 60.0f);
	C->PlayerTickForTest(1.0f / 60.0f);
	const int32 Contexts = C->MakeContextCallCountForTest();
	const int32 Readout = C->GetToolReadoutRevision();
	C->PlayerTickForTest(1.0f / 60.0f);
	if (!TestEqual(TEXT("a still frame is a frame-context cache hit - or the check below measures nothing"),
			C->MakeContextCallCountForTest(), Contexts)
		|| !TestEqual(TEXT("and a readout cache hit"), C->GetToolReadoutRevision(), Readout)
		|| !TestFalse(TEXT("HasRunway still answers no"), C->HasRunway())
		|| !TestEqual(TEXT("from its cache"), C->HasRunwayRecomputeCountForTest(), RunwayWalks))
	{
		return false;
	}

	const uint32 Epoch = Target->GetEditEpoch();
	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }

	TestTrue(TEXT("the load moved the edit epoch (#451) - a tool's memoised refusal described the replaced graph"),
		Target->GetEditEpoch() != Epoch);
	TestTrue(TEXT("the load put the tool down - its pending node was a slot in the replaced graph"),
		C->GetActiveTool()->IsIdle());
	const TArray<FRoadNode>& Nodes = Target->Network->GetNodes();
	TestTrue(TEXT("and it let go BEFORE the load: the loaded airport's own bare node in that slot is still there"),
		Nodes.IsValidIndex(Bare) && Nodes[Bare].bAlive);
	TestFalse(TEXT("the selection was cleared - its index named a slot in the replaced graph"), C->GetSelection().IsSet());
	TestTrue(TEXT("HasRunway answers for the loaded airport, which has one - Land is not left greyed"), C->HasRunway());
	TestTrue(TEXT("by re-walking it, not from the pre-load cache"), C->HasRunwayRecomputeCountForTest() > RunwayWalks);
	// COUNTED FROM HERE: putting the tool down builds a context of its own (MakeToolContext), which is not the cache.
	const int32 AfterLoad = C->MakeContextCallCountForTest();
	C->PlayerTickForTest(1.0f / 60.0f);
	TestEqual(TEXT("the first frame after the load rebuilds the frame context: its key (target, tool, cursor, tunables) "
		"is unchanged, so only the announcement can retire it"), C->MakeContextCallCountForTest(), AfterLoad + 1);
	TestTrue(TEXT("and re-asks the readout, which described the replaced graph"), C->GetToolReadoutRevision() > Readout);
	return true;
}

/**
 * THE SAME ANNOUNCEMENT FOR AN UNDO THE CONTROLLER DID NOT MAKE (#426). The controller used to deactivate the tool in
 * its own OnUndo, so an undo from anywhere else - the settings dialog's Revert (FGamePlayerSettingsSink::Revert),
 * Blueprint - left a chain holding a node the undo had just changed underneath it. The facade announces now; this
 * undoes straight on the actor and expects the driver to hear it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUndoAnywhereRetiresToolTest,
	"AirportMgr.Actions.UndoFromAnywhereRetiresTheTool",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUndoAnywhereRetiresToolTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }
	Target->PlaceNode(FVector2D(0.0, 30000.0));
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(Target);
	C->InitInputSystem();

	const int32 Taxiway = LoadRetiresToolIndex(TEXT("Taxiway"));
	if (!TestTrue(TEXT("the registry names a Taxiway tool"), Taxiway != INDEX_NONE)) { return false; }
	C->SelectTool(Taxiway);
	LoadRetiresClick(*C, *Target, FVector2D(-20000.0, 0.0));
	if (!TestFalse(TEXT("the tool is mid-chain"), C->GetActiveTool()->IsIdle())) { return false; }

	if (!TestTrue(TEXT("an undo, straight on the actor"), Target->Undo())) { return false; }
	TestTrue(TEXT("the driver heard it and put the tool down"), C->GetActiveTool()->IsIdle());
	return true;
}

#endif
