#include "CoreMinimal.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEditorTool.h"
#include "Solve/RoadGeom.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE HORIZON CAP EXISTED ONLY IN PIE (issue #191/#92-#93, item (b)):
 * ARoadBuildController::CursorOnRoadPlane measures every click against
 * MaxPlaceDistanceFactor * the active camera's own distance, refusing one that lands too far
 * out - a click a few pixels above the horizon runs the ray/plane distance toward infinity
 * otherwise. URoadBuildEditorTool::RayToPlane passed TNumericLimits<double>::Max()
 * unconditionally, so the identical near-horizon click that PIE refused landed wherever the
 * ray happened to cross Z==SurfaceZ in the editor - kilometres out, silently.
 *
 * Render() already computed the view-centre distance every frame, to size ViewWorldWidth, and
 * threw it away rather than feeding it to RayToPlane. SetViewCentreDistanceForTest stands in
 * for a real Render call here, the same substitution SetTargetForTest makes for Setup's own
 * ResolveTarget - IToolsContextRenderAPI needs a live viewport this headless harness does not
 * have (see BuildGestureCompositionTest's own comment on the same class of gap).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolHorizonCapTest,
	"Airside.Editor.PlaneHitRefusesBeyondTheHorizonCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolHorizonCapTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World))
	{
		return false;
	}
	if (!TestNotNull(TEXT("a target actor"), TestWorld.Actor))
	{
		return false;
	}

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = 0;
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}

	// SetTargetForTest, not Setup()'s own ResolveTarget: the bare UInteractiveToolManager this
	// harness constructs has no world of its own to find TestWorld.Actor in.
	Tool->SetTargetForTest(TestWorld.Actor);

	const double SurfaceZ = TestWorld.Actor->SurfaceZ;
	const FVector StraightDown(0.0, 0.0, -1.0);
	FVector2D Hit;

	// UNCAPPED BEFORE ANY Render CALL - ViewCentreDistance's own default (-1) must keep
	// matching the old unconditional TNumericLimits<double>::Max(), or the very first frame of
	// every editor session (before Render has run once) would start refusing clicks that used
	// to work.
	TestTrue(TEXT("uncapped before Render has ever run, same as before this fix"),
		Tool->RayToPlaneForTest(FRay(FVector(0.0, 0.0, SurfaceZ + 1.0e6), StraightDown), Hit));

	// CAPPED, at the SAME multiple PIE uses, once a view-centre distance is on record - what
	// Render() sets every frame it actually runs.
	Tool->SetViewCentreDistanceForTest(1000.0);
	const double Cap = RoadGeom::DefaultMaxPlaceDistanceFactor * 1000.0;

	TestTrue(TEXT("well within the cap still resolves"),
		Tool->RayToPlaneForTest(FRay(FVector(0.0, 0.0, SurfaceZ + Cap * 0.5), StraightDown), Hit));
	TestFalse(TEXT("far past the cap refuses, matching CursorOnRoadPlane's own guard"),
		Tool->RayToPlaneForTest(FRay(FVector(0.0, 0.0, SurfaceZ + Cap * 50.0), StraightDown), Hit));

	// ORTHOGRAPHIC EXEMPTS ITSELF, same as Render's own comment: every ray shares the camera's
	// direction there, so the horizon runaway this guard exists for cannot happen. Render sets
	// ViewCentreDistance back to -1 in that branch; simulated directly here since this harness
	// has no IToolsContextRenderAPI to drive Render's own branch through.
	Tool->SetViewCentreDistanceForTest(-1.0);
	TestTrue(TEXT("uncapped again once the view-centre distance is unknown, as in orthographic"),
		Tool->RayToPlaneForTest(FRay(FVector(0.0, 0.0, SurfaceZ + Cap * 50.0), StraightDown), Hit));

	return true;
}

#endif
