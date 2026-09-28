#include "UI/UiButton.h"

#include "Brushes/SlateNoResource.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "UIStyle.h"

FUiButtonLook UUiButton::LookFor(const UUIStyle& S, EUiButtonKind InKind, bool bInEnabled, bool bInSelected)
{
	// GHOST TAKES ITS FILL FROM ITS OWN PER-STATE BRUSHES (transparent at rest, Well on hover),
	// so its background colour stays white and only the ink moves.
	if (InKind == EUiButtonKind::Ghost)
	{
		return { FLinearColor::White, bInEnabled ? S.Ink : S.InkMuted };
	}
	// DISABLED IS CONTROL + MUTED INK, WHATEVER THE KIND. Disabled dims a button by its ink, never
	// by lightening the fill (the inspector's Depart bug), and a disabled Primary or Danger must
	// not keep the colour that says "press me".
	if (!bInEnabled)
	{
		return { S.Control, S.InkMuted };
	}
	if (bInSelected)
	{
		return { S.Accent, S.InkOnAccent };
	}
	switch (InKind)
	{
	case EUiButtonKind::Primary: return { S.Accent, S.InkOnAccent };
	// Danger reads SURFACE on brick: InkOnAccent (dark slate) is 2.2:1 on Warning, Surface ~4:1.
	case EUiButtonKind::Danger:  return { S.Warning, S.Surface };
	default:                     return { S.Control, S.Ink };
	}
}

void UUiButton::SetLabel(const FText& Text)
{
	PendingLabel = Text;
	if (Label != nullptr)
	{
		Label->SetText(Text);   // after Build: text only, the tree stays (ContentShape)
	}
}

void UUiButton::SetDetail(const FText& Text)
{
	PendingDetail = Text;
	if (Detail != nullptr)
	{
		Detail->SetText(Text);
	}
}

void UUiButton::SetIcon(UTexture2D* InIcon, float Size)
{
	IconTexture = InIcon;
	IconSize = Size;
}

void UUiButton::SetLabelMinWidth(float Width)
{
	LabelMinWidth = Width;
	if (Label != nullptr)
	{
		Label->SetMinDesiredWidth(Width);
	}
}

void UUiButton::Build(const UUIStyle& InStyle, EUiButtonKind InKind, EUiButtonLayout InLayout, bool bStylePadding)
{
	Style = &InStyle;
	Kind = InKind;
	Layout = InLayout;

	FButtonStyle ButtonStyle = GetStyle();
	if (Kind == EUiButtonKind::Ghost)
	{
		const FSlateRoundedBoxBrush Hot(InStyle.Well, InStyle.ControlRadius);
		ButtonStyle.SetNormal(FSlateNoResource());
		ButtonStyle.SetHovered(Hot);
		ButtonStyle.SetPressed(Hot);
		ButtonStyle.SetDisabled(FSlateNoResource());
	}
	else
	{
		// Tint STEPS on one white fill: the tint reaches the shader as an 8-bit vertex colour,
		// so nothing can exceed 1 - hover is "full", rest and press step down from it.
		const FSlateBrush Fill = InStyle.ControlFill();
		auto Step = [&Fill](float Grey)
		{
			FSlateBrush B = Fill;
			B.TintColor = FSlateColor(FLinearColor(Grey, Grey, Grey, 1.0f));
			return B;
		};
		ButtonStyle.SetNormal(Step(0.96f));
		ButtonStyle.SetHovered(Step(1.0f));
		ButtonStyle.SetPressed(Step(0.85f));
		ButtonStyle.SetDisabled(Step(0.96f));
	}
	if (bStylePadding)
	{
		ButtonStyle.SetNormalPadding(InStyle.ButtonPadding);
		ButtonStyle.SetPressedPadding(InStyle.ButtonPadding);
	}
	SetStyle(ButtonStyle);
	BuildContent();
	bPainted = false;
	Paint();
}

