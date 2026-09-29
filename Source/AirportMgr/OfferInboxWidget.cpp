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
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/Spacer.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/SlateBrush.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OfferGenerator.h"
#include "Model/SimClock.h"
#include "ArrivalViewModels.h"
#include "OfferViewModels.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
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
	Arrivals = NewObject<UArrivalsViewModel>(this);
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

		Column->AddChildToVerticalBox(HeadingRow)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 6.0f));

		// THE DEMAND STRIP, under the heading: the day's demand an hour a bar, night shaded,
		// now in accent. What makes the morning peak something to build FOR rather than a
		// surprise (spec 2026-09-28 section 4). A fixed-height box, so the card does not jog
		// when the bars change height.
		USizeBox* StripBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("DemandStrip"));
		StripBox->SetHeightOverride(DemandStripHeight);
		UHorizontalBox* Strip = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		StripBox->SetContent(Strip);
		for (int32 Hour = 0; Hour < 24; ++Hour)
		{
			USizeBox* Bar = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			Bar->SetWidthOverride(8.0f);
			Bar->SetHeightOverride(1.0f);
			UBorder* Fill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Fill->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, 1.0f));
			Bar->SetContent(Fill);
			UHorizontalBoxSlot* BarSlot = Strip->AddChildToHorizontalBox(Bar);
			BarSlot->SetVerticalAlignment(VAlign_Bottom);
			BarSlot->SetPadding(FMargin(0.0f, 0.0f, 2.0f, 0.0f));
			DemandBars.Add(Bar);
			DemandFills.Add(Fill);
		}
		Column->AddChildToVerticalBox(StripBox)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		OfferColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InboxRows"));
		Column->AddChildToVerticalBox(OfferColumn);

		// ARRIVALS, UNDER THE OFFERS, in the same card (spec 2026-09-28-arrival-queue): what is
		// coming is one place to look - the offers you might take, then the flights you did.
		UHorizontalBox* ArrivalHeading = WidgetTree->ConstructWidget<UHorizontalBox>(
			UHorizontalBox::StaticClass(), TEXT("ArrivalsHeading"));
		UTextBlock* ArrivalTitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ArrivalsTitle"));
		ArrivalTitle->SetText(NSLOCTEXT("AirportMgr", "ArrivalsTitle", "ARRIVALS"));
		Style->ApplyText(*ArrivalTitle, EUITextRole::Heading, Style->InkMuted);
		ArrivalHeading->AddChildToHorizontalBox(ArrivalTitle)->SetVerticalAlignment(VAlign_Center);
		UHorizontalBoxSlot* ArrivalGap = ArrivalHeading->AddChildToHorizontalBox(
			WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
		ArrivalGap->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ArrivalCountText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ArrivalsCount"));
		Style->ApplyText(*ArrivalCountText, EUITextRole::Label, Style->InkMuted);
		ArrivalHeading->AddChildToHorizontalBox(ArrivalCountText)->SetVerticalAlignment(VAlign_Center);
		Column->AddChildToVerticalBox(ArrivalHeading)->SetPadding(FMargin(0.0f, 12.0f, 0.0f, 6.0f));

		ArrivalColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ArrivalRows"));
		Column->AddChildToVerticalBox(ArrivalColumn);

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

	Inbox->Refresh(*Runtime->GetFlightBoard(), *Traffic, *Target->Network, *Runtime->GetClock(), Runtime->GetAirlines());
	Arrivals->Refresh(*Runtime->GetFlightBoard(), *Runtime->GetClock());

	// THE STRIP'S SAMPLES, from the runtime's own airline list and the live fee - the same
	// inputs UOfferGenerator::TickMinute reads, through the same TotalRateAt.
	const USimClock& Clock = *Runtime->GetClock();
	const UOfferGenerator* Generator = Runtime->GetOfferGenerator();
	const double Factor = Generator != nullptr ? Generator->DemandFactor() : 1.0;
	// AND EACH AIRLINE'S SATISFACTION, through the generator's own reader - see UOfferGenerator::AirlineFactor.
	DemandSamples = UOfferInboxViewModel::SampleDemand(Runtime->GetAirlineOffers(), Clock, Factor, 24,
		[Generator](const UAirlineDefinition& Airline) { return Generator != nullptr ? Generator->AirlineFactor(Airline) : 1.0; });
	DemandNight.SetNum(24);
	for (int32 Hour = 0; Hour < 24; ++Hour)
	{
		DemandNight[Hour] = !Clock.IsDaylight((Hour + 0.5) * 3600.0);
	}
	DemandNowSlot = FMath::Clamp(FMath::FloorToInt32(Clock.TimeOfDay() / 3600.0), 0, 23);
	PaintRows();
}

