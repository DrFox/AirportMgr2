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
	FSlateBrush Handle = FSlateRoundedBoxBrush(Style.Accent, 7.0f);
	Handle.ImageSize = FVector2D(14.0, 14.0);
	Look.SetNormalBarImage(Bar).SetHoveredBarImage(Bar).SetDisabledBarImage(Bar);
	Look.SetNormalThumbImage(Handle).SetHoveredThumbImage(Handle).SetDisabledThumbImage(Handle);
	Look.SetBarThickness(4.0f);
	Slider->SetWidgetStyle(Look);
	Slider->SetMinValue(Min);
	Slider->SetMaxValue(Max);
	Slider->SetStepSize(Step);
	Slider->OnValueChanged.AddDynamic(this, &UUiSlider::HandleSliderMoved);
	Slider->OnMouseCaptureEnd.AddDynamic(this, &UUiSlider::HandleReleased);
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
		// USlider::SetValue BROADCASTS OnValueChanged (Slider.cpp:159-162, UE 5.8) - flagged, so
		// HandleSliderMoved can tell code's echo from the player's drag and not rebroadcast it.
		// ENFORCED BY: AirportMgr.UI.Controls.Slider ("code setting it raises nothing").
		TGuardValue<bool> Guard(bSettingFromCode, true);
		Slider->SetValue(Value);
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
	if (bSettingFromCode)
	{
		return;   // our own SetValue's echo, not the player
	}
	// ONLY A CHANGE IS NEWS. USlider reports every mouse move, and moves within one step quantise
	// to the value already held - rebroadcasting each re-applied a live setting many times a
	// second (step 4a final review, Important 2).
	const float Before = Value;
	SetValue(Raw);
	if (!FMath::IsNearlyEqual(Value, Before))
	{
		++Broadcasts;
		OnValueChanged.Broadcast(Value);
	}
}

void UUiSlider::HandleReleased()
{
	++Commits;
	OnValueCommitted.Broadcast(Value);
}

FString UUiSlider::ReadoutForTest() const
{
	return Readout != nullptr ? Readout->GetText().ToString() : FString();
}
