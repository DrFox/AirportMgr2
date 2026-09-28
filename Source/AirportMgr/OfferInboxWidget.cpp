#include "OfferInboxWidget.h"

#include "AirportMgr.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "Components/Spacer.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/SlateBrush.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "OfferViewModels.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "UI/UiButton.h"
#include "UI/UiRow.h"
#include "UI/UiButton.h"
#include "UI/UiRow.h"
#include "UIStyle.h"

// Its own category, and its own NAME: the module is a unity build, and two
// DEFINE_LOG_CATEGORY_STATIC of one name compile alone and collide together.
DEFINE_LOG_CATEGORY_STATIC(LogOfferInbox, Log, All);

void UOfferRowEntry::HandleAccept()
{
	if (UOfferInboxWidget* Widget = Owner.Get())
	{
		Widget->AcceptRow(RowIndex);
	}
}

void UOfferRowEntry::HandleDecline()
{
	if (UOfferInboxWidget* Widget = Owner.Get())
	{
		Widget->DeclineRow(RowIndex);
	}
}

void UOfferInboxWidget::BuildOnce(const UUIStyle& Style)
{
	Inbox = NewObject<UOfferInboxViewModel>(this);
	EnsureSlots(&Style);

	// SelfHitTestInvisible, not Collapsed: see UAirportMgrPanelWidget::BuildOnce for why an
	// otherwise-empty panel must stay this way rather than Collapsed.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	// SHOWN FROM THE START, and not closable (WantsWindow): an offer must never be the thing that
	// hides - missing one costs money (TopOffset's comment).
	SetShown(true);
}

bool UOfferInboxWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("offers");
	Out.Title = NSLOCTEXT("AirportMgr", "InboxWindow", "Offers");
	Out.bClosable = false;
	// Nor resizable: a window shrunk below its offers would scroll one out of sight, and an offer
	// must never be the thing that hides (TopOffset's comment). It grows with its offers instead.
	Out.bResizable = false;
	Out.Anchor = EUiWindowAnchor::TopRight;
	Out.Offset = FVector2D(12.0, TopOffset);
	return true;
}

void UOfferInboxWidget::EnsureSlots(const UUIStyle* Style)
{
	// Code-built chrome only where the asset gave none - the same rule as the bar and the
	// inspector. A TOP-right card: a title with a count, then one row per offer. Top, not
	// bottom, because the feed owns the bottom-right corner now and the two-row bar is tall
	// enough to have swallowed the old placement (spec section 6.2).
	//
	// Its window is Surface, so the Well-coloured offer rows inside it have something to sit ON. A
	// flat Panel here once made the container and its rows one surface, and the offers read as
	// lines of text in a box rather than as things awaiting an answer (#90).
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureContentRoot(TEXT("InboxCard"))))
	{
		// HEADING AND COUNT ON ONE LINE. The count used to be FText::AsNumber on a line of
		// its OWN directly under the word OFFERS - a bare "1" floating in the card, which is
		// what a debug readout looks like rather than a panel heading.
		UHorizontalBox* HeadingRow = WidgetTree->ConstructWidget<UHorizontalBox>(
			UHorizontalBox::StaticClass(), TEXT("InboxHeading"));

		TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InboxTitle"));
		TitleText->SetText(NSLOCTEXT("AirportMgr", "InboxTitle", "OFFERS"));
		// The same heading treatment the bar's sections take - see UUIStyle::ApplyText (#89).
		Style->ApplyText(*TitleText, EUITextRole::Heading, Style->InkMuted);
		// The window's title bar says "Offers" now; the count stays on this row.
		TitleText->SetVisibility(ESlateVisibility::Collapsed);
		HeadingRow->AddChildToHorizontalBox(TitleText)->SetVerticalAlignment(VAlign_Center);

		UHorizontalBoxSlot* HeadGap = HeadingRow->AddChildToHorizontalBox(
			WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
		HeadGap->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		BadgeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InboxBadge"));
		Style->ApplyText(*BadgeText, EUITextRole::Label, Style->InkMuted);
		UHorizontalBoxSlot* BadgeSlot = HeadingRow->AddChildToHorizontalBox(BadgeText);
		BadgeSlot->SetPadding(FMargin(16.0f, 0.0f, 0.0f, 0.0f));
		BadgeSlot->SetVerticalAlignment(VAlign_Center);

		Column->AddChildToVerticalBox(HeadingRow)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		OfferColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InboxRows"));
		Column->AddChildToVerticalBox(OfferColumn);

		UE_LOG(LogOfferInbox, Log, TEXT("No inbox asset: building the code-only panel"));
	}
}

