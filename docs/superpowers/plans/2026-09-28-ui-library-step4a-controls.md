# UI Library Step 4a - The Four Controls: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `UUiToggle`, `UUiSlider`, `UUiRadioGroup` (segmented) and `UUiDropdown` exist, look like the library, and behave: values clamp and quantise, exactly one segment is selected, the dropdown's label follows its choice, and each raises its change event only when the PLAYER changes it.

**Architecture:** Each control is a small `UUserWidget` built in C++ by `Build(const UUIStyle&, ...)` (the `UUiWindow` pattern - a composite needs its own `WidgetTree`), constructed by a panel through `WidgetTree->ConstructWidget` or by a test through `CreateWidget`. Colours come from `UUIStyle` slots by meaning. Change events are dynamic multicast delegates (spec section 1: code and a later Blueprint both bind). A value set from CODE (`SetValue(v)` with no broadcast) does not raise the event - the Settings dialog in step 4b sets every control from the saved settings on open, and an event there would write the values straight back.

**Tech Stack:** UE 5.8 C++ (UMG: `UButton`, `USlider`, `UMenuAnchor`).

**Spec:** `docs/superpowers/specs/2026-09-28-ui-widget-library-design.md` section 1 (the widget table). Delivery step 4, first half; the consumers (the Settings dialog, player settings fields) are plan 4b.

## Global Constraints

- Worktree `C:\repos\airportmgr2-ui-widget-library`, branch `feature/ui-widget-library`. No push, no PR (user).
- Build `-NoHotReloadFromIDE`; close this worktree's editor by PID first. New test .cpp: two builds. Tests: `-Filter AirportMgr`; read the `N test(s) run` line.
- Edits via Write/Edit or Python scripts written with Write.
- Colours by slot (spec section 4): toggle track off `Rule`, on `Accent`, knob `Surface`; slider bar `Rule`, handle `Accent`; segmented and dropdown are `UUiButton`s (their colour rule is `UUiButton::LookFor`). No `SetBackgroundColor(` or `FButtonStyle` outside `UI/` (rule 28 - these all live in `UI/`).
- A phase is an enum; comments explain WHY.

## Review Focus

1. **Code sets a value and the event fires anyway** (the dialog opening would write the settings back, or loop). Expect no broadcast from `SetValue`/`SetOn`/`SetSelected` unless asked. Each task's test pins it.
2. **A slider value off its step** (dragged to 1.37 on a 0.05 step, or a saved 0.7499999). Expect quantised to 1.35 / 0.75, and the readout to show the quantised value. Task 2.
3. **A value outside the range** (an old save with UI scale 3.0). Expect clamped to the max. Task 2.
4. **A segmented/dropdown index out of range** (a saved quality level 7 of 4). Expect clamped to the last, not a crash or no selection. Tasks 3 and 4.
5. **The dropdown chooses and its label does not follow.** Expect the button to read the chosen option. Task 4.

---

### Task 1: UUiToggle

**Files:** Create `Source/AirportMgr/UI/UiToggle.h`, `UI/UiToggle.cpp`, `UI/UiControlsTest.cpp`

**Interfaces - produces:**

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiToggleChanged, bool, bOn);
class UUiToggle : public UUserWidget {
	void Build(const UUIStyle& Style);
	void SetOn(bool bInOn, bool bBroadcast = false);
	bool IsOn() const;
	UPROPERTY(BlueprintAssignable) FUiToggleChanged OnToggled;
	UFUNCTION() void HandleClicked();   // the player's click: flips and broadcasts
	FLinearColor TrackColourForTest() const; int32 BroadcastCountForTest() const;
};
```

- [ ] **Step 1: Write the failing test** `UI/UiControlsTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiToggle.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A TOGGLE SAYS ITS STATE IN COLOUR (Accent on, Rule off) and raises its event only for the
 * PLAYER's click - a value set from code (the Settings dialog loading saved settings on open)
 * must not write itself straight back. Review Focus 1.
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
```

- [ ] **Step 2: Build twice; expect compile failure** (`UI/UiToggle.h`).

- [ ] **Step 3: Write `UI/UiToggle.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiToggle.generated.h"

class UBorder;
class UButton;
class UOverlaySlot;
class UUIStyle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiToggleChanged, bool, bOn);

