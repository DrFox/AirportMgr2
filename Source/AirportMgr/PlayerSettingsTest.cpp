#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "PlayerSettings.h"
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

#endif
