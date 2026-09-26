#include "CoreMinimal.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEditorTool.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ISSUE #304: `RoadBuildEditorTool.cpp`'s `FViewportPreviewSink::Label` used to do "deliberately
 * nothing", so a tool's refusal reason had nowhere to land in the editor - PIE's own
 * `ARoadBuildHUD::Label` has always drawn the identical call. `FRunwayTool::BuildPreview` is the
 * concrete case the issue names: after one threshold is placed, a hover short of
 * `GetMinimumRunwayLength()` describes itself with `Sink.Label(Far, "too short: ...")`, which the
 * editor viewport swallowed silently while PIE showed it.
 *
 * COMPOSITION LEVEL, not FRunwayTool's own unit test (RunwayToolTest.cpp already pins the too-
 * short JUDGEMENT itself) - this drives the REAL `URoadBuildEditorTool` ITF callbacks
 * (OnClickPress/OnClickRelease/OnUpdateHover) the way a player's mouse actually would, and reads
 * back through `CollectPreviewLabelTextForTest` - the same "no live viewport" substitution
 * `SetViewCentreDistanceForTest`/`HoverFrameContextForTest` already use - rather than calling
 * `FRunwayTool::BuildPreview` directly, which would prove the tool's OWN logic but nothing about
 * whether the editor tool's DrawHUD ever asks for it.
 *
 * WRITTEN RED FIRST: before `FViewportPreviewSink` collected labels and
 * `CollectPreviewLabelTextForTest` existed, this test could not compile - the exact shape "a list
 * nothing consumes" takes when the list in question is a virtual call's return value rather than
 * a table.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildEditorToolPreviewLabelTest,
	"Airside.Editor.DrawHUDShowsRefusalLabels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildEditorToolPreviewLabelTest::RunTest(const FString& Parameters)
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

	// The runway tool, found by its registry key rather than a literal index - same precedent
	// as FRoadBuildEdModeSessionTest's own lookup.
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

	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = RunwayIndex;
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);
	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}

	// Setup() FIRST, THEN SetTargetForTest - Setup's own ResolveTarget finds nothing through the
	// bare UInteractiveToolManager this harness builds (no world of its own), same order
	// Airside.Editor.UndoDeactivatesTheActiveBuildTool already established.
	Tool->Setup();
	Tool->SetTargetForTest(TestWorld.Actor);

	const double SurfaceZ = TestWorld.Actor->SurfaceZ;
	const auto RayAt = [SurfaceZ](const FVector2D& Plane)
	{
		// Straight down from well above the plane, so the ray always hits it directly under
		// (Plane.X, Plane.Y) regardless of SurfaceZ's own value.
		return FInputDeviceRay(
			FRay(FVector(Plane.X, Plane.Y, SurfaceZ + 1000.0), FVector(0.0, 0.0, -1.0)), Plane);
	};

	// FIRST CLICK: a press and release that never travelled, so OnClickRelease resolves it as a
	// click (FBuildGesture) rather than a drag - FRunwayTool::OnClick's first call sets
	// Threshold and returns, arming the "hover describes Far" branch BuildPreview reads below.
	Tool->OnClickPress(RayAt(FVector2D(0.0, 0.0)));
	Tool->OnClickRelease(RayAt(FVector2D(0.0, 0.0)));

	// THE HOVER: 1 m from the threshold, comfortably under ARoadNetworkActor::
	// MinimumRunwayLength's default (500 m). No second click needed - BuildPreview reads
	// Context.GuidedCursor() as "Far" live, which is what makes the refusal visible before it
	// is ever committed to.
	Tool->OnUpdateHover(RayAt(FVector2D(100.0, 0.0)));

	const TArray<FString> Labels = Tool->CollectPreviewLabelTextForTest();
	bool bFoundTooShort = false;
	for (const FString& Label : Labels)
	{
		if (Label.Contains(TEXT("too short")))
		{
			bFoundTooShort = true;
			break;
		}
	}
	TestTrue(TEXT("a too-short runway's refusal label reaches the editor (issue #304) - "
		"FViewportPreviewSink::Label used to do nothing at all"), bFoundTooShort);

	Tool->Shutdown(EToolShutdownType::Cancel);
	return true;
}

#endif