/**
 * An on/off switch (UI library step 4a): a pill track, Accent when on and Rule when off, with a
 * Surface knob that sits at the end it is switched to. A UButton inside, so hover and press come
 * from the engine; the button draws nothing itself (the track IS the look).
 */
UCLASS()
class AIRPORTMGR_API UUiToggle : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style);

	/** From CODE: no event unless bBroadcast - see the plan's Review Focus 1. */
	void SetOn(bool bInOn, bool bBroadcast = false);
	bool IsOn() const { return bOn; }

	/** Raised when the PLAYER flips it (or code asks, with bBroadcast). */
	UPROPERTY(BlueprintAssignable) FUiToggleChanged OnToggled;

	/** The player's click. Public for the test that clicks it. */
	UFUNCTION() void HandleClicked();

	FLinearColor TrackColourForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	void Paint();

	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UBorder> Track;
	UPROPERTY() TObjectPtr<UOverlaySlot> KnobSlot;
	bool bOn = false;
	int32 Broadcasts = 0;
};
```

- [ ] **Step 4: Write `UI/UiToggle.cpp`:**

```cpp
#include "UI/UiToggle.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "UIStyle.h"

namespace
{
	// The switch's own proportions: a 36x20 pill, a 16 px knob inset 2 px - an iOS-scale switch,
	// small enough to sit at the end of a settings row without dominating it.
	constexpr float TrackWidth = 36.0f;
	constexpr float TrackHeight = 20.0f;
	constexpr float KnobSize = 16.0f;
	constexpr float KnobInset = 2.0f;
}

void UUiToggle::Build(const UUIStyle& InStyle)
{
	Style = &InStyle;

	UButton* Hit = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ToggleHit"));
	FButtonStyle NoLook = Hit->GetStyle();
	NoLook.SetNormal(FSlateNoResource()); NoLook.SetHovered(FSlateNoResource());
	NoLook.SetPressed(FSlateNoResource()); NoLook.SetDisabled(FSlateNoResource());
	NoLook.SetNormalPadding(FMargin(0.0f)); NoLook.SetPressedPadding(FMargin(0.0f));
	Hit->SetStyle(NoLook);
	Hit->OnClicked.AddDynamic(this, &UUiToggle::HandleClicked);
	WidgetTree->RootWidget = Hit;

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(TrackWidth);
	Size->SetHeightOverride(TrackHeight);
	Hit->SetContent(Size);

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	Size->SetContent(Layers);

	Track = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ToggleTrack"));
	UOverlaySlot* TrackSlot = Layers->AddChildToOverlay(Track);
	TrackSlot->SetHorizontalAlignment(HAlign_Fill);
	TrackSlot->SetVerticalAlignment(VAlign_Fill);

	USizeBox* KnobBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	KnobBox->SetWidthOverride(KnobSize);
	KnobBox->SetHeightOverride(KnobSize);
	UBorder* Knob = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("ToggleKnob"));
	Knob->SetBrush(FSlateRoundedBoxBrush(InStyle.Surface, KnobSize * 0.5f));
	KnobBox->SetContent(Knob);
	KnobSlot = Layers->AddChildToOverlay(KnobBox);
	KnobSlot->SetVerticalAlignment(VAlign_Center);
	KnobSlot->SetPadding(FMargin(KnobInset));
	Paint();
}

void UUiToggle::SetOn(bool bInOn, bool bBroadcast)
{
	bOn = bInOn;
	Paint();
	if (bBroadcast)
	{
		++Broadcasts;
		OnToggled.Broadcast(bOn);
	}
}

void UUiToggle::HandleClicked()
{
	SetOn(!bOn, /*bBroadcast=*/true);
}

void UUiToggle::Paint()
{
	if (Style == nullptr || Track == nullptr || KnobSlot == nullptr)
	{
		return;
	}
	Track->SetBrush(FSlateRoundedBoxBrush(bOn ? Style->Accent : Style->Rule, TrackHeight * 0.5f));
	KnobSlot->SetHorizontalAlignment(bOn ? HAlign_Right : HAlign_Left);
}

