#include "CoreMinimal.h"
#include "AirportMgrUserSettings.h"
#include "Misc/AutomationTest.h"
#include "UI/UiLayoutStore.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REAL STORE ROUND-TRIPS through the settings object - on a key of the test's own, removed
 * afterwards, so the player's windows are never touched (FConfigToolPreferences' precedent).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUserSettingsLayoutStoreTest, "AirportMgr.Settings.LayoutStoreRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUserSettingsLayoutStoreTest::RunTest(const FString& Parameters)
{
	// THE ENGINE MAKES OUR SETTINGS CLASS - this is that guard (it was a test of its own, AirportMgr.Settings.
	// EngineMakesOurUserSettings, whose one assertion this is; GameSinkRevertRestoresACustomMix guards it too).
	// GameUserSettingsClassName is a config line nothing else checks: left out, the engine quietly makes a plain
	// UGameUserSettings, Get() returns null, and every remembered layout is written nowhere - with every host test
	// still green, because they all use a memory store on purpose.
	// ENFORCED BY: this check, and Config/DefaultEngine.ini's GameUserSettingsClassName line it guards.
	if (UAirportMgrUserSettings::Get() == nullptr)
	{
		AddError(TEXT("GEngine's GameUserSettings is not a UAirportMgrUserSettings - GameUserSettingsClassName in DefaultEngine.ini names the class"));
		return false;
	}
	const FName Key(TEXT("test.layout.roundtrip"));
	FUserSettingsLayoutStore Store;
	FUiWindowPlacement P;
	P.TopLeft = FVector2D(321.0, 123.0);
	P.Size = FVector2D(250.0, 150.0);
	P.bSized = true;
	Store.Write(Key, P);
	const TOptional<FUiWindowPlacement> Back = Store.Read(Key);
	TestTrue(TEXT("read back"), Back.IsSet());
	if (Back.IsSet())
	{
		TestEqual(TEXT("position"), Back->TopLeft, P.TopLeft);
		TestEqual(TEXT("size"), Back->Size, P.Size);
		TestTrue(TEXT("sized"), Back->bSized);
	}
	Store.Remove(Key);
	TestFalse(TEXT("removed again - the player's file keeps nothing of this test"), Store.Read(Key).IsSet());
	return true;
}

#endif