void UUiButton::BuildContent()
{
	if (!PendingLabel.IsEmpty())
	{
		Label = NewObject<UTextBlock>(this);
		Label->SetText(PendingLabel);
		Style->ApplyText(*Label, EUITextRole::Label, Style->Ink);
		if (LabelMinWidth > 0.0f) { Label->SetMinDesiredWidth(LabelMinWidth); }
		if (Layout == EUiButtonLayout::Stacked) { Label->SetJustification(ETextJustify::Center); }
	}
	if (!PendingDetail.IsEmpty())
	{
		Detail = NewObject<UTextBlock>(this);
		Detail->SetText(PendingDetail);
		Style->ApplyText(*Detail, EUITextRole::Label, Style->InkMuted);
		if (Layout == EUiButtonLayout::Stacked) { Detail->SetJustification(ETextJustify::Center); }
	}
	if (IconTexture != nullptr)
	{
		Icon = NewObject<UImage>(this);
		Icon->SetBrushFromTexture(IconTexture, false);
		Icon->SetDesiredSizeOverride(FVector2D(IconSize));
	}

	// LABEL ONLY: THE CONTENT IS THE TEXT BLOCK, so a reader that casts GetContent() to a
	// UTextBlock (the inspector's Follow caption) keeps working without knowing about this class.
	if (Icon == nullptr && Detail == nullptr)
	{
		if (Label != nullptr) { SetContent(Label); }
		return;
	}
	if (Layout == EUiButtonLayout::Stacked)
	{
		UVerticalBox* Stack = NewObject<UVerticalBox>(this);
		if (Icon != nullptr) { Stack->AddChildToVerticalBox(Icon)->SetHorizontalAlignment(HAlign_Center); }
		if (Label != nullptr) { Stack->AddChildToVerticalBox(Label)->SetHorizontalAlignment(HAlign_Center); }
		if (Detail != nullptr) { Stack->AddChildToVerticalBox(Detail)->SetHorizontalAlignment(HAlign_Center); }
		SetContent(Stack);
		return;
	}
	UHorizontalBox* Line = NewObject<UHorizontalBox>(this);
	if (Icon != nullptr)
	{
		UHorizontalBoxSlot* IconSlot = Line->AddChildToHorizontalBox(Icon);
		IconSlot->SetVerticalAlignment(VAlign_Center);
		IconSlot->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));
	}
	if (Label != nullptr) { Line->AddChildToHorizontalBox(Label)->SetVerticalAlignment(VAlign_Center); }
	if (Detail != nullptr)
	{
		UHorizontalBoxSlot* DetailSlot = Line->AddChildToHorizontalBox(Detail);
		DetailSlot->SetVerticalAlignment(VAlign_Center);
		DetailSlot->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	}
	SetContent(Line);
}

void UUiButton::SetState(bool bInEnabled, bool bInSelected)
{
	if (bPainted && bInEnabled == bEnabled && bInSelected == bSelected)
	{
		return;   // UnchangedStatePaintsNothing: SetBackgroundColor has no early-out of its own
	}
	bEnabled = bInEnabled;
	bSelected = bInSelected;
	Paint();
}

void UUiButton::Paint()
{
	// ENABLED STATE FIRST, style or not: a designer-placed button (UInspectorWidget's
	// BindWidgetOptional Depart) is never Built, and must still stop taking clicks when disabled.
	SetIsEnabled(bEnabled);
	if (Style == nullptr)
	{
		return;   // not Built: no colours to paint with; Build paints
	}
	const FUiButtonLook Look = LookFor(*Style, Kind, bEnabled, bSelected);
	SetBackgroundColor(Look.Fill);
	if (Label != nullptr) { Label->SetColorAndOpacity(FSlateColor(Look.Ink)); }
	if (Icon != nullptr) { Icon->SetColorAndOpacity(Look.Ink); }
	// Detail stays muted unless the button is lit, where muted would vanish into the Accent.
	if (Detail != nullptr) { Detail->SetColorAndOpacity(FSlateColor(bSelected && bEnabled ? Look.Ink : Style->InkMuted)); }
	bPainted = true;
	++PaintCount;
}