FLinearColor UUiToggle::TrackColourForTest() const
{
	return Track != nullptr ? Track->Background.TintColor.GetSpecifiedColor() : FLinearColor::Transparent;
}
```

Note: `FButtonStyle` here is inside `UI/`, so rule 28 allows it; the toggle's button is a hit area, not a button look.

- [ ] **Step 5: Build; run `-Filter AirportMgr.UI.Controls`.** Expected PASS.
- [ ] **Step 6: Commit** `git add Source/AirportMgr/UI && git commit -m "feat(ui): UUiToggle"`

---

### Task 2: UUiSlider

**Files:** Create `UI/UiSlider.h`, `UI/UiSlider.cpp`; Modify `UI/UiControlsTest.cpp`

**Interfaces - produces:**

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiSliderChanged, float, Value);
class UUiSlider : public UUserWidget {
	void Build(const UUIStyle& Style, float Min, float Max, float Step, int32 Decimals, const FText& Suffix);
	void SetValue(float V, bool bBroadcast = false);   // clamped and quantised
	float GetValue() const;
	FString ReadoutForTest() const; int32 BroadcastCountForTest() const;
	UPROPERTY(BlueprintAssignable) FUiSliderChanged OnValueChanged;
	UFUNCTION() void HandleSliderMoved(float Raw);   // USlider's own event: the player's drag
};
```

- [ ] **Step 1: Write the failing test** - append to `UiControlsTest.cpp` (include `UI/UiSlider.h`):

```cpp
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
```

- [ ] **Step 2: Build; expect compile failure.**

- [ ] **Step 3: Write `UI/UiSlider.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiSlider.generated.h"

class USlider;
class UTextBlock;
class UUIStyle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiSliderChanged, float, Value);

/**
 * A value slider with its readout (UI library step 4a): a Rule-coloured bar, an Accent handle, and
 * the value to Decimals places with a suffix ("1.25x"). Values are CLAMPED to Min..Max and
 * QUANTISED to Step on every path in - a player's drag, a saved setting, code - so what the readout
 * shows is always a value the setting can actually hold.
 */
UCLASS()
class AIRPORTMGR_API UUiSlider : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, float InMin, float InMax, float InStep, int32 InDecimals, const FText& InSuffix);

	/** From CODE: no event unless bBroadcast. */
	void SetValue(float V, bool bBroadcast = false);
	float GetValue() const { return Value; }

	UPROPERTY(BlueprintAssignable) FUiSliderChanged OnValueChanged;

	/** USlider's own event, which fires only for the player's drag (USlider::SetValue does not). */
	UFUNCTION() void HandleSliderMoved(float Raw);

	FString ReadoutForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	float Quantise(float V) const;

	UPROPERTY() TObjectPtr<USlider> Slider;
	UPROPERTY() TObjectPtr<UTextBlock> Readout;
	float Min = 0.0f;
	float Max = 1.0f;
	float Step = 0.0f;
	int32 Decimals = 2;
	FText Suffix;
	float Value = 0.0f;
	int32 Broadcasts = 0;
};
```

- [ ] **Step 4: Write `UI/UiSlider.cpp`:**

