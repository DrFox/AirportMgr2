#include "CoreMinimal.h"
#include "BuildCameraComponent.h"
#include "BuildHudLayer.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE COMPOSITION-LEVEL HALF of issue #94's split. UBuildCameraComponent and UBuildHudLayer
 * are tested in isolation elsewhere (Airside.View.BuildCameraRig.ApplyLimitsAndReset,
 * Airside.View.BuildCameraComponent.ToggleWatchAgent), which proves nothing about whether
 * ARoadBuildController still actually OWNS one of each - a constructor that stopped calling
 * CreateDefaultSubobject would still pass every isolated test (IsWatchingAgent null-guards
 * to false, GetHudForTest would just return null quietly) while the game shipped a
 * controller with no camera and no HUD at all.
 *
 * World-spawn pattern from ClickModifierTest.cpp, the precedent for testing this actor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildControllerCameraTest,
	"AirportMgr.Actions.ControllerOwnsCameraAndHud",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildControllerCameraTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// 1. BOTH SUBOBJECTS EXIST. Not a rename-proof accident: dropping either
	// CreateDefaultSubobject call in the constructor would leave these null, and nothing
	// short of asking directly would notice - see this test's own class comment.
	UBuildCameraComponent* CameraComp = C->FindComponentByClass<UBuildCameraComponent>();
	TestNotNull(TEXT("the controller owns a UBuildCameraComponent"), CameraComp);
	TestNotNull(TEXT("the controller owns a UBuildHudLayer"), C->GetHudForTest());
	if (CameraComp == nullptr)
	{
		return false;
	}

	// 2. THE FORWARDER. ARoadBuildController::ZoomIn/ZoomOut bind the mouse wheel and must
	// actually reach the component's ZoomBy - a forwarder that silently did nothing would
	// leave the wheel dead with no compile error to catch it.
	//
	// CreateBuildCamera called directly (bypassing BeginPlay's TActorIterator search) so the
	// spawned camera actor exists for UpdateView to move - FAirsideTestWorld's own
	// ARoadNetworkActor stands in for "whatever the level has".
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target))
	{
		return false;
	}
	CameraComp->CreateBuildCamera(*C, *Target);
	const double StartDistance = CameraComp->ActiveRig().Distance;

	C->ZoomIn();
	// ZoomBy moves the TARGET the view eases towards, not ActiveRig() (CurrentView)
	// directly - see FBuildCameraRig::EaseToward's own comment for why the two are kept
	// separate. A generous DeltaTime (1 second, default CameraLag 0.12s) converges the two
	// to well within a rounding error, which is what lets this test observe the zoom
	// through ActiveRig() rather than through a private field.
	CameraComp->UpdateView(1.0f, 0.0, 0.0, 0.0, 0.0, Target);

	TestTrue(TEXT("ZoomIn moved the active rig closer, through the forwarder"),
		CameraComp->ActiveRig().Distance < StartDistance);

	return true;
}

/**
 * A MOUSE DRAG TURNS THE SAME AMOUNT WHATEVER THE FRAME RATE, and a held key does too.
 *
 * These are opposite requirements met by opposite code, which is why both are measured
 * here. A key is an AXIS - a constant while held - so it must be multiplied by DeltaTime.
 * A mouse delta is a DISTANCE ALREADY TRAVELLED this frame, so it must not be. Getting
 * either the wrong way round produces a camera that is subtly wrong only on other people's
 * machines: the bug does not reproduce at the frame rate it was written at.
 *
 * CameraLag is zeroed so the eased rig snaps to the target and the yaw can be read straight
 * off ActiveRig() rather than inferred through the easing curve.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraTurnIsFrameRateIndependentTest,
	"Airside.View.BuildCameraComponent.TurnIsFrameRateIndependent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCameraTurnIsFrameRateIndependentTest::RunTest(const FString& Parameters)
{
	// A SPAWNED CAMERA IS REQUIRED, not just a component: UpdateView returns immediately
	// without one, so a world-free version of this test measured nothing and reported a
	// cheerful zero degrees of rotation in both cases. Same world-spawn pattern as the
	// forwarder test above.
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	ARoadNetworkActor* Target = TestWorld.Actor;
	if (!TestNotNull(TEXT("a target actor"), Target)) { return false; }

	// One frame of input at a given frame rate, as a yaw delta. A fresh controller per
	// measurement so neither run inherits the other's eased state.
	auto YawAfter = [&](float DeltaTime, double Turn, double TurnPixels) -> double
	{
		ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
		if (C == nullptr) { return 0.0; }
		UBuildCameraComponent* Camera = C->FindComponentByClass<UBuildCameraComponent>();
		if (Camera == nullptr) { return 0.0; }
		Camera->CreateBuildCamera(*C, *Target);
		// Lag zeroed so the eased rig snaps to its target and the yaw can be read straight
		// off ActiveRig() instead of through the easing curve.
		Camera->CameraLag = 0.0;
		const double Before = Camera->ActiveRig().Yaw;
		Camera->UpdateView(DeltaTime, 0.0, 0.0, Turn, TurnPixels, Target);
		return Camera->ActiveRig().Yaw - Before;
	};

	constexpr float Slow = 1.0f / 30.0f;
	constexpr float Fast = 1.0f / 240.0f;

	// THE MOUSE: 100 px is 100 px, at 30 fps or 240.
	const double MouseSlow = YawAfter(Slow, 0.0, 100.0);
	const double MouseFast = YawAfter(Fast, 0.0, 100.0);
	TestTrue(TEXT("a mouse drag turns the view at all"), FMath::Abs(MouseSlow) > 1.0e-9);
	TestEqual(TEXT("and turns it identically at 30 and 240 fps"), MouseFast, MouseSlow, 1.0e-9);

	// THE KEY: held for a frame eight times as long it turns eight times as far, which is
	// the SAME property stated for a rate instead of a distance.
	const double KeySlow = YawAfter(Slow, 1.0, 0.0);
	const double KeyFast = YawAfter(Fast, 1.0, 0.0);
	TestTrue(TEXT("a held key turns the view at all"), FMath::Abs(KeyFast) > 1.0e-9);
	TestEqual(TEXT("and covers the same ground per SECOND, not per frame"),
		KeySlow / KeyFast, static_cast<double>(Slow) / static_cast<double>(Fast), 1.0e-6);
	return true;
}

#endif
