#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "BuildActions.h"
#include "RoadBuildHUD.h"
#include "Tool/RoadBuildTool.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE COMMIT PROMPT'S ONE DECISION, with no Canvas and no PIE.
 *
 * The drawing itself cannot be tested headlessly, so the part that CAN go wrong silently is
 * split out: whether the prompt appears at all, and whether it names the key the registry
 * actually bound. A prompt reading "Build [Enter]" beside an unbound Enter is this project's
 * most-repeated bug - see CLAUDE.md on the startup banner that advertised four routes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCommitPromptNamesTheBoundKeyTest,
	"AirportMgr.HUD.CommitPromptNamesTheBoundKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FCommitPromptNamesTheBoundKeyTest::RunTest(const FString& Parameters)
{
	// NOTHING TO COMMIT, NOTHING OFFERED. bCommittable is false by default for every tool
	// but one, so a prompt that ignored it would hang over every gesture in the game.
	TestTrue(TEXT("an empty readout offers nothing"),
		ARoadBuildHUD::CommitPromptText(FToolReadout()).IsEmpty());

	FToolReadout Ready;
	Ready.bCommittable = true;
	const FString Prompt = ARoadBuildHUD::CommitPromptText(Ready);

	TestFalse(TEXT("a committable readout offers something"), Prompt.IsEmpty());
	TestTrue(TEXT("and it is the registry's own label"), Prompt.Contains(TEXT("Build")));

	// THE KEY IS REAL, not decoration. FindAction(Key) is what SetupInputComponent binds
	// from, so asking it back is asking whether the key in the prompt actually does anything.
	const FBuildAction* Build = FindAction(FName(TEXT("edit.build")));
	if (!TestNotNull(TEXT("a Build action"), Build)) { return false; }
	if (!TestTrue(TEXT("Build has a key bound"), Build->Key.IsValid())) { return false; }

	TestTrue(TEXT("the prompt names that key"),
		Prompt.Contains(Build->Key.GetDisplayName().ToString()));
	TestEqual(TEXT("and that key reaches the same action when pressed"),
		FindAction(Build->Key, Build->bRequiresCtrl), Build);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRoadBuildHUDLooksTest,
	"AirportMgr.HUD.LooksCoverEveryStyle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRoadBuildHUDLooksTest::RunTest(const FString& Parameters)
{
	// THE ONE-LIST CHECK for Looks (#104). A style Marker/Line/CrossMark/Label draws with no
	// matching entry used to fall through StyleColour's checkNoEntry() crash; now it is
	// LookFor's, and this is what catches a style left out of the constructor's seeding list
	// before a player does, at the first frame that draws it.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildHUD* Hud = World->SpawnActor<ARoadBuildHUD>();
	if (!TestNotNull(TEXT("the hud"), Hud)) { return false; }

	// EPreviewStyle is a plain 0-based enum ending at ServiceAnchor - iterated the same way
	// FBuildActionsRegistryTest walks EActionSection, rather than by reflection.
	for (uint8 S = 0; S <= static_cast<uint8>(EPreviewStyle::ServiceAnchor); ++S)
	{
		const EPreviewStyle Style = static_cast<EPreviewStyle>(S);
		const FPreviewLook& Look = Hud->LookForTest(Style);
		TestTrue(*FString::Printf(TEXT("style %d has a positive radius scale"), S), Look.RadiusScale > 0.0f);
		TestTrue(*FString::Printf(TEXT("style %d has a positive thickness scale"), S), Look.ThicknessScale > 0.0f);
	}
	return true;
}

#endif
