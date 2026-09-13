#include "CoreMinimal.h"
#include "BuildCameraComponent.h"
#include "BuildHudLayer.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"

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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

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
	// spawned camera actor exists for UpdateView to move - a fresh ARoadNetworkActor stands
	// in for "whatever the level has".
	ARoadNetworkActor* Target = World->SpawnActor<ARoadNetworkActor>();
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
	CameraComp->UpdateView(1.0f, 0.0, 0.0, 0.0, Target);

	TestTrue(TEXT("ZoomIn moved the active rig closer, through the forwarder"),
		CameraComp->ActiveRig().Distance < StartDistance);

	return true;
}

#endif