```cpp
#include "UI/UiSlider.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "UIStyle.h"

void UUiSlider::Build(const UUIStyle& Style, float InMin, float InMax, float InStep, int32 InDecimals, const FText& InSuffix)
{
	Min = InMin;
	Max = FMath::Max(InMin, InMax);
	Step = FMath::Max(0.0f, InStep);
	Decimals = InDecimals;
	Suffix = InSuffix;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	WidgetTree->RootWidget = Row;

	// A fixed width for the bar, so every slider in a settings list lines up under the others.
	USizeBox* BarBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	BarBox->SetWidthOverride(140.0f);
	Slider = WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(), TEXT("SliderBar"));
	FSliderStyle Look = Slider->GetWidgetStyle();
	const FSlateRoundedBoxBrush Bar(Style.Rule, 2.0f);
	const FSlateRoundedBoxBrush Handle(Style.Accent, 7.0f);
	Look.SetNormalBarImage(Bar).SetHoveredBarImage(Bar).SetDisabledBarImage(Bar);
	FSlateBrush HandleBrush = Handle;
	HandleBrush.ImageSize = FVector2D(14.0, 14.0);
	Look.SetNormalThumbImage(HandleBrush).SetHoveredThumbImage(HandleBrush).SetDisabledThumbImage(HandleBrush);
	Look.SetBarThickness(4.0f);
	Slider->SetWidgetStyle(Look);
	Slider->SetMinValue(Min);
	Slider->SetMaxValue(Max);
	Slider->SetStepSize(Step);
	Slider->OnValueChanged.AddDynamic(this, &UUiSlider::HandleSliderMoved);
	BarBox->SetContent(Slider);
	Row->AddChildToHorizontalBox(BarBox)->SetVerticalAlignment(VAlign_Center);

	Readout = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("SliderReadout"));
	Style.ApplyText(*Readout, EUITextRole::Label, Style.Ink);
	Readout->SetMinDesiredWidth(44.0f);
	Readout->SetJustification(ETextJustify::Right);
	UHorizontalBoxSlot* ReadSlot = Row->AddChildToHorizontalBox(Readout);
	ReadSlot->SetVerticalAlignment(VAlign_Center);
	ReadSlot->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	SetValue(Min);
}

float UUiSlider::Quantise(float V) const
{
	const float Clamped = FMath::Clamp(V, Min, Max);
	if (Step <= 0.0f)
	{
		return Clamped;
	}
	// ROUNDED, not truncated: 0.7499999 is a saved 0.75, not a step below it.
	return FMath::Clamp(Min + FMath::RoundToFloat((Clamped - Min) / Step) * Step, Min, Max);
}

void UUiSlider::SetValue(float V, bool bBroadcast)
{
	Value = Quantise(V);
	if (Slider != nullptr && !FMath::IsNearlyEqual(Slider->GetValue(), Value))
	{
		Slider->SetValue(Value);   // does not raise USlider's OnValueChanged - no loop
	}
	if (Readout != nullptr)
	{
		FNumberFormattingOptions Fmt;
		Fmt.MinimumFractionalDigits = Decimals;
		Fmt.MaximumFractionalDigits = Decimals;
		Readout->SetText(FText::Format(INVTEXT("{0}{1}"), FText::AsNumber(Value, &Fmt), Suffix));
	}
	if (bBroadcast)
	{
		++Broadcasts;
		OnValueChanged.Broadcast(Value);
	}
}

void UUiSlider::HandleSliderMoved(float Raw)
{
	SetValue(Raw, /*bBroadcast=*/true);
}

FString UUiSlider::ReadoutForTest() const
{
	return Readout != nullptr ? Readout->GetText().ToString() : FString();
}
```

- [ ] **Step 5: Build; run `-Filter AirportMgr.UI.Controls`.** Expected 2 pass. `FText::AsNumber` is culture-formatted; the test machine's culture uses "." - if it does not, assert on `GetValue()` only and ledger it.
- [ ] **Step 6: Commit** `git commit -am "feat(ui): UUiSlider - clamped, quantised, with its readout"` (add the new files).

---

### Task 3: UUiRadioGroup (segmented)

**Files:** Create `UI/UiRadioGroup.h`, `UI/UiRadioGroup.cpp`; Modify `UI/UiControlsTest.cpp`

**Interfaces - produces:**

```cpp
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiChoiceChanged, int32, Index);
class UUiRadioEntry : public UObject { int32 Index; TWeakObjectPtr<UUiRadioGroup> Owner; UFUNCTION() void HandleClicked(); };
class UUiRadioGroup : public UUserWidget {
	void Build(const UUIStyle& Style, const TArray<FText>& Options);
	void SetSelected(int32 Index, bool bBroadcast = false);   // clamped into range
	int32 GetSelected() const;
	void Choose(int32 Index);   // the player's click: select + broadcast
	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnSelectionChanged;
	int32 SelectedButtonCountForTest() const; int32 BroadcastCountForTest() const;
};
```

`FUiChoiceChanged` is declared in `UiRadioGroup.h` and reused by the dropdown (Task 4 includes this header).

- [ ] **Step 1: Write the failing test** - append (include `UI/UiRadioGroup.h`):

```cpp
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
```

- [ ] **Step 2: Build; expect compile failure.**

