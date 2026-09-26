// THE VARIANT HALF OF UBuildBarWidget - the popout row under the tools. A second .cpp for one
// class, rather than more of BuildBarWidget.cpp, because the row has its own lifecycle (rebuilt
// on a signature, not built once) and BuildBarWidget.cpp was already the bar's longest file.
// Logs under LogRoadBuild: LogBuildBar is BuildBarWidget.cpp's file-static, and a second
// DEFINE of it here would collide in the unity build (Check-Architecture rule 2).

#include "BuildBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "UIStyle.h"

void UBuildBarVariantEntry::HandleClicked()
{
	if (UBuildBarWidget* Bar = Owner.Get())
	{
		Bar->RunVariant(Axis, Option);
	}
}

void UBuildBarWidget::RunVariant(int32 Axis, int32 Option)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("Variant click %d/%d ignored: no controller"), Axis, Option);
		return;
	}
	RunVariantFor(*C, Axis, Option);
}

void UBuildBarWidget::RunVariantFor(ARoadBuildController& C, int32 Axis, int32 Option)
{
	C.SelectActiveVariant(Axis, Option);
}

bool UBuildBarWidget::IsVariantSectionVisibleForTest() const
{
	return VariantSection != nullptr && VariantSection->GetVisibility() != ESlateVisibility::Collapsed;
}

void UBuildBarWidget::RefreshVariantsFor(ARoadBuildController& C)
{
	if (VariantSection == nullptr)
	{
		return;
	}

	TArray<FToolVariantAxis> Axes;
	C.GetActiveVariantAxes(Axes);

	// THE SIGNATURE: tool index, then every axis Id and option Id. Built every tick - a few dozen
	// FNames, no allocation past the first - because the alternative, subscribing to tool
	// changes AND content edits, is the lifetime bookkeeping the bar's own header rejects.
	TArray<FName> Signature;
	Signature.Add(FName(*FString::FromInt(C.GetActiveToolIndex())));
	for (const FToolVariantAxis& Axis : Axes)
	{
		Signature.Add(Axis.Id);
		for (const FToolVariant& Option : Axis.Options)
		{
			Signature.Add(Option.Id);
		}
	}
	if (Signature != VariantSignature)
	{
		VariantSignature = MoveTemp(Signature);
		RebuildVariants(Axes);
	}

	VariantSection->SetVisibility(Axes.Num() > 0
		? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);

	// LIT AND ENABLED, every tick, exactly as RefreshStateFor paints the tool buttons: accent
	// means armed, so the lit option is the one the next click lays.
	const UUIStyle* Style = PanelStyle;
	for (UBuildBarVariantEntry* Entry : VariantEntries)
	{
		if (Entry == nullptr || Entry->Button == nullptr || !Axes.IsValidIndex(Entry->Axis)
			|| !Axes[Entry->Axis].Options.IsValidIndex(Entry->Option))
		{
			continue;
		}
		const FToolVariantAxis& Axis = Axes[Entry->Axis];
		const bool bEnabled = Axis.Options[Entry->Option].bEnabled;
		const bool bLit = bEnabled && Axis.Current == Entry->Option;
		Entry->Button->SetIsEnabled(bEnabled);
		Entry->Button->SetBackgroundColor(bLit ? Style->Accent : Style->Button);
		const FLinearColor Content = bLit ? Style->PanelDark : (bEnabled ? Style->Text : Style->TextMuted);
		Entry->Label->SetColorAndOpacity(FSlateColor(Content));
	}
}

void UBuildBarWidget::RebuildVariants(const TArray<FToolVariantAxis>& Axes)
{
	const UUIStyle* Style = PanelStyle;
	VariantSection->ClearChildren();
	VariantEntries.Reset();

	FString Described;
	for (int32 AxisIndex = 0; AxisIndex < Axes.Num(); ++AxisIndex)
	{
		const FToolVariantAxis& Axis = Axes[AxisIndex];
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());

		// THE AXIS NAMED, as a section is headed: a runway's three lines would otherwise be
		// three runs of buttons with nothing to say which is the surface.
		UTextBlock* Heading = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Heading->SetText(FText::FromString(Axis.Label.ToString().ToUpper()));
		Style->ApplyText(*Heading, EUITextRole::Heading, Style->TextMuted);
		UHorizontalBoxSlot* HeadingSlot = Line->AddChildToHorizontalBox(Heading);
		HeadingSlot->SetVerticalAlignment(VAlign_Center);
		HeadingSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));

		for (int32 OptionIndex = 0; OptionIndex < Axis.Options.Num(); ++OptionIndex)
		{
			const FToolVariant& Option = Axis.Options[OptionIndex];
			UBuildBarVariantEntry* Entry = NewObject<UBuildBarVariantEntry>(this);
			Entry->Axis = AxisIndex;
			Entry->Option = OptionIndex;
			Entry->Owner = this;
			Entry->Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
			Entry->Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());

			// LABEL, AND THE DETAIL UNDER IT WHEN THERE IS ONE - one text block, two lines,
			// because the detail is part of what the option IS, not a caption beside it.
			Entry->Label->SetText(Option.Detail.IsEmpty() ? Option.Label
				: FText::Format(INVTEXT("{0}\n{1}"), Option.Label, Option.Detail));
			Style->ApplyText(*Entry->Label, EUITextRole::Label, Style->Text);
			Entry->Label->SetJustification(ETextJustify::Center);

			Entry->Button->SetContent(Entry->Label);
			Entry->Button->SetBackgroundColor(Style->Button);
			Entry->Button->SetToolTipText(FText::Format(INVTEXT("{0}: {1}"), Axis.Label, Option.Label));
			Entry->Button->OnClicked.AddDynamic(Entry, &UBuildBarVariantEntry::HandleClicked);

			UHorizontalBoxSlot* ButtonSlot = Line->AddChildToHorizontalBox(Entry->Button);
			ButtonSlot->SetPadding(FMargin(3.0f, 2.0f));
			VariantEntries.Add(Entry);
		}
		VariantSection->AddChild(Line);
		Described += FString::Printf(TEXT(" %s[%d]"), *Axis.Id.ToString(), Axis.Options.Num());
	}

	// ONE LINE PER REBUILD, naming each row and its size: "the row shows the wrong widths" is
	// then answerable from the log - was it rebuilt at all, and from what.
	UE_LOG(LogRoadBuild, Log, TEXT("Variant bar rebuilt: %d button(s)%s"),
		VariantEntries.Num(), Described.IsEmpty() ? TEXT(" (no choices, hidden)") : *Described);
}
