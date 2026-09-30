#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AirportMgrUserSettings.h"
#include "BuildActions.h"
#include "BuildHudLayer.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Slider.h"
#include "Scalability.h"
#include "SettingsPanelWidget.h"
#include "UI/UiSlider.h"
#include "UI/UiWindowHost.h"
#include "PlayerSettings.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/SnapGuideSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * GRID SNAP ON START TURNS A GRID ON, never a level's own grid off (plan 4b ruling): on, an airport
 * with no grid starts on the first step the bar's Grid button gives; off, whatever the level set
 * stays - the toggle is the player's preference for a fresh start, not an override of a design.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerSettingsStartGridTest, "AirportMgr.Settings.StartGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerSettingsStartGridTest::RunTest(const FString& Parameters)
{
	FSnapGuideSettings FirstStep;
	FirstStep.CycleGridStep();

	FSnapGuideSettings Guides;
	PlayerSettings::ApplyStartGrid(Guides, true);
	TestEqual(TEXT("on: no grid becomes the Grid button's first step"), Guides.GridStep, FirstStep.GridStep);

	FSnapGuideSettings Plain;
	PlayerSettings::ApplyStartGrid(Plain, false);
	TestEqual(TEXT("off: no grid stays no grid"), Plain.GridStep, EGridStep::Off);

	FSnapGuideSettings Designed;
	Designed.GridStep = EGridStep::TenMetres;
	PlayerSettings::ApplyStartGrid(Designed, false);
	TestEqual(TEXT("off: a level's own 10 m grid is left alone"), Designed.GridStep, EGridStep::TenMetres);
	PlayerSettings::ApplyStartGrid(Designed, true);
	TestEqual(TEXT("on: and so is it here - it is already on"), Designed.GridStep, EGridStep::TenMetres);
	return true;
}