- [ ] **Step 3: Write `UI/UiRadioGroup.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiRadioGroup.generated.h"

class UUiButton;
class UUiRadioGroup;
class UUIStyle;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUiChoiceChanged, int32, Index);

/** One segment's click, as an object: UButton::OnClicked binds only to a UFUNCTION on a UObject
 *  (the UBuildBarEntry reason). Holds the INDEX, never a label to look up. */
UCLASS()
class AIRPORTMGR_API UUiRadioEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UUiRadioGroup> Owner;
	UFUNCTION() void HandleClicked();
};

/**
 * One of N as a segmented row of UUiButtons (UI library step 4a) - flatter than radio circles, and
 * the lit segment is the bar's own "selected" look (UUiButton::LookFor: Accent), so a choice reads
 * the same here as an armed tool does on the bar.
 */
UCLASS()
class AIRPORTMGR_API UUiRadioGroup : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FText>& Options);

	/** From CODE: clamped into range; no event unless bBroadcast. */
	void SetSelected(int32 Index, bool bBroadcast = false);
	int32 GetSelected() const { return Selected; }

	/** The player's click: selects and raises the event - unless it is already the one lit. */
	void Choose(int32 Index);

	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnSelectionChanged;

	int32 SelectedButtonCountForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	void Paint();

	UPROPERTY() TArray<TObjectPtr<UUiButton>> Buttons;
	UPROPERTY() TArray<TObjectPtr<UUiRadioEntry>> Entries;
	int32 Selected = 0;
	int32 Broadcasts = 0;
};
```

- [ ] **Step 4: Write `UI/UiRadioGroup.cpp`:**

```cpp
#include "UI/UiRadioGroup.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

void UUiRadioEntry::HandleClicked()
{
	if (UUiRadioGroup* Group = Owner.Get())
	{
		Group->Choose(Index);
	}
}

void UUiRadioGroup::Build(const UUIStyle& Style, const TArray<FText>& Options)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	WidgetTree->RootWidget = Row;
	for (int32 I = 0; I < Options.Num(); ++I)
	{
		UUiButton* B = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		B->SetLabel(Options[I]);
		B->Build(Style, EUiButtonKind::Secondary);
		UUiRadioEntry* Entry = NewObject<UUiRadioEntry>(this);
		Entry->Index = I;
		Entry->Owner = this;
		B->OnClicked.AddDynamic(Entry, &UUiRadioEntry::HandleClicked);
		UHorizontalBoxSlot* BSlot = Row->AddChildToHorizontalBox(B);
		BSlot->SetPadding(FMargin(I == 0 ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f));
		Buttons.Add(B);
		Entries.Add(Entry);
	}
	Paint();
}

void UUiRadioGroup::SetSelected(int32 Index, bool bBroadcast)
{
	// CLAMPED, not ignored: a saved index past the end (a quality level from a build with more)
	// still lands on a real choice, and exactly one segment stays lit (Review Focus 4).
	Selected = Buttons.Num() > 0 ? FMath::Clamp(Index, 0, Buttons.Num() - 1) : 0;
	Paint();
	if (bBroadcast)
	{
		++Broadcasts;
		OnSelectionChanged.Broadcast(Selected);
	}
}

void UUiRadioGroup::Choose(int32 Index)
{
	if (Index == Selected)
	{
		return;
	}
	SetSelected(Index, /*bBroadcast=*/true);
}

void UUiRadioGroup::Paint()
{
	for (int32 I = 0; I < Buttons.Num(); ++I)
	{
		if (Buttons[I] != nullptr)
		{
			Buttons[I]->SetState(true, I == Selected);
		}
	}
}

int32 UUiRadioGroup::SelectedButtonCountForTest() const
{
	int32 Lit = 0;
	for (const UUiButton* B : Buttons)
	{
		// The lit segment is the one whose fill is Accent (UUiButton::LookFor's "selected").
		if (B != nullptr && B->GetBackgroundColor().Equals(GetDefault<UUIStyle>()->Accent))
		{
			++Lit;
		}
	}
	return Lit;
}
```

Note `SelectedButtonCountForTest` compares against the CDO style's Accent: the test builds with the CDO style. Ledger this as a known test-only coupling if the executor prefers to hold the style pointer instead.

- [ ] **Step 5: Build; run `-Filter AirportMgr.UI.Controls`.** Expected 3 pass.
- [ ] **Step 6: Commit** `git commit -m "feat(ui): UUiRadioGroup - a segmented one-of-N"` (add the new files).

---

### Task 4: UUiDropdown

**Files:** Create `UI/UiDropdown.h`, `UI/UiDropdown.cpp`; Modify `UI/UiControlsTest.cpp`

**Interfaces - produces:**

