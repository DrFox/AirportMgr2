#include "CoreMinimal.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/AutomationTest.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiDropdown.h"
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

	// THROUGH THE ENGINE'S OWN EVENT, not the handler: the seam a deleted AddDynamic would cut
	// while every line above stayed green (step 4a final review, Important 3).
	UButton* Hit = T->WidgetTree->FindWidget<UButton>(TEXT("ToggleHit"));
	if (!TestNotNull(TEXT("the toggle's hit button"), Hit)) { return false; }
	Hit->OnClicked.Broadcast();
	TestTrue(TEXT("a real click reaches the toggle"), T->IsOn());
	TestEqual(TEXT("and raises the event"), T->BroadcastCountForTest(), 2);
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

	// A DRAG WITHIN ONE STEP CHANGES NOTHING, so it raises nothing: USlider reports every mouse
	// move, and a live-applied UI scale re-laid the whole screen on each (final review, Important 2).
	Sl->HandleSliderMoved(1.01f);
	TestEqual(TEXT("a move that quantises to the same value raises nothing new"), Sl->BroadcastCountForTest(), 1);

	// Through the engine's events, not the handlers (Important 3).
	USlider* Bar = Sl->WidgetTree->FindWidget<USlider>(TEXT("SliderBar"));
	if (!TestNotNull(TEXT("the slider's bar"), Bar)) { return false; }
	Bar->OnValueChanged.Broadcast(1.22f);
	TestTrue(TEXT("a real drag reaches the slider"), FMath::IsNearlyEqual(Sl->GetValue(), 1.2f, 1e-4f));
	TestEqual(TEXT("and raises the event"), Sl->BroadcastCountForTest(), 2);

	// THE RELEASE, for a setting too heavy to apply on every step (UI scale re-lays the screen
	// under the captured mouse and can oscillate): raised once, with the value let go on.
	TestEqual(TEXT("no commit before the release"), Sl->CommitCountForTest(), 0);
	Bar->OnMouseCaptureEnd.Broadcast();
	TestEqual(TEXT("letting go commits once"), Sl->CommitCountForTest(), 1);
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

	// A real click on the SECOND segment picks index 1 - the seam, and the index each entry carries.
	UButton* Second = G->WidgetTree->FindWidget<UButton>(TEXT("Segment1"));
	if (!TestNotNull(TEXT("the second segment"), Second)) { return false; }
	Second->OnClicked.Broadcast();
	TestEqual(TEXT("a real click on segment 1 selects 1"), G->GetSelected(), 1);
	TestEqual(TEXT("and raises the event"), G->BroadcastCountForTest(), 2);
	return true;
}

/**
 * THE DROPDOWN'S BUTTON READS WHAT WAS CHOSEN (Review Focus 5), an index past the end lands on the
 * last (Focus 4), and its popup lists every option. The popup itself is Slate's to place and open;
 * a headless test builds its content through the same function the anchor calls.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiDropdownTest, "AirportMgr.UI.Controls.Dropdown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiDropdownTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	const UUIStyle& S = *GetDefault<UUIStyle>();
	UUiDropdown* D = CreateWidget<UUiDropdown>(TestWorld.World, UUiDropdown::StaticClass());
	if (!TestNotNull(TEXT("a dropdown"), D)) { return false; }
	const TArray<FText> Levels = { FText::FromString(TEXT("Low")), FText::FromString(TEXT("Medium")),
		FText::FromString(TEXT("High")), FText::FromString(TEXT("Epic")) };
	D->Build(S, Levels);
	D->SetSelected(2);
	TestTrue(TEXT("its button reads the choice"), D->LabelForTest().StartsWith(TEXT("High")));
	D->SetSelected(9);
	TestEqual(TEXT("past the end lands on the last"), D->GetSelected(), 3);
	TestEqual(TEXT("code choosing raises nothing"), D->BroadcastCountForTest(), 0);
	UUiDropdownList* List = Cast<UUiDropdownList>(D->BuildMenu());
	if (!TestNotNull(TEXT("the popup builds"), List)) { return false; }
	TestEqual(TEXT("with every option"), List->OptionCountForTest(), 4);

	// A real click on the popup's second option picks index 1 (Important 3).
	UButton* Medium = List->WidgetTree->FindWidget<UButton>(TEXT("Option1"));
	if (!TestNotNull(TEXT("the popup's second option"), Medium)) { return false; }
	Medium->OnClicked.Broadcast();
	TestEqual(TEXT("a real click on option 1 selects 1"), D->GetSelected(), 1);
	TestEqual(TEXT("and raises the event"), D->BroadcastCountForTest(), 1);

	// Opening needs a Slate anchor a headless test does not have, so the seam is checked as a
	// binding: the button's click must be wired to the function that opens the list.
	UButton* Open = D->WidgetTree->FindWidget<UButton>(TEXT("DropdownButton"));
	if (!TestNotNull(TEXT("the dropdown's button"), Open)) { return false; }
	TestTrue(TEXT("its click opens the list"),
		Open->OnClicked.Contains(D, GET_FUNCTION_NAME_CHECKED(UUiDropdown, HandleOpenClicked)));
	D->Choose(0);
	TestTrue(TEXT("a choice from the list reads on the button"), D->LabelForTest().StartsWith(TEXT("Low")));
	TestEqual(TEXT("and raises the event once more"), D->BroadcastCountForTest(), 2);
	return true;
}

/**
 * THE ARROW IS A CHARACTER INTER CAN DRAW. The composite has no fallback typeface, so a glyph
 * missing from Inter is drawn by the engine's last-resort font or as a box in a shipped build.
 * U+25BE was the first choice and Inter lacks it (step 4a final review, Important 1, measured
 * with fontTools); asked of Slate's own font cache, which is what draws it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiDropdownArrowTest, "AirportMgr.UI.Controls.DropdownArrowIsInInter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiDropdownArrowTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Slate is up - the font cache is what answers"), FSlateApplication::IsInitialized())) { return false; }
	const UUIStyle* S = UAirportMgrUISettings::ResolveStyle();
	TestTrue(TEXT("the style's faces can draw the dropdown arrow"), S->CanDraw(UUiDropdown::ArrowCodepoint));
	TestTrue(TEXT("control: they can draw a plain letter"), S->CanDraw(TEXT('A')));
	return true;
}

#endif
