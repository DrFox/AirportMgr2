#include "ArrivalsPanelWidget.h"

#include "ArrivalViewModels.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Present/OpsRuntime.h"
#include "Model/Flight.h"
#include "Model/FlightRunway.h"
#include "Model/OpsAlerts.h"
#include "Model/GroundTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "UI/UiButton.h"
#include "UI/UiRow.h"
#include "UIStyle.h"

// Its own category, and its own NAME: the module is a unity build, and two
// DEFINE_LOG_CATEGORY_STATIC of one name compile alone and collide together.
DEFINE_LOG_CATEGORY_STATIC(LogArrivalsPanel, Log, All);

void UArrivalRowEntry::HandleClick()
{
	UArrivalsPanelWidget* Panel = Owner.Get();
	ARoadBuildController* Controller = Panel != nullptr ? Cast<ARoadBuildController>(Panel->GetOwningPlayer()) : nullptr;
	const UFlight* Live = Flight.Get();
	if (Panel == nullptr || Controller == nullptr || Live == nullptr)
	{
		UE_LOG(LogArrivalsPanel, Warning, TEXT("Arrivals Inspect ignored: %s"),
			Live == nullptr ? TEXT("the flight has gone") : TEXT("no controller"));
		return;
	}
	Panel->Inspect(*Live, *Controller);
}

bool UArrivalsPanelWidget::Inspect(const UFlight& Flight, ARoadBuildController& Controller)
{
	if (Flight.AgentId == INDEX_NONE)
	{
		UE_LOG(LogArrivalsPanel, Warning, TEXT("Arrivals Inspect: %s has no aircraft yet"), *Flight.Callsign);
		return false;
	}
	// THE ROW'S OWN LINE FIRST, the inspector's Show reason: SelectAndFocus logs as "Alert Go".
	UE_LOG(LogArrivalsPanel, Log, TEXT("Arrivals Inspect: %s -> agent %d"), *Flight.Callsign, Flight.AgentId);
	FAlertFocus Focus;
	Focus.Kind = EAlertFocusKind::Agent;
	Focus.Id = Flight.AgentId;
	return Controller.SelectAndFocus(Focus);
}

bool UArrivalsPanelWidget::IsInspectShownForTest(int32 Row) const
{
	const UArrivalRowEntry* Entry = Entries.IsValidIndex(Row) ? Entries[Row].Get() : nullptr;
	return Entry != nullptr && Entry->Button != nullptr && Entry->Button->GetVisibility() != ESlateVisibility::Collapsed
		&& Entry->Button->OnClicked.Contains(Entry, GET_FUNCTION_NAME_CHECKED(UArrivalRowEntry, HandleClick));
}

const UFlight* UArrivalsPanelWidget::InspectFlightForTest(int32 Row) const
{
	return Entries.IsValidIndex(Row) && Entries[Row] != nullptr ? Entries[Row]->Flight.Get() : nullptr;
}

void UArrivalsPanelWidget::BuildOnce(const UUIStyle& Style)
{
	Arrivals = NewObject<UArrivalsViewModel>(this);
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureContentRoot(TEXT("ArrivalsCard"))))
	{
		// The heading and count live in the window's title and badge now - the count must still
		// read with the window folded, which a row inside it cannot.
		ArrivalColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ArrivalRows"));
		USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("ArrivalRowsWidth"));
		Width->SetMinDesiredWidth(MinWidth);
		Width->SetContent(ArrivalColumn);
		Column->AddChildToVerticalBox(Width);
		UE_LOG(LogArrivalsPanel, Log, TEXT("No arrivals asset: building the code-only panel"));
	}
	// SelfHitTestInvisible, not Collapsed: see UAirportMgrPanelWidget::BuildOnce.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetShown(true);
}

bool UArrivalsPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("arrivals");
	Out.Title = NSLOCTEXT("AirportMgr", "ArrivalsWindow", "Arrivals");
	Out.bClosable = false;
	Out.bCollapsible = true;
	Out.Anchor = EUiWindowAnchor::TopRight;
	Out.Offset = Offset;
	return true;
}