```cpp
class UUiDropdownList : public UUserWidget {   // the popup: a Surface card of Ghost options
	void Build(const UUIStyle& Style, const TArray<FText>& Options, UUiDropdown& Owner);
	int32 OptionCountForTest() const;
};
class UUiDropdownEntry : public UObject { int32 Index; TWeakObjectPtr<UUiDropdown> Owner; UFUNCTION() void HandleClicked(); };
class UUiDropdown : public UUserWidget {
	void Build(const UUIStyle& Style, const TArray<FText>& Options);
	void SetSelected(int32 Index, bool bBroadcast = false);
	int32 GetSelected() const;
	void Choose(int32 Index);   // from the list: select, broadcast if changed, close
	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnSelectionChanged;
	UFUNCTION() UUserWidget* BuildMenu();   // UMenuAnchor's content
	FString LabelForTest() const; int32 BroadcastCountForTest() const;
};
```

- [ ] **Step 1: Write the failing test** - append (include `UI/UiDropdown.h`):

```cpp
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
	const UUiDropdownList* List = Cast<UUiDropdownList>(D->BuildMenu());
	if (TestNotNull(TEXT("the popup builds"), List))
	{
		TestEqual(TEXT("with every option"), List->OptionCountForTest(), 4);
	}
	D->Choose(0);
	TestTrue(TEXT("a choice from the list reads on the button"), D->LabelForTest().StartsWith(TEXT("Low")));
	TestEqual(TEXT("and raises the event once"), D->BroadcastCountForTest(), 1);
	return true;
}
```

- [ ] **Step 2: Build; expect compile failure.**

- [ ] **Step 3: Write `UI/UiDropdown.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiRadioGroup.h"
#include "UiDropdown.generated.h"

class UMenuAnchor;
class UUiButton;
class UUiDropdown;
class UUIStyle;

/** One option's click in the popup - the UUiRadioEntry reason. */
UCLASS()
class AIRPORTMGR_API UUiDropdownEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 Index = INDEX_NONE;
	UPROPERTY() TWeakObjectPtr<UUiDropdown> Owner;
	UFUNCTION() void HandleClicked();
};

/** The popup: a Surface card of Ghost options, one per line. */
UCLASS()
class AIRPORTMGR_API UUiDropdownList : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FText>& Options, UUiDropdown& Owner);
	int32 OptionCountForTest() const { return Entries.Num(); }

private:
	UPROPERTY() TArray<TObjectPtr<UUiDropdownEntry>> Entries;
};

/**
 * A choice from a list that opens on click (UI library step 4a): a Secondary UUiButton reading the
 * current option and a down-arrow, inside a UMenuAnchor whose popup Slate places in its own layer -
 * above every window, and never clipped by a window's ClipToBounds (the spec's known risk).
 */
UCLASS()
class AIRPORTMGR_API UUiDropdown : public UUserWidget
{
	GENERATED_BODY()

public:
	void Build(const UUIStyle& Style, const TArray<FText>& InOptions);

	/** From CODE: clamped into range; no event unless bBroadcast. */
	void SetSelected(int32 Index, bool bBroadcast = false);
	int32 GetSelected() const { return Selected; }

	/** From the popup: selects, raises the event if it changed, and closes the popup. */
	void Choose(int32 Index);

	UPROPERTY(BlueprintAssignable) FUiChoiceChanged OnSelectionChanged;

	/** The anchor's content, built on each open. Public for the test that builds it headless. */
	UFUNCTION() UUserWidget* BuildMenu();

	UFUNCTION() void HandleOpenClicked();

	FString LabelForTest() const;
	int32 BroadcastCountForTest() const { return Broadcasts; }

private:
	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	UPROPERTY() TObjectPtr<UMenuAnchor> Anchor;
	UPROPERTY() TObjectPtr<UUiButton> Button;
	TArray<FText> Options;
	int32 Selected = 0;
	int32 Broadcasts = 0;
};
```

- [ ] **Step 4: Write `UI/UiDropdown.cpp`:**

