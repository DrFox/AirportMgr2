#include "CoreMinimal.h"
#include "BuildCameraComponent.h"
#include "Misc/AutomationTest.h"
#include "Present/RoadNetworkActor.h"
#include "Content/AirsideSettings.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/AirsideTraffic.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

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

/**
 * AN ALERT'S GO LEAVES WATCH MODE (ops batch 3 PR E - FocusOn's first clause had no test): riding an aircraft and
 * being sent somewhere else cannot both hold, and a Go that only moved the hidden build view would leave the player
 * riding the aircraft, looking at nothing new.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraFocusOnLeavesWatchTest,
	"Airside.View.BuildCameraComponent.FocusOnLeavesWatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCameraFocusOnLeavesWatchTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	URoadNetwork& Net = *Actor->Network;
	const FGuidelineNodeId A = TestGraph::Node(Net, 0.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(Net, 200000.0, 0.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(Net, A, B, Options);
	if (!TestTrue(TEXT("dispatched"), Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft),
		UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Id = Actor->GetTraffic()->GetNewestAgentId();

	UBuildCameraComponent* Camera = NewObject<UBuildCameraComponent>(GetTransientPackage());
	if (!TestTrue(TEXT("it rides the aircraft"), Camera->ToggleWatchAgent(*Actor, Id) && Camera->IsWatchingAgent())) { return false; }
	const FVector2D There(12000.0, -3400.0);
	Camera->FocusOn(There);
	TestFalse(TEXT("a Go leaves watch mode"), Camera->IsWatchingAgent());
	TestEqual(TEXT("and the build view is sent there"), Camera->ViewFocus(), There);
	return true;
}

/**
 * THE COMPONENT'S DOORS HONOUR THE LAND: an alert's Go to a point past the edge lands on owned ground, and setting
 * the land (a purchase, through OnOwnedLandChanged) pulls a view already outside back in. The rig test pins the
 * clamp; this pins that the build view's own writers go through it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildCameraFocusLandTest,
	"Airside.View.BuildCameraComponent.FocusLandHoldsTheBuildView",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildCameraFocusLandTest::RunTest(const FString& Parameters)
{
	UBuildCameraComponent* Camera = NewObject<UBuildCameraComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("component constructed"), Camera))
	{
		return false;
	}
	const FIntPoint Tiles[] = { FIntPoint(0, 3), FIntPoint(0, 4) };
	const FLandGrid Land = FLandGrid::Make(FVector2D::ZeroVector, 60000.0, 8, 8, Tiles);

	Camera->FocusOn(FVector2D(1.0e6, 1.0e6));
	Camera->SetFocusLand(Land);
	TestEqual(TEXT("setting the land pulls a view already outside back to owned ground"),
		Camera->ViewFocus(), Land.ClampToOwned(FVector2D(1.0e6, 1.0e6)));

	Camera->FocusOn(FVector2D(-50000.0, 200000.0));
	TestEqual(TEXT("a Go past the edge lands on the edge"), Camera->ViewFocus(), FVector2D(0.0, 200000.0));

	Camera->SetFocusLand(FLandGrid());
	Camera->FocusOn(FVector2D(-50000.0, 200000.0));
	TestEqual(TEXT("an invalid grid releases it"), Camera->ViewFocus(), FVector2D(-50000.0, 200000.0));
	return true;
}

#endif
