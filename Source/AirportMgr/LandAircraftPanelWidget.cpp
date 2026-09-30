#include "LandAircraftPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Entities/AircraftType.h"
#include "Model/Airport.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "Styling/SlateBrush.h"
#include "UIStyle.h"

#define LOCTEXT_NAMESPACE "LandPanel"

void ULandRowEntry::HandleClick()
{
	if (ULandAircraftPanelWidget* Panel = Owner.Get())
	{
		Panel->Choose(Type);
	}
}

void ULandAircraftPanelWidget::BuildOnce(const UUIStyle& Style)
{
	// TOP LEFT (its window - WantsWindow). The offer inbox owns the top right and the ledger sits
	// under it; the bar owns the bottom. The window hides the whole panel, so the Blueprint path -
	// where EnsureContentRoot returns null - still hides.
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureContentRoot(TEXT("LandCard"))))
	{
		if (TitleText == nullptr)
		{
			TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TitleText"));
			TitleText->SetText(LOCTEXT("Title", "LAND AN AIRCRAFT"));
			Style.ApplyText(*TitleText, EUITextRole::Heading, Style.InkMuted);
			// The window's title bar says it now; kept (collapsed) because a Blueprint may bind it.
			TitleText->SetVisibility(ESlateVisibility::Collapsed);
			Column->AddChildToVerticalBox(TitleText);
		}
		if (RowColumn == nullptr)
		{
			RowColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RowColumn"));
			UVerticalBoxSlot* RowsSlot = Column->AddChildToVerticalBox(RowColumn);
			RowsSlot->SetPadding(FMargin(0.0f, Style.RowGap, 0.0f, 0.0f));
		}
	}

	// SelfHitTestInvisible on the ROOT; the WINDOW hides (SetShown) - see
	// UAirportMgrPanelWidget::BuildOnce.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetShown(false);
}

bool ULandAircraftPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("land");
	Out.Title = LOCTEXT("LandWindow", "Land an aircraft");
	Out.Anchor = EUiWindowAnchor::TopLeft;
	Out.Offset = FVector2D(12.0, TopOffset);
	return true;
}

void ULandAircraftPanelWidget::OnWindowClosedByPlayer()
{
	// THE CLOSE BUTTON IS THE TOGGLE - the ledger's reasoning: bShowing must agree, or the next 7
	// "opens" it hidden and the bar lights a panel nobody can see.
	if (bShowing)
	{
		Toggle();
	}
}

void ULandAircraftPanelWidget::Toggle()
{
	bShowing = !bShowing;
	SetShown(bShowing);

	// JUDGED ON OPEN, not left to the next tick - a panel that appeared empty for a frame and
	// then filled reads as a bug, the ledger's reasoning.
	if (bShowing)
	{
		Refresh();
	}
}

void ULandAircraftPanelWidget::Refresh()
{
	RefreshFor(Controller(), OpsRuntime());
}

void ULandAircraftPanelWidget::RefreshFor(const ARoadBuildController* C, const UOpsRuntime* Runtime)
{
	// READ ONCE, on first open. Types are content: they do not appear mid-session, and a
	// registry walk per tick would be the one expensive thing on this panel.
	if (Types.Num() == 0)
	{
		for (UAircraftType* Type : TypeSource ? TypeSource() : LandChoices::EveryMeshedType())
		{
			Types.Add(Type);
		}
	}

	const ARoadNetworkActor* Target = C != nullptr ? C->GetTarget() : nullptr;
	const URoadNetwork* Network = Target != nullptr ? Target->Network.Get() : nullptr;
	const FVector2D Focus = C != nullptr ? C->GetViewFocus() : FVector2D::ZeroVector;

	// QUOTED ONLY WHEN WHAT A QUOTE READS HAS MOVED (ops batch 3 PR E) - see JudgedKey and FLandChoicesKey. Each row is a
	// whole arrival plan since #432, so this is what keeps a still panel from planning at all.
	const UGroundTraffic* Traffic = Target != nullptr ? Target->GetGroundTraffic() : nullptr;
	const bool bAdmits = Runtime == nullptr || Runtime->GetAirport()->AdmitsArrivals();
	const FLandChoicesKey Key = LandChoices::KeyFor(Network, Traffic, Focus, bAdmits, Runtime != nullptr);
	if (bJudged && Key == JudgedKey && Types.Num() == JudgedTypeCount)
	{
		return;
	}
	JudgedKey = Key;
	JudgedTypeCount = Types.Num();
	bJudged = true;

	TArray<UAircraftType*> Raw;
	Raw.Reserve(Types.Num());
	for (const TObjectPtr<UAircraftType>& Type : Types)
	{
		Raw.Add(Type.Get());
	}
	++BuildCalls;
	// THE GAME'S VERDICT PER TYPE (#432): UOpsRuntime::QuoteLanding - the plan and the airport's gate TryAccept asks - at
	// the view focus, the point the click lands at. NO RUNTIME, NOTHING LANDS: the land path is the flight board's, and
	// without a board there is no landing to offer (the board-less fallback that used to take it went with #431).
	const TArray<FLandChoice> Choices = LandChoices::Build(Raw, [Runtime, &Focus](const FAirframe& Airframe)
	{
		if (Runtime != nullptr)
		{
			return Runtime->QuoteLanding(Airframe, Focus);
		}
		FArrivalQuote NoGame;
		NoGame.Why = EArrivalRefusal::NoRunway;
		NoGame.Sentence = TEXT("No game running - landing needs the flight board.");
		return NoGame;
	});

	// THE GATE - see PaintedRefusals.
	TArray<FString> Refusals;
	Refusals.Reserve(Choices.Num());
	for (const FLandChoice& Choice : Choices)
	{
		Refusals.Add(Choice.Refusal);
	}
	if (Refusals != PaintedRefusals || Entries.Num() != Choices.Num())
	{
		PaintRows(Choices);
		PaintedRefusals = MoveTemp(Refusals);
	}
}