/**
 * THE DRIVE SIDE IS ONE VALUE (spec section 3): Settings' "Traffic drives on" and the bar's "Drive
 * left" read and write the same thing - the road actor's, with its undo step - so neither can show
 * a side the airport is not driving on. Through the game sink, the way Settings reaches it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlayerSettingsDriveSideTest, "AirportMgr.Settings.DriveSideIsOneValue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FPlayerSettingsDriveSideTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("a road actor"), TestWorld.Actor)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	// LEFT, not the default: FPlayerSettings defaults to Right, so a sink that read nothing would
	// pass a Right start.
	// A NODE FIRST: the actor makes its network on the first edit, and a side is the network's.
	TestWorld.Actor->PlaceNode(FVector2D::ZeroVector);
	TestTrue(TEXT("the airport now drives on the left"), TestWorld.Actor->SetDriveSide(EDriveSide::Left));

	FGamePlayerSettingsSink Sink(*C);
	const FBuildAction* Bar = FindAction(FName(TEXT("game.driveside")));
	if (!TestNotNull(TEXT("the bar's drive-side action"), Bar)) { return false; }
	FBuildActionContext Ctx(*C);

	FPlayerSettings V = Sink.Read();
	TestEqual(TEXT("Settings reads the airport's side"), V.DriveSide, EDriveSide::Left);
	TestTrue(TEXT("which the bar's button lights for"), Bar->IsActive(Ctx));
	V.DriveSide = EDriveSide::Right;
	Sink.Apply(V);
	TestEqual(TEXT("Settings sets the airport's side"), TestWorld.Actor->GetDriveSide(), EDriveSide::Right);
	TestFalse(TEXT("and the bar's button goes out"), Bar->IsActive(Ctx));

	Bar->Execute(Ctx);
	TestEqual(TEXT("the bar's button flips it, and Settings reads that"), Sink.Read().DriveSide, EDriveSide::Left);
	return true;
}

/**
 * CANCEL PUTS A CUSTOM GRAPHICS MIX BACK, group by group (4b final review, Critical 1). The engine
 * reports a mix as -1, which no preset can restore - picking Epic overwrote every group, and a
 * Cancel that only re-applied "-1" left Epic in force and then saved it. Through the GAME sink:
 * the memory sink stores -1 verbatim and could not fail this way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameSinkCustomGraphicsTest, "AirportMgr.Settings.GameSinkRevertRestoresACustomMix",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGameSinkCustomGraphicsTest::RunTest(const FString& Parameters)
{
	UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get();
	if (!TestNotNull(TEXT("the engine made our settings object"), Settings)) { return false; }
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	const Scalability::FQualityLevels Original = Settings->ScalabilityQuality;
	Scalability::FQualityLevels Mix = Original;
	Mix.SetFromSingleQualityLevel(2);
	Mix.ShadowQuality = 0;   // one group off the preset: a custom mix
	Settings->ScalabilityQuality = Mix;
	TestEqual(TEXT("control: the engine reads the mix as custom"), Settings->GetOverallScalabilityLevel(), -1);

	FGamePlayerSettingsSink Sink(*C);
	Sink.BeginEdit();
	const FPlayerSettings Snapshot = Sink.Read();
	FPlayerSettings Epic = Snapshot;
	Epic.GraphicsQuality = 3;
	Sink.Apply(Epic);
	TestEqual(TEXT("control: picking Epic applied Epic"), Settings->GetOverallScalabilityLevel(), 3);
	Sink.Revert(Snapshot);
	TestTrue(TEXT("cancel restores every group of the mix"), Settings->ScalabilityQuality == Mix);

	Settings->ScalabilityQuality = Original;
	Settings->ApplyNonResolutionSettings();
	return true;
}

/**
 * A CANCELLED DRIVE SIDE LEAVES NO UNDO STEP (4b final review, Important 3). Each side the player
 * tried was an undo step; a Cancel that set the side FORWARDS pushed one more, and Ctrl+Z then
 * re-laned the airport to the side the player had cancelled. Revert undoes what the dialog did.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameSinkDriveSideUndoTest, "AirportMgr.Settings.GameSinkRevertLeavesNoUndoStep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FGameSinkDriveSideUndoTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a road actor"), TestWorld.Actor)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	C->SetTargetForTest(TestWorld.Actor);
	TestWorld.Actor->PlaceNode(FVector2D::ZeroVector);   // the one undo step before the dialog
	TestEqual(TEXT("control: the airport drives on the right"), TestWorld.Actor->GetDriveSide(), EDriveSide::Right);

	FGamePlayerSettingsSink Sink(*C);
	Sink.BeginEdit();
	const FPlayerSettings Snapshot = Sink.Read();
	FPlayerSettings Left = Snapshot;
	Left.DriveSide = EDriveSide::Left;
	Sink.Apply(Left);
	TestEqual(TEXT("control: the dialog re-laned to the left"), TestWorld.Actor->GetDriveSide(), EDriveSide::Left);
	Sink.Revert(Snapshot);
	TestEqual(TEXT("cancel puts the right back"), TestWorld.Actor->GetDriveSide(), EDriveSide::Right);
	TestTrue(TEXT("the node's step is still there to undo"), TestWorld.Actor->Undo());
	TestEqual(TEXT("and undoing it does not bring the cancelled side back"), TestWorld.Actor->GetDriveSide(), EDriveSide::Right);
	return true;
}

/**
 * STOPPING PLAY WITH SETTINGS OPEN IS A CANCEL (4b final review, Important 4). The settings object
 * lives as long as the engine, so live values left in it were read as "saved" by the next session
 * and written to disk by its next window drag. EndPlay cancels an open dialog.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsEndPlayTest, "AirportMgr.Settings.EndPlayCancelsAnOpenDialog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSettingsEndPlayTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }
	UBuildHudLayer* Hud = C->GetHud();
	Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	Hud->SettingsPanel = CreateWidget<USettingsPanelWidget>(TestWorld.World, USettingsPanelWidget::StaticClass());
	if (!TestTrue(TEXT("a host and a Settings panel"), Hud->WindowHost != nullptr && Hud->SettingsPanel != nullptr)) { return false; }
	Hud->WindowHost->AddWindow(*Hud->SettingsPanel);
	TSharedPtr<FMemoryPlayerSettingsSink> Sink = MakeShared<FMemoryPlayerSettingsSink>();
	Hud->SettingsPanel->SetSink(Sink);
	const FPlayerSettings Before = Sink->Values;

	C->ToggleSettings();
	UUiSlider* Pan = Hud->SettingsPanel->WidgetTree->FindWidget<UUiSlider>(TEXT("PanSpeed"));
	USlider* Bar = Pan != nullptr ? Pan->WidgetTree->FindWidget<USlider>(TEXT("SliderBar")) : nullptr;
	if (!TestNotNull(TEXT("the pan slider"), Bar)) { return false; }
	Bar->OnValueChanged.Broadcast(1.8f);
	TestFalse(TEXT("control: the drag is in force"), Sink->Values == Before);
	C->EndPlayForTest();
	TestTrue(TEXT("stopping play put every value back"), Sink->Values == Before);
	TestFalse(TEXT("and closed the dialog"), C->IsSettingsShowing());
	return true;
}

#endif
