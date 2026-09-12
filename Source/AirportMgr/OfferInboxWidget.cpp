#include "OfferInboxWidget.h"

#include "AirportMgr.h"
#include "Blueprint/WidgetTree.h"
#include "EngineUtils.h"
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

void UOfferInboxWidget::BuildOnce(const UUIStyle&)
{
	Inbox = NewObject<UOfferInboxViewModel>(this);
	EnsureSlots();

	// THE ROOT IS NEVER COLLAPSED, for the reason UInspectorWidget's own comment gives: Slate
	// ticks a widget from its paint pass, so a collapsed widget never ticks, and the tick is
	// the only thing that would un-collapse it.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UOfferInboxWidget::EnsureSlots()
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();   // never null

	// Code-built chrome only where the asset gave none - the same rule as the bar and the
	// inspector. A TOP-right card: a title with a count, then one row per offer. Top, not
	// bottom, because the feed owns the bottom-right corner now and the two-row bar is tall
	// enough to have swallowed the old placement (spec section 6.2).
	//
	// PanelDark, so the Panel-coloured offer cards inside it have something to sit ON. A flat
	// Panel here made the container and its rows one surface, and the offers read as lines of
	// text in a box rather than as things awaiting an answer - see EnsureCardRoot (#90).
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureCardRoot(TEXT("InboxCard"),
		FAnchors(1.0f, 0.0f, 1.0f, 0.0f), FVector2D(1.0, 0.0), FVector2D(-12.0, TopOffset), true)))
	{
		// HEADING AND COUNT ON ONE LINE. The count used to be FText::AsNumber on a line of
		// its OWN directly under the word OFFERS - a bare "1" floating in the card, which is
		// what a debug readout looks like rather than a panel heading.
		UHorizontalBox* HeadingRow = WidgetTree->ConstructWidget<UHorizontalBox>(
			UHorizontalBox::StaticClass(), TEXT("InboxHeading"));

		TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InboxTitle"));
		TitleText->SetText(NSLOCTEXT("AirportMgr", "InboxTitle", "OFFERS"));
		// The same heading treatment the bar's sections take - see UUIStyle::ApplyText (#89).
		Style->ApplyText(*TitleText, EUITextRole::Heading, Style->TextMuted);
		HeadingRow->AddChildToHorizontalBox(TitleText)->SetVerticalAlignment(VAlign_Center);

		UHorizontalBoxSlot* HeadGap = HeadingRow->AddChildToHorizontalBox(
			WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
		HeadGap->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		BadgeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InboxBadge"));
		Style->ApplyText(*BadgeText, EUITextRole::Label, Style->TextMuted);
		UHorizontalBoxSlot* BadgeSlot = HeadingRow->AddChildToHorizontalBox(BadgeText);
		BadgeSlot->SetPadding(FMargin(16.0f, 0.0f, 0.0f, 0.0f));
		BadgeSlot->SetVerticalAlignment(VAlign_Center);

		Column->AddChildToVerticalBox(HeadingRow)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

		OfferColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InboxRows"));
		Column->AddChildToVerticalBox(OfferColumn);

		UE_LOG(LogOfferInbox, Log, TEXT("No inbox asset: building the code-only panel"));
	}
}

void UOfferInboxWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// The actor is found through the world rather than held, because URoadEditFacade::
	// ClearNetwork replaces the network object and a cached pointer would go stale.
	if (const UWorld* World = GetWorld())
	{
		for (TActorIterator<ARoadNetworkActor> It(const_cast<UWorld*>(World)); It; ++It)
		{
			Refresh(*It);
			return;
		}
	}
}

void UOfferInboxWidget::Refresh(ARoadNetworkActor* Target)
{
	if (Inbox == nullptr || Target == nullptr || Target->Network == nullptr)
	{
		return;
	}

	UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	UGroundTraffic* Traffic = Target->GetTraffic() != nullptr ? Target->GetTraffic()->GetModel() : nullptr;
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
		// "none" rather than "0": the player is being told a STATE, and a zero is a value.
		const int32 Pending = Inbox->GetPendingCount();
		BadgeText->SetText(Pending == 0
			? NSLOCTEXT("AirportMgr", "InboxNone", "none")
			: FText::AsNumber(Pending));
	}

	// The Blueprint path: the list view owns the rows and MVVM gives each entry widget its
	// own UOfferViewModel through UMVVMViewListViewBaseClassExtension.
	if (OfferList != nullptr)
	{
		OfferList->SetListItems(Rows);
		return;
	}

	if (OfferColumn == nullptr)
	{
		return;
	}

	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();   // never null

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
			Entry->AcceptButton->SetIsEnabled(Row->IsAcceptable());

			// ACCEPT IS THE ONE THING ON THIS CARD THAT TAKES ACCENT. The bar spends that
			// colour on the armed tool and nothing else; here it is the affirmative verb,
			// and the two never share a screen region.
			Entry->AcceptButton->SetBackgroundColor(Row->IsAcceptable() ? Style->Accent : Style->Button);
		}
	}
}