void UOfferInboxWidget::PaintRows()
{
	const TArray<UOfferViewModel*>& Rows = Inbox->GetOffers();

	if (BadgeText != nullptr)
	{
		// AGAINST THE CAP when there is one - "3/8" says how close the inbox is to turning
		// offers away (spec ruling 6). Without one, "none" rather than "0" - the player is being
		// told a STATE, and a zero is a value - and "1 waiting" rather than "1": under the window's
		// "Offers" title a bare number is the debug-readout look the heading row's comment forbids
		// (UI library final review 2026-09-28).
		const int32 Pending = Inbox->GetPendingCount();
		const int32 Capacity = Inbox->GetCapacity();
		BadgeText->SetText(Capacity > 0
			? FText::Format(NSLOCTEXT("AirportMgr", "InboxOfCap", "{0}/{1}"), FText::AsNumber(Pending), FText::AsNumber(Capacity))
			: Pending == 0 ? NSLOCTEXT("AirportMgr", "InboxNone", "none")
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
	if (Style != nullptr)
	{
		PaintDemand(*Style);
		PaintArrivals(*Style);
	}

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
		if (Entry->AirlineText != nullptr)
		{
			const FText Who = Row->GetCallsign().IsEmpty() ? Row->GetAirline()
				: FText::Format(NSLOCTEXT("AirportMgr", "OfferWho", "{0}  {1}"), Row->GetCallsign(), Row->GetAirline());
			// HOW THE AIRLINE FEELS, beside its name - the one place the player sees that answering its
			// offers late or not at all has a cost (spec 2026-09-29-ops-event-bus section 3).
			Entry->AirlineText->SetText(Row->GetSatisfaction().IsEmpty() ? Who
				: FText::Format(NSLOCTEXT("AirportMgr", "OfferWhoMood", "{0}  \u00B7  {1}"), Who, Row->GetSatisfaction()));
		}
		if (Entry->TypeText != nullptr)
		{
			Entry->TypeText->SetText(Row->GetFee().IsEmpty() ? Row->GetTypeName()
				: FText::Format(NSLOCTEXT("AirportMgr", "OfferWhat", "{0}  \u00B7  {1}"), Row->GetTypeName(), Row->GetFee()));
		}
		if (Entry->ContractText != nullptr) { Entry->ContractText->SetText(Row->GetContract()); }

		// THE COUNTDOWN: seconds and a draining bar, amber then red-and-pulsing as it runs out,
		// because an offer that lapses unseen is money the player never knew they lost.
		const int32 Left = Row->GetSecondsLeft();
		const bool bUrgent = Left <= CountdownUrgentSeconds;
		FLinearColor TimeColour = bUrgent ? Style->Warning : Left <= CountdownAmberSeconds ? Style->Accent : Style->InkMuted;
		if (bUrgent)
		{
			TimeColour.A = 0.6f + 0.4f * FMath::Abs(FMath::Sin(static_cast<float>(FPlatformTime::Seconds()) * 4.0f));
		}
		if (Entry->CountdownText != nullptr)
		{
			Entry->CountdownText->SetText(FText::Format(NSLOCTEXT("AirportMgr", "OfferSecondsLeft", "{0} s"), FText::AsNumber(Left)));
			Entry->CountdownText->SetColorAndOpacity(FSlateColor(TimeColour));
		}
		if (Entry->CountdownBar != nullptr)
		{
			Entry->CountdownBar->SetPercent(Row->GetTimeLeftFraction());
			Entry->CountdownBar->SetFillColorAndOpacity(TimeColour);
		}
		if (Entry->FuelChip != nullptr)
		{
			// LITRES AND WHETHER THEY CAN BE GIVEN; hidden for a flight that wants none.
			Entry->FuelChip->SetVisibility(Row->GetFuelText().IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
			Entry->FuelChip->SetText(FText::Format(NSLOCTEXT("AirportMgr", "OfferFuelChip", "{0} {1}"),
				Row->GetFuelText(), Row->IsFuelServable()
					? FText::FromString(TEXT("\u2713")) : FText::FromString(TEXT("\u2717"))));
			Entry->FuelChip->SetColorAndOpacity(FSlateColor(Row->IsFuelServable() ? Style->Positive : Style->Warning));
		}
		if (Entry->TugChip != nullptr)
		{
			Entry->TugChip->SetVisibility(Row->NeedsTug() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
		// "Accept (no fuel)" - the one caption on the card that changes. Through the button's own
		// label (UUiButton::GetLabel), gated, since SetText has no early-out.
		if (Entry->AcceptButton != nullptr)
		{
			const UTextBlock* Caption = Entry->AcceptButton->GetLabel();
			if (Caption == nullptr || !Caption->GetText().EqualTo(Row->GetAcceptLabel()))
			{
				Entry->AcceptButton->SetLabel(Row->GetAcceptLabel());
			}
		}

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

	Entry.CountdownText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.CountdownText, EUITextRole::Label, Style.InkMuted);
	UHorizontalBoxSlot* CountdownSlot = Head->AddChildToHorizontalBox(Entry.CountdownText);
	CountdownSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	CountdownSlot->SetVerticalAlignment(VAlign_Center);
	Lines->AddChildToVerticalBox(Head)->SetHorizontalAlignment(HAlign_Fill);

	// THE BAR under the head line: how much of the window is left, at a glance across rows.
	Entry.CountdownBar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass());
	{
		FProgressBarStyle BarStyle = Entry.CountdownBar->GetWidgetStyle();
		BarStyle.SetBackgroundImage(FSlateRoundedBoxBrush(Style.Control, 1.5f));
		BarStyle.SetFillImage(FSlateRoundedBoxBrush(FLinearColor::White, 1.5f));
		Entry.CountdownBar->SetWidgetStyle(BarStyle);
	}
	USizeBox* BarBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	BarBox->SetHeightOverride(3.0f);
	BarBox->SetContent(Entry.CountdownBar);
	Lines->AddChildToVerticalBox(BarBox)->SetPadding(FMargin(0.0f, 3.0f, 0.0f, 3.0f));

	// LINE TWO: the airframe and what it pays, quieter. It matters while deciding, not while
	// scanning.
	Entry.TypeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.TypeText, EUITextRole::Body, Style.InkMuted);
	Lines->AddChildToVerticalBox(Entry.TypeText);

	// LINE THREE: the turnaround contract, in game time.
	Entry.ContractText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.ContractText, EUITextRole::Body, Style.InkMuted);
	Lines->AddChildToVerticalBox(Entry.ContractText);

	// LINE FOUR: what it wants on the ground. Fuel is live (can the airport give it?); the tug
	// is information only until a pushback service exists to ask.
	UHorizontalBox* Chips = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Entry.FuelChip = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.FuelChip, EUITextRole::Label, Style.Positive);
	Chips->AddChildToHorizontalBox(Entry.FuelChip)->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));
	Entry.TugChip = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Entry.TugChip->SetText(NSLOCTEXT("AirportMgr", "OfferTug", "Needs tug"));
	Style.ApplyText(*Entry.TugChip, EUITextRole::Label, Style.InkMuted);
	Chips->AddChildToHorizontalBox(Entry.TugChip);
	Lines->AddChildToVerticalBox(Chips)->SetPadding(FMargin(0.0f, 3.0f, 0.0f, 0.0f));

	// LINE FIVE: why it cannot be taken, in Warning and wrapped. Hidden while acceptable -
	// see the Collapsed comment in the repaint above.
	Entry.RefusalText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.RefusalText, EUITextRole::Body, Style.Warning);
	Entry.RefusalText->SetAutoWrapText(true);
	Entry.RefusalText->SetWrapTextAt(RowWrapWidth);
	Entry.RefusalText->SetVisibility(ESlateVisibility::Collapsed);
	Lines->AddChildToVerticalBox(Entry.RefusalText)->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));

	// LINE SIX: the two answers, right-aligned beneath what they answer.
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

