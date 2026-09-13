#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tool/BuildGesture.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildGestureTest,
	"Airside.Tool.BuildGesture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildGestureTest::RunTest(const FString& Parameters)
{
	// 1. A press that never travels past the threshold is a click on release.
	{
		FBuildGesture Gesture;
		Gesture.Press(FVector2D(0.0, 0.0));
		TestTrue(TEXT("a small move stays None"),
			Gesture.Move(FVector2D(1.0, 0.0), 4.0) == EGestureStep::None);
		TestTrue(TEXT("release without ever crossing the threshold is a Click"),
			Gesture.Release() == EGestureEnd::Click);
	}

	// 2. Crossing the threshold reports DragBegan exactly once, then Dragging.
	{
		FBuildGesture Gesture;
		Gesture.Press(FVector2D(0.0, 0.0));
		TestTrue(TEXT("moving past the threshold reports DragBegan"),
			Gesture.Move(FVector2D(10.0, 0.0), 4.0) == EGestureStep::DragBegan);
		TestTrue(TEXT("staying past it reports Dragging, not DragBegan again"),
			Gesture.Move(FVector2D(11.0, 0.0), 4.0) == EGestureStep::Dragging);
		TestTrue(TEXT("releasing a drag reports DragEnd"),
			Gesture.Release() == EGestureEnd::DragEnd);
	}

	// 3. A release with nothing pressed does nothing - a stray mouse-up, or a second call.
	{
		FBuildGesture Gesture;
		TestTrue(TEXT("release with no press is Nothing"), Gesture.Release() == EGestureEnd::Nothing);

		Gesture.Press(FVector2D(0.0, 0.0));
		Gesture.Release();
		TestTrue(TEXT("a second release after the first is also Nothing"),
			Gesture.Release() == EGestureEnd::Nothing);
	}

	// 4. Cancel drops the gesture without resolving it either way - Escape mid-drag must
	//    not read as a click on whatever happens to release the mouse next.
	{
		FBuildGesture Gesture;
		Gesture.Press(FVector2D(0.0, 0.0));
		Gesture.Move(FVector2D(10.0, 0.0), 4.0);
		TestTrue(TEXT("dragging before cancel"), Gesture.IsDragging());

		Gesture.Cancel();
		TestFalse(TEXT("cancel clears the drag flag"), Gesture.IsDragging());
		TestTrue(TEXT("a release after cancel is Nothing, not a resolved gesture"),
			Gesture.Release() == EGestureEnd::Nothing);
	}

	// 5. Move with nothing pressed is always None - a hover tick must not be mistaken for a
	//    drag in progress.
	{
		FBuildGesture Gesture;
		TestFalse(TEXT("not pressed"), Gesture.IsPressed());
		TestTrue(TEXT("a move with no press is None"),
			Gesture.Move(FVector2D(1000.0, 1000.0), 4.0) == EGestureStep::None);
	}

	return true;
}

#endif