UWidget* UOfferInboxWidget::BuildRow(const UUIStyle& Style, UOfferRowEntry& Entry, int32 Index)
{
	// ONE CARD PER OFFER, Panel over the inbox's PanelDark ground, so a row reads as a thing
	// that can be answered rather than as a line of text. Rounded from the style's own
	// CornerRadius, which sat in the asset unread while everything drew as square slabs.
	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(
		UBorder::StaticClass(), *FString::Printf(TEXT("OfferCard%d"), Index));
	Card->SetBrush(FSlateRoundedBoxBrush(Style.Panel, Style.CornerRadius));
	Card->SetPadding(FMargin(10.0f, 8.0f));

	UVerticalBox* Lines = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Card->SetContent(Lines);

	// LINE ONE: who is asking, and how long is left. The countdown sits hard right because it
	// is the field that MOVES, and a moving number is easier to read in a column of its own
	// than buried mid-sentence.
	UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Entry.AirlineText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.AirlineText, EUITextRole::Title, Style.Text);
	Head->AddChildToHorizontalBox(Entry.AirlineText)->SetVerticalAlignment(VAlign_Center);

	UHorizontalBoxSlot* GapSlot = Head->AddChildToHorizontalBox(
		WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass()));
	GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	Entry.EtaText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.EtaText, EUITextRole::Label, Style.TextMuted);
	UHorizontalBoxSlot* EtaSlot = Head->AddChildToHorizontalBox(Entry.EtaText);
	EtaSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	EtaSlot->SetVerticalAlignment(VAlign_Center);
	Lines->AddChildToVerticalBox(Head)->SetHorizontalAlignment(HAlign_Fill);

	// LINE TWO: the airframe, quieter. It matters while deciding, not while scanning.
	Entry.TypeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Style.ApplyText(*Entry.TypeText, EUITextRole::Body, Style.TextMuted);
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
		NSLOCTEXT("AirportMgr", "OfferAccept", "Accept"), Style.Accent, Style.PanelDark, Index);
	Entry.AcceptButton->OnClicked.AddDynamic(&Entry, &UOfferRowEntry::HandleAccept);
	Answers->AddChildToHorizontalBox(Entry.AcceptButton)->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));

	UButton* DeclineButton = MakeAnswerButton(Style, TEXT("Decline"),
		NSLOCTEXT("AirportMgr", "OfferDecline", "Decline"), Style.Button, Style.Text, Index);
	DeclineButton->OnClicked.AddDynamic(&Entry, &UOfferRowEntry::HandleDecline);
	Answers->AddChildToHorizontalBox(DeclineButton);

	Lines->AddChildToVerticalBox(Answers)->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	return Card;
}

UButton* UOfferInboxWidget::MakeAnswerButton(const UUIStyle& Style, const TCHAR* Name,
	const FText& Label, const FLinearColor& Fill, const FLinearColor& Ink, int32 Index)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>(
		UButton::StaticClass(), *FString::Printf(TEXT("Offer%s%d"), Name, Index));

	// ROUNDED THROUGH THE BUTTON STYLE, not through a background tint. UButton draws its own
	// FButtonStyle brushes, so SetBrush on the widget is ignored and SetBackgroundColor only
	// tints whichever brush the style already has - which was the engine's flat default box,
	// and is why these read as stock editor buttons. The brushes are left WHITE so that
	// SetBackgroundColor stays the one place a state colour is chosen, as the repaint does.
	FButtonStyle ButtonStyle = Button->GetStyle();
	const FSlateRoundedBoxBrush Rounded(FLinearColor::White, Style.CornerRadius);
	ButtonStyle.SetNormal(Rounded);
	ButtonStyle.SetHovered(Rounded);
	ButtonStyle.SetPressed(Rounded);
	ButtonStyle.SetDisabled(Rounded);
	ButtonStyle.SetNormalPadding(FMargin(12.0f, 5.0f));
	ButtonStyle.SetPressedPadding(FMargin(12.0f, 5.0f));
	Button->SetStyle(ButtonStyle);
	Button->SetBackgroundColor(Fill);

	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Text->SetText(Label);
	Style.ApplyText(*Text, EUITextRole::Label, Ink);
	Button->AddChild(Text);
	return Button;
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
