#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEditorTool.h"
#include "Tool/BuildSession.h"
#include "Tool/RunwayTool.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The editor's build session outlives the tool instances that drive it.
 *
 * THE BUG THIS EXISTS FOR, which shipped and was found by hand. FBuildSession used to be a
 * member of URoadBuildEditorTool, and the InteractiveTools framework builds a NEW tool object
 * on every activation - so the session, and with it every FRunwayTool's WidthIndex, was
 * destroyed and rebuilt each time a tool was picked. In the editor a runway could therefore
 * only ever be laid at the first width in the list, while the same key cycled 18/23/30/45/60
 * perfectly well in PIE, where ARoadBuildController holds one long-lived session.
 *
 * Two separate things had to be true for that to happen, so this checks both: the session is
 * SHARED (the tool points at the mode's), and a reselect on the shared session actually
 * cycles the width. Either alone would pass while the feature stayed broken.
 *
 * A THIRD THING joined them at issue #78's review: the reselect's FToolContext needs a real
 * Target now that FRunwayTool::NextWidth reads runway profiles through IRoadEditTarget
 * instead of content directly, and URoadBuildEdMode::StartToolAction had stopped resolving
 * one. This drives the reselect through Mode->MakeReselectContext() - the exact resolution
 * StartToolAction uses - rather than building an unrelated ARoadNetworkActor of its own,
 * which is what let an earlier version of this test pass while the mode stayed broken.
 *
 * IN AirsideEditor rather than AirsideTests, for the reason ToolCommandsMatchRegistry gives:
 * AirsideTests depends on Airside alone, and making it reach an editor class would invert the
 * direction the plugin is built on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEdModeSessionTest,
	"Airside.Editor.ToolStateOutlivesTheToolInstance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEdModeSessionTest::RunTest(const FString& Parameters)
{
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	// A REAL WORLD, purely so MakeReselectContext's ARoadNetworkActor::FindOrCreate(GetWorld())
	// has something to search and spawn into - see WorldOverrideForTest's own comment for why
	// GetWorld() needs help here at all. Not the same as registering the mode with a live
	// editor (Enter() is never called); this substitutes for the ONE thing this test's
	// reselect assertions need from a real editor session.
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false);
	if (!TestNotNull(TEXT("a world for the mode to resolve a target in"), World))
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Editor);
	WorldContext.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};
	Mode->WorldOverrideForTest = World;

	// The runway tool, found by its registry key rather than a literal index - the table is
	// not contiguous and an index written here would be a second claim about its order.
	int32 RunwayIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ToolRegistry().Num(); ++Index)
	{
		if (ToolRegistry()[Index].Key == EKeys::Six)
		{
			RunwayIndex = Index;
		}
	}
	if (!TestTrue(TEXT("the registry has the runway tool on 6"), RunwayIndex != INDEX_NONE))
	{
		return false;
	}

	// THE SHARING, checked through the builder rather than by setting the pointer by hand:
	// it is the builder that has to do this, and a test that wired it itself would pass with
	// the builder unchanged - which is exactly how this shipped broken.
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = RunwayIndex;

	// A REAL tool manager, because BuildTool uses it as the new tool's OUTER and NewObject
	// with a null outer crashes rather than returning null - which is how the first run of
	// this test died, in BuildTool rather than in anything it was testing.
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);

	UInteractiveTool* Built = Builder->BuildTool(State);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Built);
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}
	TestEqual(TEXT("the tool drives the MODE's session, not one of its own"),
		Tool->SessionForTest(), static_cast<const FBuildSession*>(&Mode->GetSession()));

	// A second instance, as a second activation of the same palette entry would produce.
	URoadBuildEditorTool* Again = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("a second activation builds a second tool"), Again))
	{
		return false;
	}
	TestEqual(TEXT("and it drives the same session, so state survives the rebuild"),
		Again->SessionForTest(), Tool->SessionForTest());

	// THE RESELECT'S CONTEXT, THROUGH THE MODE - not a target this test builds itself.
	// StartToolAction's own lambda cannot be driven directly here: it is gated on
	// GetToolManager()/GetInteractiveToolsContext(), both null on a bare
	// NewObject<URoadBuildEdMode> with no Enter() and no real editor viewport (confirmed
	// against UEdMode.cpp - EditorToolsContext/ModeToolsContext are set only by
	// CreateInteractiveToolsContexts(), called from mode registration this test does not
	// do). Mode->MakeReselectContext() is what StartToolAction calls once that gate passes,
	// so calling it here exercises the SAME resolution a real reselect uses - unlike an
	// earlier version of this test, which built an unrelated ARoadNetworkActor of its own
	// and could not have caught the mode forgetting to resolve one (issue #78's review).
	FBuildSession& Session = Mode->GetSession();
	const FToolContext ReselectContext = Mode->MakeReselectContext();
	TestNotNull(TEXT("the mode resolves a real target for a reselect"), ReselectContext.Target);

	// THE RESELECT, on the shared session. Selecting the runway tool twice is what a second
	// press of its key does, and the second press must cycle the width rather than do nothing.
	Session.SelectTool(RunwayIndex, ReselectContext);
	FRunwayTool* Runway = static_cast<FRunwayTool*>(Session.GetActiveTool());
	if (!TestNotNull(TEXT("the runway tool is active on the shared session"), Runway))
	{
		return false;
	}
	TestEqual(TEXT("it starts on the first width"), Runway->WidthIndex, 0);

	Session.SelectTool(RunwayIndex, ReselectContext);
	TestEqual(TEXT("picking it again cycles the width - the press that did nothing before"),
		Runway->WidthIndex, 1);

	// And the choice survives the tool object being rebuilt, which is the whole point: the
	// same pointer, still at width 1, after a third activation.
	URoadBuildEditorTool* Third = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	TestNotNull(TEXT("a third activation builds"), Third);
	TestEqual(TEXT("the width chosen before it was rebuilt is still chosen"),
		static_cast<FRunwayTool*>(Mode->GetSession().GetActiveTool())->WidthIndex, 1);
	return true;
}

#endif
