#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiToggle.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A TOGGLE SAYS ITS STATE IN COLOUR (Accent on, Rule off) and raises its event only for the
 * PLAYER's click - a value set from code (the Settings dialog loading saved settings on open)
 * must not write itself straight back. Review Focus 1 of the step 4a plan.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiToggleTest, "AirportMgr.UI.Controls.Toggle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiToggleTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiToggle* T = CreateWidget<UUiToggle>(TestWorld.World, UUiToggle::StaticClass());
	if (!TestNotNull(TEXT("a toggle"), T)) { return false; }
	T->Build(S);
	TestFalse(TEXT("starts off"), T->IsOn());
	TestEqual(TEXT("off reads as Rule"), T->TrackColourForTest(), S.Rule);
	T->SetOn(true);
	TestTrue(TEXT("code can turn it on"), T->IsOn());
	TestEqual(TEXT("on reads as Accent"), T->TrackColourForTest(), S.Accent);
	TestEqual(TEXT("and code setting it raises nothing"), T->BroadcastCountForTest(), 0);
	T->HandleClicked();
	TestFalse(TEXT("the player's click flips it"), T->IsOn());
	TestEqual(TEXT("and raises the event once"), T->BroadcastCountForTest(), 1);
	return true;
}

#endif
