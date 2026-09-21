#include "CoreMinimal.h"
#include "InteractiveToolManager.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildEdMode.h"
#include "RoadBuildEditorTool.h"
#include "Tool/BuildGesture.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The seam FBuildGesture replaced (issue #92) is tested in isolation by
 * Airside.Tool.BuildGesture, which proves the recogniser's own rules. It proves nothing
 * about whether URoadBuildEditorTool's ITF callbacks actually reach it - a driver that
 * stopped calling Gesture.Press/Move/Release entirely would still pass that test, the same
 * way a tool with no key binding still passed every per-tool test before issue #33.
 *
 * This is the composition-level half: drive the REAL ITF callbacks
 * (OnClickPress/OnClickDrag/OnTerminateDragSequence) on a tool built through the real
 * builder, and check FBuildGesture's own state afterwards through GestureForTest.
 *
 * No world or ARoadNetworkActor is set up - Target resolves to null, so every ray/plane
 * hit refuses and every IBuildTool callback the default Select tool receives is a no-op
 * (see IBuildTool::OnDragBegin/OnDrag's empty default bodies). That is deliberate: this
 * test is about whether the MOUSE gesture reaches FBuildGesture, not about what a tool does
 * with it, which the per-tool tests already cover.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildGestureCompositionTest,
	"Airside.Editor.ClickDragReachesFBuildGesture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildGestureCompositionTest::RunTest(const FString& Parameters)
{
	URoadBuildEdMode* Mode = NewObject<URoadBuildEdMode>(GetTransientPackage());
	if (!TestNotNull(TEXT("an ed mode"), Mode))
	{
		return false;
	}

	// Index 0, Select - the tool every activation starts on; which tool is irrelevant here.
	URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(Mode);
	Builder->ToolIndex = 0;

	// A real tool manager as the new tool's outer - see FRoadBuildEdModeSessionTest for why
	// BuildTool needs one rather than a null outer.
	FToolBuilderState State;
	State.ToolManager = NewObject<UInteractiveToolManager>(Mode);

	URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(Builder->BuildTool(State));
	if (!TestNotNull(TEXT("the builder made a tool"), Tool))
	{
		return false;
	}
	Tool->Setup();

	TestFalse(TEXT("nothing pressed yet"), Tool->GestureForTest().IsPressed());

	const auto DeviceRayAt = [](const FVector2D& Screen)
	{
		// The world ray never resolves (no Target), so its exact value does not matter -
		// straight down is as good as any other direction here.
		return FInputDeviceRay(FRay(FVector::ZeroVector, FVector(0.0, 0.0, 1.0)), Screen);
	};

	Tool->OnClickPress(DeviceRayAt(FVector2D(0.0, 0.0)));
	TestTrue(TEXT("a press reaches the gesture"), Tool->GestureForTest().IsPressed());
	TestFalse(TEXT("and is not yet a drag"), Tool->GestureForTest().IsDragging());

	// A QUARTER OF THE SHARED THRESHOLD, not a bare "1.0" (issue #191/#92-#93):
	// URoadBuildEditorTool no longer carries its own copy of this number - OnClickDrag calls
	// Gesture.Move with no threshold argument at all, so it runs against
	// FBuildGesture::DefaultThresholdPixels' default parameter. Deriving these two points from
	// that constant is what makes this test fail if a future change moves the shared default
	// without noticing this call site, rather than passing by coincidence forever.
	Tool->OnClickDrag(DeviceRayAt(FVector2D(FBuildGesture::DefaultThresholdPixels * 0.25, 0.0)));
	TestFalse(TEXT("a small move does not promote to a drag"), Tool->GestureForTest().IsDragging());

	// TWO AND A HALF TIMES THE SHARED THRESHOLD - comfortably past it, whatever its value.
	Tool->OnClickDrag(DeviceRayAt(FVector2D(FBuildGesture::DefaultThresholdPixels * 2.5, 0.0)));
	TestTrue(TEXT("a move past the threshold promotes to a drag"), Tool->GestureForTest().IsDragging());

	// Escape mid-drag: the transaction this opened must close, which is what would assert
	// (or leave a dangling transaction) if OnTerminateDragSequence stopped calling Cancel.
	Tool->OnTerminateDragSequence();
	TestFalse(TEXT("terminating the drag sequence clears the gesture entirely"),
		Tool->GestureForTest().IsPressed());
	TestFalse(TEXT("including the drag flag"), Tool->GestureForTest().IsDragging());

	Tool->Shutdown(EToolShutdownType::Cancel);
	return true;
}

#endif