void UOfferInboxWidget::PaintArrivals(const UUIStyle& Style)
{
	if (Arrivals == nullptr || ArrivalColumn == nullptr)
	{
		return;
	}
	const TArray<UArrivalRowViewModel*> Rows = Arrivals->GetRows();
	if (ArrivalCountText != nullptr)
	{
		ArrivalCountText->SetText(Rows.Num() == 0 ? NSLOCTEXT("AirportMgr", "ArrivalsNone", "none") : FText::AsNumber(Rows.Num()));
	}

	// REBUILT WHEN THE COUNT CHANGES, like the offer cards: a rebuild every frame would churn the
	// widget tree for text that only moves by the minute.
	if (ArrivalTitles.Num() != Rows.Num())
	{
		ArrivalColumn->ClearChildren();
		ArrivalTitles.Reset();
		ArrivalStatuses.Reset();
		ArrivalDetails.Reset();
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			// A WELL ROW, the offer cards' own surface (UUiRow) - so the two lists read as one card.
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
			ArrivalTitles.Add(Title);
			ArrivalStatuses.Add(Status);
			ArrivalDetails.Add(Detail);
		}
	}

	for (int32 Index = 0; Index < Rows.Num() && Index < ArrivalTitles.Num(); ++Index)
	{
		const UArrivalRowViewModel* Row = Rows[Index];
		if (Row == nullptr)
		{
			continue;
		}
		ArrivalTitles[Index]->SetText(Row->GetTitle());
		ArrivalStatuses[Index]->SetText(Row->GetStatus());
		// HOLDING IN ACCENT: the one state the player can do something about (a free runway).
		ArrivalStatuses[Index]->SetColorAndOpacity(FSlateColor(Row->GetStatus().EqualTo(
			NSLOCTEXT("AirportMgr", "ArrivalHolding", "HOLDING")) ? Style.Accent : Style.InkMuted));
		ArrivalDetails[Index]->SetText(Row->GetDetail());
		ArrivalDetails[Index]->SetColorAndOpacity(FSlateColor(Row->IsLate() ? Style.Warning : Style.InkMuted));
		ArrivalDetails[Index]->SetVisibility(Row->GetDetail().IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	}
}

void UOfferInboxWidget::PaintDemand(const UUIStyle& Style)
{
	if (DemandBars.Num() == 0)
	{
		return;
	}
	double Peak = 0.0;
	for (const double Each : DemandSamples) { Peak = FMath::Max(Peak, Each); }
	for (int32 Hour = 0; Hour < DemandBars.Num(); ++Hour)
	{
		const double Sample = DemandSamples.IsValidIndex(Hour) ? DemandSamples[Hour] : 0.0;
		// A HAIRLINE, never nothing, for an hour with no demand - a gap reads as a missing bar.
		const float Height = Peak > 0.0 ? FMath::Max(1.0f, static_cast<float>(Sample / Peak) * DemandStripHeight) : 1.0f;
		if (DemandBars[Hour] != nullptr) { DemandBars[Hour]->SetHeightOverride(Height); }
		if (DemandFills[Hour] != nullptr)
		{
			const bool bNight = DemandNight.IsValidIndex(Hour) && DemandNight[Hour];
			DemandFills[Hour]->SetBrushColor(Hour == DemandNowSlot ? Style.Accent
				: bNight ? Style.Control : Style.InkMuted);
		}
	}
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
