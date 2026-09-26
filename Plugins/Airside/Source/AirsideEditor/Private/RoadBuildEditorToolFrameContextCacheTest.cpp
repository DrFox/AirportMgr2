#include "CoreMinimal.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEditorTool.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE EDITOR TWIN OF AirportMgr.Actions.PlayerTickBuildsOneContext (issue #167's own test) - issue
 * #303. Before FBuildSession::GetFrameContext, OnUpdateHover, Render and DrawHUD each called
 * MakeContextAt independently: three runs of the whole snap + guide pipeline (a junction solve
 * per live node outside its cheap-reject radius, then the guide chain over every source) for one
 * cursor position, where PIE's PlayerTick had shared one since issue #167.
 *
 * Render and DrawHUD are stood in for by HoverFrameContextForTest, which calls the exact same
 * MakeHoverContext() either of them calls before doing anything render-specific - see that
 * method's own comment for why a real IToolsContextRenderAPI/FSceneView/FCanvas is not built
 * here, the same precedent Airside.Editor.PlaneHitRefusesBeyondTheHorizonCap already set for
 * this class.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverFrameBuildsOneContextTest,
	"Airside.Editor.HoverFrameBuildsOneContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoverFrameBuildsOneContextTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	if (!TestNotNull(TEXT("a target actor"), TestWorld.Actor)) { return false; }

	// TOOLINDEX IS NOT DRIVEN HERE: it only takes effect through Setup()'s own
	// Sess().SelectTool(ToolIndex, ...) call, which this test skips (see SetTargetForTest below),
	// so the session's active tool stays at its constructed default, Select. That is fine for
	// what this measures - GetFrameContext runs the whole snap + guide pipeline the same way
	// whatever tool is lit, since ResolveSnap and the guide chain do not switch on it.
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool)) { return false; }

	// SetTargetForTest, not Setup()'s own ResolveTarget: the bare UInteractiveToolManager this
	// harness constructs has no world of its own to find TestWorld.Actor in - same precedent as
	// the horizon-cap test beside this one.
	Tool->SetTargetForTest(TestWorld.Actor);

	const double SurfaceZ = TestWorld.Actor->SurfaceZ;
	const FVector StraightDown(0.0, 0.0, -1.0);
	const FRay Ray(FVector(1000.0, 500.0, SurfaceZ + 100000.0), StraightDown);

	const FBuildSession* Session = Tool->SessionForTest();
	if (!TestNotNull(TEXT("a session"), Session)) { return false; }

	const int32 Before = Session->MakeContextCallCountForTest();

	// ONE REAL HOVER EVENT - what OnUpdateHover fires from a moved mouse. Records the plane hit
	// AND builds this "frame's" context, exactly as ARoadBuildController::PlayerTick does for
	// PIE - see FBuildSession::GetFrameContext's own comment on why this is the hook that
	// stands in for the missing "top of frame".
	Tool->OnUpdateHover(FInputDeviceRay(Ray));

	// RENDER, THEN DrawHUD, in that order every frame - both read the SAME LastPlaneHit
	// OnUpdateHover just recorded, with nothing else changing PlaneHit/Tunables/modifiers in
	// between, so both must be served from the cache rather than rebuilding.
	Tool->HoverFrameContextForTest();
	Tool->HoverFrameContextForTest();

	const int32 After = Session->MakeContextCallCountForTest();

	TestEqual(TEXT("OnUpdateHover, Render and DrawHUD share one context for an unmoved cursor - "
					"this goes to 2 or 3 if either of the last two stops reading the cache"),
		After - Before, 1);

	return true;
}

/**
 * A KEY MISS STILL REBUILDS. Without this half the test above would also pass on a cache that
 * NEVER invalidates - always answering the first context it ever built, whatever the cursor did
 * next.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverFrameContextRebuildsWhenTheCursorMovesTest,
	"Airside.Editor.HoverFrameContextRebuildsWhenTheCursorMoves",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FHoverFrameContextRebuildsWhenTheCursorMovesTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	if (!TestNotNull(TEXT("a target actor"), TestWorld.Actor)) { return false; }

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool)) { return false; }
	Tool->SetTargetForTest(TestWorld.Actor);

	const double SurfaceZ = TestWorld.Actor->SurfaceZ;
	const FVector StraightDown(0.0, 0.0, -1.0);

	const FBuildSession* Session = Tool->SessionForTest();
	if (!TestNotNull(TEXT("a session"), Session)) { return false; }

	Tool->OnUpdateHover(FInputDeviceRay(FRay(FVector(0.0, 0.0, SurfaceZ + 100000.0), StraightDown)));
	const int32 AfterFirst = Session->MakeContextCallCountForTest();

	// A DIFFERENT CURSOR POSITION: the key's PlaneHit now differs, so this must NOT be served
	// from the cache the first hover populated.
	Tool->OnUpdateHover(FInputDeviceRay(FRay(FVector(4000.0, 4000.0, SurfaceZ + 100000.0), StraightDown)));
	const int32 AfterSecond = Session->MakeContextCallCountForTest();

	TestEqual(TEXT("a moved cursor rebuilds rather than reusing the first hover's context"),
		AfterSecond - AfterFirst, 1);

	return true;
}

#endif