void UArrivalsPanelWidget::TickPanel(float DeltaTime)
{
	Refresh();
}

FLinearColor UArrivalsPanelWidget::StatusColourForTest(int32 Row) const
{
	return Statuses.IsValidIndex(Row) && Statuses[Row] != nullptr ? Statuses[Row]->GetColorAndOpacity().GetSpecifiedColor() : FLinearColor::Transparent;
}

void UArrivalsPanelWidget::Refresh()
{
	UOpsRuntime* Runtime = OpsRuntime();
	if (Arrivals == nullptr || Runtime == nullptr || Runtime->GetFlightBoard() == nullptr || Runtime->GetClock() == nullptr)
	{
		// NO RUNTIME ONLY IN A HEADLESS TEST that resolved none (OpsRuntimeResolver): this panel is built by the
		// controller's HUD layer, which exists in PIE alone, where the runtime is a game-instance subsystem that always
		// exists. Not "the editor mode" - it never builds this panel (#471; #470 corrected the controller's Land path).
		return;
	}
	// THE ROW SET AND THE COUNT, folded or not: the count must still read on a folded window's title bar. The sentences and the paint are
	// for a body somebody can see - a folded window is still ticked by the host (RunPanelTick), and used to compose and paint every row
	// of every tick for a body nobody could see (#446).
	// ENFORCED BY: AirportMgr.UI.Arrivals.FoldedPanelComposesNothing
	Arrivals->SyncRows(*Runtime->GetFlightBoard());
	if (IsFolded())
	{
		PaintBadge();
		return;
	}
	// THE RUNWAY EACH FLIGHT IS ON OR WAITS FOR (FlightRunway, 2026-10-01). No target is no runway named, not a stale one.
	const ARoadNetworkActor* Target = Runtime->GetTarget();
	const URoadNetwork* Network = Target != nullptr ? Target->GetNetwork() : nullptr;
	const UGroundTraffic* Traffic = Target != nullptr ? Target->GetGroundTraffic() : nullptr;
	const UFlightBoard& Board = *Runtime->GetFlightBoard();
	Arrivals->RefreshText(*Runtime->GetClock(), [Network, Traffic, &Board](const UFlight& Flight)
	{
		return Network != nullptr && Traffic != nullptr
			? FlightRunway::Names(*Network, FlightRunway::For(Board, *Traffic, Flight)) : FString();
	});
	if (PanelStyle != nullptr)
	{
		PaintRows(*PanelStyle);
	}
}

void UArrivalsPanelWidget::PaintBadge()
{
	if (Arrivals == nullptr || Arrivals->GetCount() == BadgedCount)
	{
		return;
	}
	BadgedCount = Arrivals->GetCount();
	SetWindowBadge(BadgedCount == 0 ? NSLOCTEXT("AirportMgr", "ArrivalsNone", "none") : FText::AsNumber(BadgedCount));
}