void UOfferInboxWidget::TickPanel(float InDeltaTime)
{

	// The target comes from the controller, not a fresh TActorIterator scan: this widget
	// only ever hangs off BuildHudLayer, which only ever exists on ARoadBuildController, so
	// the controller's own Target (found once in BeginPlay) is the same actor a scan would
	// find here - see #104. Re-read every tick rather than cached, because
	// URoadEditFacade::ClearNetwork replaces the network object and a cached pointer would
	// go stale.
	if (const ARoadBuildController* C = Controller())
	{
		Refresh(C->GetTarget());
	}
}

void UOfferInboxWidget::Refresh(ARoadNetworkActor* Target)
{
	if (Inbox == nullptr || Target == nullptr || Target->Network == nullptr)
	{
		return;
	}

	UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	UGroundTraffic* Traffic = Target->GetGroundTraffic();
	if (Runtime == nullptr || Traffic == nullptr || Runtime->GetFlightBoard() == nullptr
		|| Runtime->GetClock() == nullptr)
	{
		// The editor mode has no game instance and so no runtime. Nothing to show, and
		// nothing wrong: the inbox is a play-mode panel.
		return;
	}

	Inbox->Refresh(*Runtime->GetFlightBoard(), *Traffic, *Target->Network, *Runtime->GetClock());
	PaintRows();
}

