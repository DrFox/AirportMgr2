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
#include "UI/UiRow.h"
#include "UIStyle.h"

// Its own category, and its own NAME: the module is a unity build, and two
// DEFINE_LOG_CATEGORY_STATIC of one name compile alone and collide together.
DEFINE_LOG_CATEGORY_STATIC(LogArrivalsPanel, Log, All);

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

void UArrivalsPanelWidget::Refresh()
{
	UOpsRuntime* Runtime = OpsRuntime();
	if (Arrivals == nullptr || Runtime == nullptr || Runtime->GetFlightBoard() == nullptr || Runtime->GetClock() == nullptr)
	{
		// The editor mode has no game instance and so no runtime - a play-mode panel, like the inbox.
		return;
	}
	Arrivals->Refresh(*Runtime->GetFlightBoard(), *Runtime->GetClock());
	if (PanelStyle != nullptr)
	{
		PaintRows(*PanelStyle);
	}
}

void UArrivalsPanelWidget::PaintRows(const UUIStyle& Style)
{
	if (Arrivals == nullptr || ArrivalColumn == nullptr)
	{
		return;
	}
	const TArray<UArrivalRowViewModel*> Rows = Arrivals->GetRows();
	SetWindowBadge(Rows.Num() == 0 ? NSLOCTEXT("AirportMgr", "ArrivalsNone", "none") : FText::AsNumber(Rows.Num()));

	// REBUILT WHEN THE COUNT CHANGES, like the offer cards: a rebuild every frame would churn the
	// widget tree for text that only moves by the minute.
	if (Titles.Num() != Rows.Num())
	{
		ArrivalColumn->ClearChildren();
		Titles.Reset();
		Statuses.Reset();
		Details.Reset();
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
			Lines->AddChildToVerticalBox(Head)->SetHorizontalAlignment(HAlign_Fill);

			UTextBlock* Detail = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Style.ApplyText(*Detail, EUITextRole::Body, Style.InkMuted);
			Lines->AddChildToVerticalBox(Detail);

			UVerticalBoxSlot* CardSlot = ArrivalColumn->AddChildToVerticalBox(Card);
			CardSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : 4.0f, 0.0f, 0.0f));
			Titles.Add(Title);
			Statuses.Add(Status);
			Details.Add(Detail);
		}
	}

	for (int32 Index = 0; Index < Rows.Num() && Index < Titles.Num(); ++Index)
	{
		const UArrivalRowViewModel* Row = Rows[Index];
		if (Row == nullptr)
		{
			continue;
		}
		Titles[Index]->SetText(Row->GetTitle());
		Statuses[Index]->SetText(Row->GetStatus());
		// HOLDING IN ACCENT: the one state the player can do something about (a free runway).
		Statuses[Index]->SetColorAndOpacity(FSlateColor(Row->GetStatus().EqualTo(
			NSLOCTEXT("AirportMgr", "ArrivalHolding", "HOLDING")) ? Style.Accent : Style.InkMuted));
		Details[Index]->SetText(Row->GetDetail());
		Details[Index]->SetColorAndOpacity(FSlateColor(Row->IsLate() ? Style.Warning : Style.InkMuted));
		Details[Index]->SetVisibility(Row->GetDetail().IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
}