void UArrivalsPanelWidget::PaintRows(const UUIStyle& Style)
{
	if (Arrivals == nullptr || ArrivalColumn == nullptr)
	{
		return;
	}
	const TArray<UArrivalRowViewModel*> Rows = Arrivals->GetRows();
	PaintBadge();

	// REBUILT WHEN THE COUNT CHANGES, like the offer cards: a rebuild every frame would churn the
	// widget tree for text that only moves by the minute.
	if (Titles.Num() != Rows.Num())
	{
		ArrivalColumn->ClearChildren();
		Titles.Reset();
		Statuses.Reset();
		Details.Reset();
		PaintedRevisions.Reset();
		Entries.Reset();
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			// A WELL ROW, the offer cards' own surface (UUiRow) - so the two windows read as one family.
			UUiRow* Card = WidgetTree->ConstructWidget<UUiRow>(UUiRow::StaticClass());
			Card->Build(Style, FMargin(10.0f, 5.0f));
			UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
			Card->SetContent(Lines);

			UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Style.ApplyText(*Title, EUITextRole::Body, Style.Ink);
			Head->AddChildToHorizontalBox(Title)->SetVerticalAlignment(VAlign_Center);
			UHorizontalBoxSlot* Gap = Head->AddChildToHorizontalBox(WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
			Gap->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			UTextBlock* Status = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Style.ApplyText(*Status, EUITextRole::Label, Style.InkMuted);
			UHorizontalBoxSlot* StatusSlot = Head->AddChildToHorizontalBox(Status);
			StatusSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
			StatusSlot->SetVerticalAlignment(VAlign_Center);
			// INSPECT, at the head's end: shown only while the flight has an aircraft (PaintRows' loop below).
			UUiButton* InspectButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
			InspectButton->SetLabel(NSLOCTEXT("AirportMgr", "ArrivalsInspect", "Inspect"));
			InspectButton->Build(Style, EUiButtonKind::Secondary);
			InspectButton->SetToolTipText(NSLOCTEXT("AirportMgr", "ArrivalsInspectTip", "Select the aircraft and move the camera to it"));
			InspectButton->SetVisibility(ESlateVisibility::Collapsed);
			UArrivalRowEntry* Entry = NewObject<UArrivalRowEntry>(this);
			Entry->Owner = this;
			Entry->Button = InspectButton;
			InspectButton->OnClicked.AddDynamic(Entry, &UArrivalRowEntry::HandleClick);
			UHorizontalBoxSlot* InspectSlot = Head->AddChildToHorizontalBox(InspectButton);
			InspectSlot->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
			InspectSlot->SetVerticalAlignment(VAlign_Center);
			Lines->AddChildToVerticalBox(Head)->SetHorizontalAlignment(HAlign_Fill);

			UTextBlock* Detail = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Style.ApplyText(*Detail, EUITextRole::Body, Style.InkMuted);
			Lines->AddChildToVerticalBox(Detail);

			UVerticalBoxSlot* CardSlot = ArrivalColumn->AddChildToVerticalBox(Card);
			CardSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f));
			Titles.Add(Title);
			Statuses.Add(Status);
			Details.Add(Detail);
			Entries.Add(Entry);
			PaintedRevisions.Add(0);
		}
	}

	for (int32 Index = 0; Index < Rows.Num() && Index < Titles.Num(); ++Index)
	{
		const UArrivalRowViewModel* Row = Rows[Index];
		// A SLOT WHOSE ROW HAS NOT RECOMPOSED since it last painted has nothing new to show (the stamp is unique across rows, so a different
		// row landing in the slot repaints it). 0 is "never painted", and a row that has never composed stamps 0 too - and is skipped, correctly.
		if (Row == nullptr || PaintedRevisions[Index] == Row->GetRevision())
		{
			continue;
		}
		PaintedRevisions[Index] = Row->GetRevision();
		++RowPaints;
		Titles[Index]->SetText(Row->GetTitle());
		Statuses[Index]->SetText(Row->GetStatus());
		// HOLDING IN ACCENT: the one state the player can do something about (a free runway). The ROW says it is holding - this used to
		// compare the status's localised text against a copy of the word (#447).
		Statuses[Index]->SetColorAndOpacity(FSlateColor(Row->IsHolding() ? Style.Accent : Style.InkMuted));
		Details[Index]->SetText(Row->GetDetail());
		Details[Index]->SetColorAndOpacity(FSlateColor(Row->IsLate() ? Style.Warning : Style.InkMuted));
		Details[Index]->SetVisibility(Row->GetDetail().IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		// THE ENTRY FOLLOWS THE ROW into its slot - a stamp that moved is also a different row landing here (see the gate above).
		if (UArrivalRowEntry* Entry = Entries[Index])
		{
			Entry->Flight = Row->Flight;
			Entry->Button->SetVisibility(Row->HasAircraft() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	}
}
