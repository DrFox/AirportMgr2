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

	// A HIT AREA, not a button look: the track is the whole appearance, so the engine button
	// draws nothing (and rule 29 allows its FButtonStyle - it lives in UI/).
	UButton* Hit = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ToggleHit"));
	FButtonStyle NoLook = Hit->GetStyle();
	NoLook.SetNormal(FSlateNoResource());
	NoLook.SetHovered(FSlateNoResource());
	NoLook.SetPressed(FSlateNoResource());
	NoLook.SetDisabled(FSlateNoResource());
	NoLook.SetNormalPadding(FMargin(0.0f));
	NoLook.SetPressedPadding(FMargin(0.0f));
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
