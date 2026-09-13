#include "CoreMinimal.h"
#include "BuildCameraComponent.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The parts of UBuildCameraComponent reachable with no World and no spawned camera actor -
 * CreateBuildCamera/UpdateView need both, and are exercised in PIE rather than here (see
 * CLAUDE.md's runtime-verification table: this test proves the refusal path and ActiveRig's
 * own selection, not the eased camera pipeline).
 *
 * Named under "Airside." for the same reason BuildCameraRigTest.cpp is, despite living in
 * the game module: Run-AirsideTests.ps1 filters on that prefix by default.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraComponentTest,
	"Airside.View.BuildCameraComponent.ToggleWatchAgent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCameraComponentTest::RunTest(const FString& Parameters)
{
	UBuildCameraComponent* Camera = NewObject<UBuildCameraComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("component constructed"), Camera))
	{
		return false;
	}

	ARoadNetworkActor* Target = NewObject<ARoadNetworkActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor constructed"), Target))
	{
		return false;
	}

	// 1. NOT WATCHING by construction, and ActiveRig answers the build view - not the watch
	// rig, which has never been reset and would be a stale default otherwise.
	TestFalse(TEXT("a fresh component is not watching"), Camera->IsWatchingAgent());
	TestEqual(TEXT("ActiveRig is the build view's CurrentView while not watching"),
		Camera->ActiveRig().Distance, FBuildCameraRig().Distance, 1e-9);

	// 1b. THE PER-MODE DEFAULTS, PINNED. WatchLimits does NOT keep FCameraRigLimits' own
	// defaults - those are the BUILD view's numbers, and a component that forgot to set
	// WatchLimits explicitly would still construct, still pass every other test here, and
	// open the watch camera 8000 uu out at yaw 0 instead of framing the aircraft. Review
	// caught this once already; pinned directly so it cannot happen silently again.
	TestEqual(TEXT("WatchLimits.MinDistance is the watch rig's own number, not the view's"),
		Camera->WatchLimits.MinDistance, 800.0);
	TestEqual(TEXT("WatchLimits.MaxDistance"), Camera->WatchLimits.MaxDistance, 20000.0);
	TestEqual(TEXT("WatchLimits.MinPitch"), Camera->WatchLimits.MinPitch, 10.0);
	TestEqual(TEXT("WatchLimits.MaxPitch"), Camera->WatchLimits.MaxPitch, 60.0);
	TestEqual(TEXT("WatchLimits.StartDistance - 15.5 m, framing a 13 m wingspan"),
		Camera->WatchLimits.StartDistance, 1550.0);
	TestEqual(TEXT("WatchLimits.StartYaw - off the right wing and a little behind"),
		Camera->WatchLimits.StartYaw, -75.0);
	TestEqual(TEXT("ViewLimits, by contrast, IS FCameraRigLimits' own default distance"),
		Camera->ViewLimits.StartDistance, FCameraRigLimits().StartDistance);

	// 2. REFUSED: starting to watch an agent that does not exist must not flip the mode -
	// this is the "Nothing to follow" case ARoadBuildController::ToggleWatchAgent logs.
	// Target has no dispatched agents, so ANY id refuses.
	TestFalse(TEXT("toggling onto a nonexistent agent is refused"),
		Camera->ToggleWatchAgent(*Target, 12345));
	TestFalse(TEXT("and the mode did not flip"), Camera->IsWatchingAgent());
	TestEqual(TEXT("GetWatchAgentId stays at its default"), Camera->GetWatchAgentId(), 0);

	return true;
}

#endif
