#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiRadioGroup.h"
#include "UI/UiSlider.h"
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

/**
 * A SLIDER LANDS ON ITS STEP AND STAYS IN RANGE - a drag to 1.37 on a 0.05 step reads 1.35, a
 * saved 3.0 on a 0.75..1.5 range reads 1.5, and the readout shows what the value IS, not what
 * the mouse was near (Review Focus 2, 3). Code setting it raises nothing (Focus 1).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSliderTest, "AirportMgr.UI.Controls.Slider",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiSliderTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiSlider* Sl = CreateWidget<UUiSlider>(TestWorld.World, UUiSlider::StaticClass());
	if (!TestNotNull(TEXT("a slider"), Sl)) { return false; }
	Sl->Build(S, 0.75f, 1.5f, 0.05f, 2, FText::FromString(TEXT("x")));
	Sl->SetValue(1.37f);
	TestTrue(TEXT("quantised to the step"), FMath::IsNearlyEqual(Sl->GetValue(), 1.35f, 1e-4f));
	TestEqual(TEXT("the readout shows the quantised value"), Sl->ReadoutForTest(), FString(TEXT("1.35x")));
	Sl->SetValue(3.0f);
	TestTrue(TEXT("clamped to the maximum"), FMath::IsNearlyEqual(Sl->GetValue(), 1.5f, 1e-4f));
	Sl->SetValue(0.7499999f);
	TestTrue(TEXT("a float that is nearly on a step lands on it"), FMath::IsNearlyEqual(Sl->GetValue(), 0.75f, 1e-4f));
	TestEqual(TEXT("code setting it raises nothing"), Sl->BroadcastCountForTest(), 0);
	Sl->HandleSliderMoved(1.02f);
	TestTrue(TEXT("the player's drag quantises too"), FMath::IsNearlyEqual(Sl->GetValue(), 1.0f, 1e-4f));
	TestEqual(TEXT("and raises the event once"), Sl->BroadcastCountForTest(), 1);
	return true;
}

/**
 * EXACTLY ONE SEGMENT IS LIT, whatever index arrives - a saved 7 of 2 lands on the last, not on
 * nothing (Review Focus 4). Code choosing raises nothing; the player's click raises once.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiRadioGroupTest, "AirportMgr.UI.Controls.Segmented",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiRadioGroupTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiRadioGroup* G = CreateWidget<UUiRadioGroup>(TestWorld.World, UUiRadioGroup::StaticClass());
	if (!TestNotNull(TEXT("a group"), G)) { return false; }
	G->Build(S, { FText::FromString(TEXT("Left")), FText::FromString(TEXT("Right")) });
	TestEqual(TEXT("one lit from the start"), G->SelectedButtonCountForTest(), 1);
	G->SetSelected(7);
	TestEqual(TEXT("an out-of-range index lands on the last"), G->GetSelected(), 1);
	TestEqual(TEXT("still exactly one lit"), G->SelectedButtonCountForTest(), 1);
	TestEqual(TEXT("code choosing raises nothing"), G->BroadcastCountForTest(), 0);
	G->Choose(0);
	TestEqual(TEXT("the player's click selects"), G->GetSelected(), 0);
	TestEqual(TEXT("and raises the event once"), G->BroadcastCountForTest(), 1);
	G->Choose(0);
	TestEqual(TEXT("clicking the lit one again raises nothing - nothing changed"), G->BroadcastCountForTest(), 1);
	return true;
}

#endif