void UOfferInboxWidget::PaintRows()
{
	const TArray<UOfferViewModel*>& Rows = Inbox->GetOffers();

	if (BadgeText != nullptr)
	{
		// "none" rather than "0": the player is being told a STATE, and a zero is a value. And
		// "1 waiting" rather than "1": under the window's "Offers" title the count sits on a line of
		// its own, and a bare number there is the debug-readout look the heading row's comment
		// forbids (final review 2026-09-28).
		const int32 Pending = Inbox->GetPendingCount();
		BadgeText->SetText(Pending == 0
			? NSLOCTEXT("AirportMgr", "InboxNone", "none")
			: FText::Format(NSLOCTEXT("AirportMgr", "InboxWaiting", "{0} waiting"), FText::AsNumber(Pending)));
	}

	// The Blueprint path: UListView::SetListItems (core UMG, not ModelViewViewModel - issue
	// #191 dropped that dependency, since nothing used it) hands each entry widget its own
	// UOfferViewModel through IUserObjectListEntry; the entry widget's Blueprint graph reads
	// its getters the same way PaintRows does below for the code-built path.
	if (OfferList != nullptr)
	{
		OfferList->SetListItems(Rows);
		return;
	}

	if (OfferColumn == nullptr)
	{
		return;
	}

	// PanelStyle is the base class's (issue #187) - resolved once in Initialize, before
	// BuildOnce ever ran. This used to call UAirportMgrUISettings::ResolveStyle() (a
	// TSoftObjectPtr::LoadSynchronous) itself, every tick NativeTick calls Refresh, which is
	// every tick outright - issue #309, the regression #187 did not reach because this file
	// was not one of the two it named.
	const UUIStyle* Style = PanelStyle;

	// The code-only path. Rebuilt when the COUNT changes rather than every tick: a rebuild
	// every frame would drop a half-pressed button and churn the widget tree.
	if (Entries.Num() != Rows.Num())
	{
		OfferColumn->ClearChildren();
		Entries.Reset();

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			UOfferRowEntry* Entry = NewObject<UOfferRowEntry>(this);
			Entry->RowIndex = Index;
			Entry->Owner = this;
			Entries.Add(Entry);

			// A GAP BETWEEN CARDS, or the rounded corners meet and two offers read as one.
			UVerticalBoxSlot* CardSlot =
				OfferColumn->AddChildToVerticalBox(BuildRow(*Style, *Entry, Index));
			if (CardSlot != nullptr)
			{
				CardSlot->SetPadding(FMargin(0.0f, Index == 0 ? 0.0f : RowGap, 0.0f, 0.0f));
			}
		}
	}

	// Text and enabled state are repainted every refresh, because the ETA counts down and a
	// stand freeing makes a greyed-out offer acceptable again without the count changing.
	for (int32 Index = 0; Index < Rows.Num() && Index < Entries.Num(); ++Index)
	{
		const UOfferViewModel* Row = Rows[Index];
		UOfferRowEntry* Entry = Entries[Index];
		if (Row == nullptr || Entry == nullptr)
		{
			continue;
		}

		// EACH FIELD IN ITS OWN WIDGET. These four used to be one FText::Format joined by
		// double spaces - "Cumbria Air  PA-46-500TP Meridian  in 9 min  " - which gave the
		// airline, the airframe and the countdown identical weight and read as a log line
		// rather than as something with an answer expected.
		if (Entry->AirlineText != nullptr) { Entry->AirlineText->SetText(Row->GetAirline()); }
		if (Entry->TypeText != nullptr)    { Entry->TypeText->SetText(Row->GetTypeName()); }
		if (Entry->EtaText != nullptr)     { Entry->EtaText->SetText(Row->GetEta()); }

		if (Entry->RefusalText != nullptr)
		{
			// COLLAPSED, not blanked: an empty text block still takes its line height, so a
			// card would change height when a stand freed and jog the whole stack.
			const bool bShow = !Row->IsAcceptable() && !Row->GetRefusal().IsEmpty();
			Entry->RefusalText->SetVisibility(
				bShow ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
			if (bShow)
			{
				Entry->RefusalText->SetText(Row->GetRefusal());
			}
		}

		if (Entry->AcceptButton != nullptr)
		{
			// DISABLED, not hidden: the player needs to see the offer and the reason it
			// cannot be taken, which is what tells them to build another stand.
			//
			// ACCEPT IS THE ONE THING ON THIS CARD THAT TAKES ACCENT. The bar spends that
			// colour on the armed tool and nothing else; here it is the affirmative verb,
			// and the two never share a screen region. The Primary KIND carries that now, and
			// UUiButton::LookFor dims a disabled Primary to Control rather than leave it shouting.
			Entry->AcceptButton->SetState(Row->IsAcceptable(), false);
		}
	}
}