void ULandAircraftPanelWidget::PaintRows(const TArray<FLandChoice>& Choices)
{
	if (RowColumn == nullptr || PanelStyle == nullptr)
	{
		return;
	}
	const UUIStyle& Style = *PanelStyle;

	// REBUILT WHOLE, as the ledger's rows are: when the runway changes, most rows' state
	// changes with it, so a diff would touch nearly everything with an index invariant to
	// get wrong.
	RowColumn->ClearChildren();
	Entries.Reset();

	for (const FLandChoice& Choice : Choices)
	{
		ULandRowEntry* Entry = NewObject<ULandRowEntry>(this);
		Entry->Type = Choice.Type;
		Entry->Owner = this;

		UUiButton* Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		// The airframe's name at a fixed width, then why it would be refused - so the names line
		// up in a column. UUiButton::Build carries the rounded-white recipe
		// UOfferInboxWidget::MakeAnswerButton used to type by hand.
		Button->SetLabel(Choice.Label);
		Button->SetLabelMinWidth(NameWidth);
		if (!Choice.Refusal.IsEmpty())
		{
			Button->SetDetail(FText::FromString(Choice.Refusal));
		}
		Button->Build(Style, EUiButtonKind::Secondary);
		// DISABLED, not merely tinted: a greyed row is a click the arrival would refuse, so
		// it must not be clickable at all. SetState both disables and dims its ink.
		Button->SetState(Choice.bAdmitted, false);
		Button->OnClicked.AddDynamic(Entry, &ULandRowEntry::HandleClick);
		Entry->Button = Button;

		UVerticalBoxSlot* RowSlot = RowColumn->AddChildToVerticalBox(Button);
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
		Entries.Add(Entry);
	}
}

void ULandAircraftPanelWidget::Choose(UAircraftType* Type)
{
	if (Type == nullptr)
	{
		return;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Land panel: %s chosen"), *Type->GetName());
	if (ARoadBuildController* C = Controller())
	{
		ChooseFor(*C, Type);
	}
}

void ULandAircraftPanelWidget::ChooseFor(ARoadBuildController& C, UAircraftType* Type)
{
	C.LandAircraftNearViewFocus(Type);
}

bool ULandAircraftPanelWidget::IsRowEnabledForTest(int32 Index) const
{
	return Entries.IsValidIndex(Index) && Entries[Index] != nullptr && Entries[Index]->Button != nullptr
		&& Entries[Index]->Button->GetIsEnabled();
}

int32 ULandAircraftPanelWidget::RowWidgetCountForTest() const
{
	return RowColumn != nullptr ? RowColumn->GetChildrenCount() : 0;
}

int32 ULandAircraftPanelWidget::RowIndexOfForTest(const TCHAR* AssetName) const
{
	return Entries.IndexOfByPredicate([AssetName](const TObjectPtr<ULandRowEntry>& Entry)
	{
		return Entry != nullptr && Entry->Type != nullptr && Entry->Type->GetName() == AssetName;
	});
}

bool ULandAircraftPanelWidget::IsRowBoundForTest(int32 Index) const
{
	return Entries.IsValidIndex(Index) && Entries[Index] != nullptr && Entries[Index]->Button != nullptr
		&& Entries[Index]->Button->OnClicked.Contains(Entries[Index].Get(),
			GET_FUNCTION_NAME_CHECKED(ULandRowEntry, HandleClick));
}

void ULandAircraftPanelWidget::ClickRowForTest(int32 Index, ARoadBuildController& C)
{
	if (Entries.IsValidIndex(Index) && Entries[Index] != nullptr)
	{
		ChooseFor(C, Entries[Index]->Type);
	}
}

void ULandAircraftPanelWidget::TickPanel(float InDeltaTime)
{

	// ONLY WHILE OPEN - a closed panel asks nothing.
	if (bShowing)
	{
		Refresh();
	}
}

#undef LOCTEXT_NAMESPACE
