#include "BuildBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Model/OpsEvents.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "RoadBuildController.h"

DEFINE_LOG_CATEGORY_STATIC(LogBuildBar, Log, All);

void UBuildBarEntry::HandleClicked()
{
	if (UBuildBarWidget* Bar = Owner.Get())
	{
		Bar->RunAction(ActionIndex);
	}
}

ARoadBuildController* UBuildBarWidget::Controller() const
{
	// The owning player when the controller created us; the first controller otherwise
	// (tests create the bar from a world). Null is a supported state: buttons still build,
	// and RefreshState simply has nothing to ask.
	if (APlayerController* Owning = GetOwningPlayer())
	{
		return Cast<ARoadBuildController>(Owning);
	}
	return GetWorld() ? Cast<ARoadBuildController>(GetWorld()->GetFirstPlayerController()) : nullptr;
}

bool UBuildBarWidget::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || bBuilt || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	bBuilt = true;
	EnsureSlots();
	BuildButtons();

	// The first UI consumer of the outcome bus (systems spec §5.1): the latest notification
	// shows above the bar. Bound here rather than polled because a notification is an event
	// with a text, not a state to read back.
	if (UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
	{
		Runtime->GetEvents()->OnNotification.AddDynamic(this, &UBuildBarWidget::OnNotification);
	}
	return bOk;
}

void UBuildBarWidget::EnsureSlots()
{
	// A root only if the asset gave none: BindWidgetOptional has already filled every slot
	// the asset supplies, and a code-built root would replace the designer's bar.
	//
	// The code-built chrome is a real bottom bar, not a placeholder: a canvas with the bar
	// anchored to the bottom edge and the notification line above it. Python cannot author
	// the Blueprint on this engine build (UWidgetBlueprint::WidgetTree is not a scriptable
	// property), so this IS the default look, and a Blueprint is an optional restyle.
	UHorizontalBox* Fallback = nullptr;
	if (WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("FallbackRoot"));
		WidgetTree->RootWidget = Root;

		if (NotificationText == nullptr)
		{
			NotificationText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NotificationText"));
			UCanvasPanelSlot* NoteSlot = Root->AddChildToCanvas(NotificationText);
			NoteSlot->SetAnchors(FAnchors(0.5f, 1.0f, 0.5f, 1.0f));
			NoteSlot->SetAlignment(FVector2D(0.5, 1.0));
			NoteSlot->SetAutoSize(true);
			NoteSlot->SetPosition(FVector2D(0.0, -BarHeight - 16.0));
		}

		UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("BarBorder"));
		Border->SetBrushColor(BarTint);
		Border->SetPadding(FMargin(12.0f, 8.0f));
		UCanvasPanelSlot* BarSlot = Root->AddChildToCanvas(Border);
		// Stretched across the bottom edge: with both anchors on y=1, Offsets reads as
		// (left margin, top margin, right margin, HEIGHT).
		BarSlot->SetAnchors(FAnchors(0.0f, 1.0f, 1.0f, 1.0f));
		BarSlot->SetAlignment(FVector2D(0.0, 1.0));
		BarSlot->SetOffsets(FMargin(0.0f, 0.0f, 0.0f, static_cast<float>(BarHeight)));

		Fallback = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("FallbackBar"));
		Border->SetContent(Fallback);
		UE_LOG(LogBuildBar, Log, TEXT("No bar asset: building the code-only bar"));
	}

	auto Ensure = [&](TObjectPtr<UPanelWidget>& Section, const TCHAR* Name)
	{
		if (Section != nullptr)
		{
			return;
		}
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), Name);
		if (Fallback != nullptr)
		{
			UHorizontalBoxSlot* BoxSlot = Fallback->AddChildToHorizontalBox(Box);
			BoxSlot->SetPadding(FMargin(12.0f, 0.0f));
			BoxSlot->SetVerticalAlignment(VAlign_Center);
		}
		else if (UPanelWidget* Root = Cast<UPanelWidget>(WidgetTree->RootWidget))
		{
			// The asset has a root but not this section: append to the root so the buttons
			// are at least visible, and say so - the designer forgot a panel.
			Root->AddChild(Box);
			UE_LOG(LogBuildBar, Warning, TEXT("Bar asset has no '%s' panel; appended a plain one to the root"), Name);
		}
		Section = Box;
	};
	Ensure(TimeSection, TEXT("TimeSection"));
	Ensure(ToolsSection, TEXT("ToolsSection"));
	Ensure(EditSection, TEXT("EditSection"));
	Ensure(AircraftSection, TEXT("AircraftSection"));
	Ensure(GameSection, TEXT("GameSection"));

	if (ClockText == nullptr)
	{
		ClockText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ClockText"));
		if (UHorizontalBox* Box = Cast<UHorizontalBox>(TimeSection))
		{
			UHorizontalBoxSlot* ClockSlot = Box->AddChildToHorizontalBox(ClockText);
			ClockSlot->SetPadding(FMargin(0.0f, 0.0f, 12.0f, 0.0f));
			ClockSlot->SetVerticalAlignment(VAlign_Center);
		}
		else
		{
			TimeSection->AddChild(ClockText);
		}
	}
}