```cpp
#include "UI/UiDropdown.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/MenuAnchor.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

void UUiDropdownEntry::HandleClicked()
{
	if (UUiDropdown* D = Owner.Get())
	{
		D->Choose(Index);
	}
}

void UUiDropdownList::Build(const UUIStyle& Style, const TArray<FText>& Options, UUiDropdown& Owner)
{
	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("DropdownCard"));
	Card->SetBrush(FSlateRoundedBoxBrush(Style.Surface, Style.ControlRadius, Style.Rule, 1.0f));
	Card->SetPadding(FMargin(4.0f));
	WidgetTree->RootWidget = Card;
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Card->SetContent(Column);
	for (int32 I = 0; I < Options.Num(); ++I)
	{
		UUiButton* B = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		B->SetLabel(Options[I]);
		B->Build(Style, EUiButtonKind::Ghost);
		UUiDropdownEntry* Entry = NewObject<UUiDropdownEntry>(this);
		Entry->Index = I;
		Entry->Owner = &Owner;
		B->OnClicked.AddDynamic(Entry, &UUiDropdownEntry::HandleClicked);
		Column->AddChildToVerticalBox(B)->SetHorizontalAlignment(HAlign_Fill);
		Entries.Add(Entry);
	}
}

void UUiDropdown::Build(const UUIStyle& InStyle, const TArray<FText>& InOptions)
{
	Style = &InStyle;
	Options = InOptions;
	Anchor = WidgetTree->ConstructWidget<UMenuAnchor>(UMenuAnchor::StaticClass(), TEXT("DropdownAnchor"));
	Anchor->SetPlacement(MenuPlacement_ComboBox);
	Anchor->OnGetUserMenuContentEvent.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UUiDropdown, BuildMenu));
	WidgetTree->RootWidget = Anchor;
	Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("DropdownButton"));
	Button->SetLabel(FText::GetEmpty());   // SetSelected fills it
	Button->SetLabel(FText::FromString(TEXT(" ")));
	Button->Build(InStyle, EUiButtonKind::Secondary);
	Button->OnClicked.AddDynamic(this, &UUiDropdown::HandleOpenClicked);
	Anchor->SetContent(Button);
	SetSelected(0);
}

void UUiDropdown::SetSelected(int32 Index, bool bBroadcast)
{
	Selected = Options.Num() > 0 ? FMath::Clamp(Index, 0, Options.Num() - 1) : 0;
	if (Button != nullptr && Options.IsValidIndex(Selected))
	{
		// The arrow says "this opens a list" - without it the control reads as a plain button.
		Button->SetLabel(FText::Format(INVTEXT("{0}  \u25BE"), Options[Selected]));
	}
	if (bBroadcast)
	{
		++Broadcasts;
		OnSelectionChanged.Broadcast(Selected);
	}
}

void UUiDropdown::Choose(int32 Index)
{
	if (Anchor != nullptr && Anchor->IsOpen())
	{
		Anchor->Close();
	}
	const int32 Before = Selected;
	SetSelected(Index, /*bBroadcast=*/false);
	if (Selected != Before)
	{
		++Broadcasts;
		OnSelectionChanged.Broadcast(Selected);
	}
}

UUserWidget* UUiDropdown::BuildMenu()
{
	if (Style == nullptr)
	{
		return nullptr;
	}
	UUiDropdownList* List = CreateWidget<UUiDropdownList>(this, UUiDropdownList::StaticClass());
	List->Build(*Style, Options, *this);
	return List;
}

void UUiDropdown::HandleOpenClicked()
{
	if (Anchor != nullptr)
	{
		Anchor->Open(/*bFocusMenu=*/true);
	}
}

FString UUiDropdown::LabelForTest() const
{
	return Button != nullptr && Button->GetLabel() != nullptr ? Button->GetLabel()->GetText().ToString() : FString();
}
```

(The two `SetLabel` calls before `Build`: a label must be non-empty for `UUiButton::BuildContent` to create the text block that `SetSelected` later retitles - the first line is redundant; keep only `SetLabel(FText::FromString(TEXT(" ")))` with that comment.)

- [ ] **Step 5: Build; run `-Filter AirportMgr.UI.Controls`.** Expected 4 pass. `\u25BE` renders in Inter; if the capture in 4b shows a box, use "v".
- [ ] **Step 6: Commit** `git commit -m "feat(ui): UUiDropdown - a choice from a popup list"` (add the new files).

---

### Task 5: Verify

- [ ] **Step 1: Full suite; quote the `N test(s) run` line.**
- [ ] **Step 2: No visual check here** - nothing shows these controls until plan 4b's Settings dialog; its PIE check covers their look. Say so in the hand-over.
