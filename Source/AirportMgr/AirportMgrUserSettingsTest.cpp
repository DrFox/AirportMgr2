#include "CoreMinimal.h"
#include "AirportMgrUserSettings.h"
#include "Misc/AutomationTest.h"
#include "UI/UiLayoutStore.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ENGINE MAKES OUR SETTINGS CLASS. GameUserSettingsClassName is a config line nothing else
 * checks: left out, the engine quietly makes a plain UGameUserSettings, Get() returns null, and
 * every remembered layout is written nowhere - with every host test still green, because they all
 * use a memory store on purpose.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportMgrUserSettingsClassTest, "AirportMgr.Settings.EngineMakesOurUserSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportMgrUserSettingsClassTest::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("GEngine's GameUserSettings is a UAirportMgrUserSettings"), UAirportMgrUserSettings::Get());
	return true;
}

/**
 * THE REAL STORE ROUND-TRIPS through the settings object - on a key of the test's own, removed
 * afterwards, so the player's windows are never touched (FConfigToolPreferences' precedent).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUserSettingsLayoutStoreTest, "AirportMgr.Settings.LayoutStoreRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUserSettingsLayoutStoreTest::RunTest(const FString& Parameters)
{
	if (UAirportMgrUserSettings::Get() == nullptr)
	{
		AddError(TEXT("no UAirportMgrUserSettings - see EngineMakesOurUserSettings"));
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