UWidget* UOfferInboxWidget::BuildRow(const UUIStyle& Style, UOfferRowEntry& Entry, int32 Index)
{
	// ONE ROW PER OFFER, a Well on the window's Surface, so it reads as a thing that can be
	// answered rather than as a line of text - UUiRow owns that choice now (CornerRadius once
	// sat in the asset unread while everything drew square).
	UUiRow* Card = WidgetTree->ConstructWidget<UUiRow>(
		UUiRow::StaticClass(), *FString::Printf(TEXT("OfferCard%d"), Index));
	Card->Build(Style, FMargin(10.0f, 8.0f));

	UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Card->SetContent(Lines);

	// LINE ONE: who is asking, and how long is left. The countdown sits hard right because it
	// is the field that MOVES, and a moving number is easier to read in a column of its own
	// than buried mid-sentence.
	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Entry.AirlineText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.AirlineText, EUITextRole::Title, Style.Ink);
	Head->AddChildToHorizontalBox(Entry.AirlineText)->SetVerticalAlignment(VAlign_Center);

	UHorizontalBoxSlot* GapSlot = Head->AddChildToHorizontalBox(
		WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
	GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	Entry.EtaText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.EtaText, EUITextRole::Label, Style.InkMuted);
	UHorizontalBoxSlot* EtaSlot = Head->AddChildToHorizontalBox(Entry.EtaText);
	EtaSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	EtaSlot->SetVerticalAlignment(VAlign_Center);
	Lines->AddChildToVerticalBox(Head)->SetHorizontalAlignment(HAlign_Fill);

	// LINE TWO: the airframe, quieter. It matters while deciding, not while scanning.
	Entry.TypeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.TypeText, EUITextRole::Body, Style.InkMuted);
	Lines->AddChildToVerticalBox(Entry.TypeText);

	// LINE THREE: why it cannot be taken, in Warning and wrapped. Hidden while acceptable -
	// see the Collapsed comment in the repaint above.
	Entry.RefusalText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.RefusalText, EUITextRole::Body, Style.Warning);
	Entry.RefusalText->SetAutoWrapText(true);
	Entry.RefusalText->SetWrapTextAt(RowWrapWidth);
	Entry.RefusalText->SetVisibility(ESlateVisibility::Collapsed);
	Lines->AddChildToVerticalBox(Entry.RefusalText)->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));

	// LINE FOUR: the two answers, right-aligned beneath what they answer.
	UHorizontalBox* Answers = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	UHorizontalBoxSlot* PushSlot = Answers->AddChildToHorizontalBox(
		WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
	PushSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	Entry.AcceptButton = MakeAnswerButton(Style, TEXT("Accept"),
		NSLOCTEXT("AirportMgr", "OfferAccept", "Accept"), EUiButtonKind::Primary, Index);
	Entry.AcceptButton->OnClicked.AddDynamic(&Entry, &UOfferRowEntry::HandleAccept);
	Answers->AddChildToHorizontalBox(Entry.AcceptButton)->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));

	UUiButton* DeclineButton = MakeAnswerButton(Style, TEXT("Decline"),
		NSLOCTEXT("AirportMgr", "OfferDecline", "Decline"), EUiButtonKind::Secondary, Index);
	DeclineButton->OnClicked.AddDynamic(&Entry, &UOfferRowEntry::HandleDecline);
	Answers->AddChildToHorizontalBox(DeclineButton);

	Lines->AddChildToVerticalBox(Answers)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	return Card;
}

UUiButton* UOfferInboxWidget::MakeAnswerButton(const UUIStyle& Style, const TCHAR* Name,
	const FText& Label, EUiButtonKind Kind, int32 Index)
{
	UUiButton* Button = WidgetTree->ConstructWidget<UUiButton>(
		UUiButton::StaticClass(), *FString::Printf(TEXT("Offer%s%d"), Name, Index));

	// ROUNDED THROUGH THE BUTTON STYLE, not through a background tint: UButton draws its own
	// FButtonStyle brushes, which were the engine's flat default box and read as stock editor
	// buttons. That recipe (white brushes, ButtonPadding - issue #192) lives in UUiButton::Build
	// now, and the kind picks the state colour through UUiButton::LookFor.
	Button->SetLabel(Label);
	Button->Build(Style, Kind);
	return Button;
}

FString UOfferInboxWidget::BadgeForTest() const
{
	return BadgeText != nullptr ? BadgeText->GetText().ToString() : FString();
}

const UUiButton* UOfferInboxWidget::AcceptButtonForTest(int32 Row) const
{
	return Entries.IsValidIndex(Row) && Entries[Row] != nullptr ? Entries[Row]->AcceptButton.Get() : nullptr;
}

int32 UOfferInboxWidget::RowWidgetCountForTest() const
{
	return OfferColumn != nullptr ? OfferColumn->GetChildrenCount() : 0;
}

void UOfferInboxWidget::AcceptRow(int32 RowIndex)
{
	if (Inbox == nullptr || !Inbox->GetOffers().IsValidIndex(RowIndex))
	{
		return;
	}
	Inbox->Accept(Inbox->GetOffers()[RowIndex]);
}

void UOfferInboxWidget::DeclineRow(int32 RowIndex)
{
	if (Inbox == nullptr || !Inbox->GetOffers().IsValidIndex(RowIndex))
	{
		return;
	}
	Inbox->Decline(Inbox->GetOffers()[RowIndex]);
}