UPanelWidget* UBuildBarWidget::SectionPanel(EActionSection Section) const
{
	switch (Section)
	{
	case EActionSection::Time:     return TimeSection;
	case EActionSection::Tools:    return ToolsSection;
	case EActionSection::Edit:     return EditSection;
	case EActionSection::Aircraft: return AircraftSection;
	case EActionSection::Game:     return GameSection;
	}
	return nullptr;
}

void UBuildBarWidget::BuildButtons()
{
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		const FBuildAction& Action = Actions[Index];
		UPanelWidget* Panel = SectionPanel(Action.Section);
		if (Panel == nullptr)
		{
			continue;
		}

		UBuildBarEntry* Entry = NewObject<UBuildBarEntry>(this);
		Entry->ActionIndex = Index;
		Entry->Owner = this;
		Entry->Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		Entry->Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());

		// The key in the caption, so the bar teaches the shortcuts it replaces.
		FString Caption = Action.Label.ToString();
		if (Action.Key.IsValid())
		{
			Caption += FString::Printf(TEXT(" (%s%s)"), Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""),
				*Action.Key.GetDisplayName().ToString());
		}
		Entry->Label->SetText(FText::FromString(Caption));
		FSlateFontInfo Font = Entry->Label->GetFont();
		Font.Size = FontSize;
		Entry->Label->SetFont(Font);

		Entry->Button->SetContent(Entry->Label);
		Entry->Button->SetBackgroundColor(NormalTint);
		Entry->Button->OnClicked.AddDynamic(Entry, &UBuildBarEntry::HandleClicked);

		if (UHorizontalBox* Box = Cast<UHorizontalBox>(Panel))
		{
			Box->AddChildToHorizontalBox(Entry->Button)->SetPadding(ButtonPadding);
		}
		else
		{
			Panel->AddChild(Entry->Button);
		}
		Entries.Add(Entry);
	}
	UE_LOG(LogBuildBar, Log, TEXT("Build bar: %d buttons from %d actions"), Entries.Num(), Actions.Num());
}

void UBuildBarWidget::RunAction(int32 ActionIndex)
{
	ARoadBuildController* C = Controller();
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	if (C == nullptr || !Actions.IsValidIndex(ActionIndex))
	{
		UE_LOG(LogBuildBar, Warning, TEXT("Bar click %d ignored: no controller or no such action"), ActionIndex);
		return;
	}
	if (!Actions[ActionIndex].IsEnabled(*C))
	{
		return;
	}
	UE_LOG(LogBuildBar, Log, TEXT("Bar: %s"), *Actions[ActionIndex].Id.ToString());
	Actions[ActionIndex].Execute(*C);
}

void UBuildBarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshState();
	RefreshClock();
}

void UBuildBarWidget::RefreshState()
{
	const ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		return;
	}
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	for (UBuildBarEntry* Entry : Entries)
	{
		if (Entry == nullptr || Entry->Button == nullptr || !Actions.IsValidIndex(Entry->ActionIndex))
		{
			continue;
		}
		const FBuildAction& Action = Actions[Entry->ActionIndex];
		const bool bEnabled = Action.IsEnabled(*C);
		Entry->Button->SetIsEnabled(bEnabled);
		Entry->Button->SetBackgroundColor(!bEnabled ? DisabledTint : Action.IsActive(*C) ? ActiveTint : NormalTint);
	}
}

void UBuildBarWidget::RefreshClock()
{
	if (ClockText == nullptr)
	{
		return;
	}
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	if (Runtime == nullptr)
	{
		ClockText->SetText(FText::FromString(TEXT("no clock")));
		return;
	}
	const USimClock* Clock = Runtime->GetClock();
	const int32 Hour = static_cast<int32>(Clock->TimeOfDay() / 3600.0);
	const int32 Minute = static_cast<int32>(FMath::Fmod(Clock->TimeOfDay(), 3600.0) / 60.0);
	ClockText->SetText(FText::FromString(FString::Printf(TEXT("Day %d  %02d:%02d  x%.0f%s"),
		Clock->Day() + 1, Hour, Minute, USimClock::Multiplier(Clock->GetSpeed()),
		Clock->GetSpeed() == ESimSpeed::Paused ? TEXT("  PAUSED") : TEXT(""))));
}

void UBuildBarWidget::OnNotification(const FString& Text)
{
	if (NotificationText != nullptr)
	{
		NotificationText->SetText(FText::FromString(Text));
	}
}

int32 UBuildBarWidget::ButtonCountForTest(EActionSection Section) const
{
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	int32 Count = 0;
	for (const UBuildBarEntry* Entry : Entries)
	{
		if (Entry != nullptr && Actions.IsValidIndex(Entry->ActionIndex) && Actions[Entry->ActionIndex].Section == Section)
		{
			++Count;
		}
	}
	return Count;
}

bool UBuildBarWidget::HasRootWidgetForTest() const
{
	return WidgetTree != nullptr && WidgetTree->RootWidget != nullptr;
}
