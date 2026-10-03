#include "AirlinesPanelWidget.h"

#include "AirlineViewModels.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Model/AirlineHistory.h"
#include "Model/AirlineRoster.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/SimClock.h"
#include "OfferViewModels.h"
#include "Present/OpsRuntime.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "UI/UiSparkline.h"
#include "UIStyle.h"

#define LOCTEXT_NAMESPACE "Airlines"

// GLYPHS CHECKED AGAINST THE FONT, not assumed (2026-10-03): the UI font is Inter (UUIStyle::Composite, FF_Inter_Regular), whose cmap
// has U+25B2/U+25BC (the inbox's mood arrows, which the list now prints through UOfferViewModel::MoodArrowOf), U+2713/U+2717 and
// U+2014 - and NOT the heavier U+2714/U+2718 the brief drew. A missing glyph renders as a box, so the tick and cross are the light pair.
namespace
{
	const TCHAR* const AirlinesTick = TEXT("✓");
	const TCHAR* const AirlinesCross = TEXT("✗");
	const TCHAR* const AirlinesDash = TEXT("—");
}

void UAirlineRowEntry::HandleClick()
{
	if (UAirlinesPanelWidget* Panel = Owner.Get())
	{
		Panel->Select(AirlineId);
	}
}

void UAirlinesPanelWidget::BuildOnce(const UUIStyle& Style)
{
	ListModel = NewObject<UAirlineListViewModel>(this);
	DetailModel = NewObject<UAirlineDetailViewModel>(this);
	BuildLayout(Style);

	// SelfHitTestInvisible on the ROOT; the WINDOW hides (SetShown) - see UAirportMgrPanelWidget::BuildOnce.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetShown(false);
}

void UAirlinesPanelWidget::BuildLayout(const UUIStyle& Style)
{
	UVerticalBox* Root = Cast<UVerticalBox>(EnsureContentRoot(TEXT("AirlinesCard")));
	if (Root == nullptr)
	{
		return;   // an asset supplied the root - its layout, not this one
	}

	// LIST LEFT, DETAIL RIGHT (the owner's choice, spec section 2): the list is short and always in view, so picking another airline
	// never means scrolling back up past the last one's flights.
	UHorizontalBox* Body = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("AirlinesBody"));
	Root->AddChildToVerticalBox(Body);

	USizeBox* ListBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("AirlinesListBox"));
	ListBox->SetWidthOverride(ListWidth);
	UVerticalBox* Left = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesLeft"));
	ListBox->SetContent(Left);
	ListColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesList"));
	Left->AddChildToVerticalBox(ListColumn);
	NoAirlinesText = AddLine(*Left, LOCTEXT("NoAirlines", "No airlines yet"), Style.InkMuted);
	UHorizontalBoxSlot* LeftSlot = Body->AddChildToHorizontalBox(ListBox);
	LeftSlot->SetPadding(FMargin(0.0f, 0.0f, Style.SectionPadding, 0.0f));

	// THE DETAIL SCROLLS ON ITS OWN, under a height cap: the window's body already scrolls (UUiWindow), but scrolling THAT would carry
	// the list away with the detail, which is the thing this layout exists to avoid.
	USizeBox* DetailBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("AirlinesDetailBox"));
	DetailBox->SetWidthOverride(DetailWidth);
	DetailBox->SetMaxDesiredHeight(DetailMaxHeight);
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("AirlinesDetailScroll"));
	DetailBox->SetContent(Scroll);
	DetailColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesDetail"));
	Scroll->AddChild(DetailColumn);
	Body->AddChildToHorizontalBox(DetailBox);

	// HEADER: name and percentage on one line, the generator's two figures under it.
	UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("AirlinesHeader"));
	HeaderName = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AirlinesHeaderName"));
	Style.ApplyText(*HeaderName, EUITextRole::Title, Style.Ink);
	UHorizontalBoxSlot* NameSlot = HeaderRow->AddChildToHorizontalBox(HeaderName);
	NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	HeaderPercent = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("AirlinesHeaderPercent"));
	Style.ApplyText(*HeaderPercent, EUITextRole::Title, Style.Ink);
	HeaderRow->AddChildToHorizontalBox(HeaderPercent);
	DetailColumn->AddChildToVerticalBox(HeaderRow);
	RateText = AddLine(*DetailColumn, FText::GetEmpty(), Style.InkMuted);
	FactorText = AddLine(*DetailColumn, FText::GetEmpty(), Style.InkMuted);

	AddHeading(*DetailColumn, LOCTEXT("LastSevenDays", "Last 7 days"));
	Trend = WidgetTree->ConstructWidget<UUiSparkline>(UUiSparkline::StaticClass(), TEXT("AirlinesTrend"));
	Trend->SetStyle(&Style);
	UVerticalBoxSlot* TrendSlot = DetailColumn->AddChildToVerticalBox(Trend);
	TrendSlot->SetPadding(FMargin(0.0f, 2.0f));
	TrendSlot->SetHorizontalAlignment(HAlign_Left);
	NoHistoryText = AddLine(*DetailColumn, LOCTEXT("NoHistory", "No history yet"), Style.InkMuted);
	TallyColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesTallies"));
	DetailColumn->AddChildToVerticalBox(TallyColumn);

	AddHeading(*DetailColumn, LOCTEXT("Fleet", "Fleet"));
	FleetColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesFleet"));
	DetailColumn->AddChildToVerticalBox(FleetColumn);

	AddHeading(*DetailColumn, LOCTEXT("OfferingNow", "Offering now"));
	OfferColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesOffers"));
	DetailColumn->AddChildToVerticalBox(OfferColumn);

	AddHeading(*DetailColumn, LOCTEXT("FlightsWithYou", "Flights with you"));
	FlightColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("AirlinesFlights"));
	DetailColumn->AddChildToVerticalBox(FlightColumn);
}

UTextBlock* UAirlinesPanelWidget::AddHeading(UVerticalBox& Column, const FText& Text)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	if (PanelStyle != nullptr)
	{
		PanelStyle->ApplyText(*Block, EUITextRole::Heading, PanelStyle->InkMuted);
	}
	UVerticalBoxSlot* HeadingSlot = Column.AddChildToVerticalBox(Block);
	// A SECTION'S GAP ABOVE, the style's own: each heading starts a new question about the airline.
	HeadingSlot->SetPadding(FMargin(0.0f, PanelStyle != nullptr ? PanelStyle->SectionPadding : 0.0f, 0.0f, 2.0f));
	return Block;
}

UTextBlock* UAirlinesPanelWidget::AddLine(UVerticalBox& Column, const FText& Text, const FLinearColor& Colour, bool bWrap)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	if (PanelStyle != nullptr)
	{
		PanelStyle->ApplyText(*Block, EUITextRole::Label, Colour);
	}
	Block->SetAutoWrapText(bWrap);
	Column.AddChildToVerticalBox(Block);
	return Block;
}

bool UAirlinesPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("airlines");
	Out.Title = LOCTEXT("AirlinesWindow", "Airlines");
	// CENTRE, where the eye already is: a two-pane window the player opens to READ, not furniture kept open beside play. The corners
	// are taken - top-left alerts, top-right inbox and ledger, bottom-left the inspector. Dragged, it stays where put (the host's store).
	Out.Anchor = EUiWindowAnchor::Centre;
	Out.Offset = FVector2D(0.0, 0.0);
	Out.bToggled = true;   // the bar's Airlines button and the close are one toggle - the host's (#447)
	return true;
}

void UAirlinesPanelWidget::OnShownChanged(bool bShown)
{
	if (bShown)
	{
		// A WINDOW REOPENED is checked at once, not up to a real second later: its game-time text went stale while it was shut.
		RealSinceTextCheck = TextCheckSeconds;
		Refresh();
	}
}

void UAirlinesPanelWidget::TickPanel(float DeltaTime)
{
	// ONLY WHILE OPEN, the ledger's rule - and even then the memo keys make a quiet frame cost a hash, not a rebuild. DeltaTime is
	// the widget tick's REAL seconds, which is what the once-a-second text check counts.
	if (IsShown())
	{
		RealSinceTextCheck += DeltaTime;
		Refresh();
	}
}

uint32 UAirlinesPanelWidget::ListKeyOf(const UOpsRuntime& Runtime)
{
	// NO ROSTER REVISION EXISTS, so the key is a hash of what the list PRINTS - every standing's id, and its percentage and arrow from
	// the inbox's own key (UOfferViewModel::MoodKeyOf, which the view model builds the row from) - so a move too small to print
	// rebuilds no button under the cursor. The names and the floor flag come from the catalog, which does not change in play.
	uint32 Key = 0;
	if (const UAirlineRoster* Roster = Runtime.GetAirlines())
	{
		for (const FAirlineStanding& Standing : Roster->GetStandings())
		{
			const FOfferMoodKey Mood = UOfferViewModel::MoodKeyOf(&Standing);
			Key = HashCombine(Key, HashCombine(GetTypeHash(Standing.AirlineId), GetTypeHash(Mood.Percent)));
			Key = HashCombine(Key, GetTypeHash(UOfferViewModel::MoodArrowOf(Mood)));
		}
		Key = HashCombine(Key, GetTypeHash(Roster->GetStandings().Num()));
	}
	return Key;
}

uint32 UAirlinesPanelWidget::DetailKeyOf(const UOpsRuntime& Runtime) const
{
	// WHAT MOVES THE DETAIL BETWEEN TEXT CHECKS, cheap enough for every frame: the selection; the board's revision (an offer or flight
	// came, went or changed phase); the selected standing's value (every recorded change moves it - the header, the tally, the line);
	// the history's day (a close); and the selected airline's offers' countdowns in the WHOLE seconds they print (SecondsLeftOf, the
	// inbox's rounding), which drain in REAL time - so "43 s" becomes "42 s" on the frame it changes. NOT the game clock: at x32 it moves
	// every frame, and what it moves is checked once a real second instead (see the class comment).
	uint32 Key = GetTypeHash(Selected);
	if (const UFlightBoard* Board = Runtime.GetFlightBoard())
	{
		Key = HashCombine(Key, GetTypeHash(Board->Revision()));
		for (const UFlight* Offer : Board->Offers())
		{
			if (Offer != nullptr && Offer->AirlineId == Selected)
			{
				Key = HashCombine(Key, GetTypeHash(UOfferViewModel::SecondsLeftOf(*Offer)));
			}
		}
	}
	if (const UAirlineRoster* Roster = Runtime.GetAirlines())
	{
		const FAirlineStanding* Standing = Roster->Find(Selected);
		Key = HashCombine(Key, GetTypeHash(Standing != nullptr ? Standing->Satisfaction : -1.0));
	}
	if (const UAirlineHistory* History = Runtime.GetAirlineHistory())
	{
		Key = HashCombine(Key, GetTypeHash(History->GetCurrentDay()));
	}
	return Key;
}

uint32 UAirlinesPanelWidget::TextKeyOf(const FAirlineDetail& D)
{
	// EVERYTHING PaintDetail PRINTS OR SHOWS, and nothing else: a field painted and left out here would be a sentence that never
	// refreshes on a quiet frame, so this walks D in the paint's own order.
	auto Text = [](uint32 Key, const FText& Value) { return HashCombine(Key, GetTypeHash(Value.ToString())); };
	uint32 Key = Text(0, D.Name);
	Key = HashCombine(Key, GetTypeHash(D.bHasStanding));
	Key = HashCombine(Key, GetTypeHash(D.SatisfactionPct));
	Key = Text(Key, D.RateLine);
	Key = Text(Key, D.FactorLine);
	Key = HashCombine(Key, GetTypeHash(D.bHasHistory));
	for (const double Value : D.Trend)
	{
		Key = HashCombine(Key, GetTypeHash(Value));
	}
	for (const FAirlineTallyRow& Tally : D.Tallies)
	{
		Key = Text(Key, Tally.Label);
		Key = HashCombine(Key, HashCombine(GetTypeHash(Tally.Count), GetTypeHash(Tally.Points)));
		Key = Text(Key, Tally.DeltaText);
	}
	Key = HashCombine(Key, GetTypeHash(D.bJudged));
	for (const FAirlineFleetRow& Fleet : D.Fleet)
	{
		Key = Text(Key, Fleet.TypeName);
		Key = HashCombine(Key, GetTypeHash(Fleet.bAdmitted));
		Key = Text(Key, Fleet.Reason);
	}
	for (const FAirlineOfferRow& Offer : D.Offers)
	{
		Key = Text(Text(Text(Key, Offer.Callsign), Offer.TypeName), Offer.Countdown);
	}
	for (const FAirlineFlightRow& Flight : D.Flights)
	{
		Key = Text(Text(Text(Text(Key, Flight.Callsign), Flight.TypeName), Flight.Phase), Flight.Contract);
		Key = HashCombine(Key, GetTypeHash(Flight.bLate));
	}
	// THE SECTION SIZES TOO, so a row moving from one section to the next ("No offers" now, the same flight under "Flights") is a change.
	Key = HashCombine(Key, HashCombine(GetTypeHash(D.Tallies.Num()), GetTypeHash(D.Fleet.Num())));
	return HashCombine(Key, HashCombine(GetTypeHash(D.Offers.Num()), GetTypeHash(D.Flights.Num())));
}

void UAirlinesPanelWidget::Refresh()
{
	const UOpsRuntime* Runtime = OpsRuntime();
	if (Runtime == nullptr || ListColumn == nullptr)
	{
		return;
	}
	const uint32 ListKey = ListKeyOf(*Runtime);
	if (!bHasPaintedList || ListKey != PaintedListKey)
	{
		PaintList(*Runtime);
		PaintedListKey = ListKey;
		bHasPaintedList = true;
	}
	// THE DETAIL: build its view model when the cheap key moved or a real second passed; rebuild its widgets only when what that
	// prints differs from the screen - see the class comment.
	const uint32 DetailKey = DetailKeyOf(*Runtime);
	const bool bKeyMoved = !bHasPaintedDetail || DetailKey != PaintedDetailKey;
	if (!bKeyMoved && RealSinceTextCheck < TextCheckSeconds)
	{
		return;
	}
	PaintedDetailKey = DetailKey;
	bHasPaintedDetail = true;
	RealSinceTextCheck = 0.0f;
	if (Selected.IsNone() || DetailModel == nullptr)
	{
		// Nothing to be about: collapsed once, then left alone (text key 0 stands for "no airline").
		if (!bHasPaintedText || PaintedTextKey != 0)
		{
			PaintDetail(nullptr);
			PaintedTextKey = 0;
			bHasPaintedText = true;
		}
		return;
	}
	const USimClock* Clock = Runtime->GetClock();
	const FAirlineDetail D = DetailModel->Build(*Runtime, Selected, Clock != nullptr ? Clock->Now() : 0.0);
	const uint32 TextKey = TextKeyOf(D);
	if (!bHasPaintedText || TextKey != PaintedTextKey)
	{
		PaintDetail(&D);
		PaintedTextKey = TextKey;
		bHasPaintedText = true;
	}
}

void UAirlinesPanelWidget::Select(FName AirlineId)
{
	if (AirlineId == Selected)
	{
		return;
	}
	Selected = AirlineId;
	UE_LOG(LogRoadBuild, Log, TEXT("Airlines window: %s selected"), *AirlineId.ToString());
	// RELIT, NOT REBUILT: this runs inside the clicked button's OnClicked, and the list's buttons stay - see the class comment. The
	// detail repaints now (its key holds the selection), so the click is answered this frame rather than on the next tick.
	LightSelectedRow();
	Refresh();
}

void UAirlinesPanelWidget::PaintList(const UOpsRuntime& Runtime)
{
	if (ListModel == nullptr || PanelStyle == nullptr)
	{
		return;
	}
	const UUIStyle& Style = *PanelStyle;
	const TArray<FAirlineListRow> Rows = ListModel->BuildRows(Runtime);

	// REBUILT WHOLE, the ledger's reason: a satisfaction change reorders nothing but relabels a row, and the rows are a dozen at most
	// (Content/Entities held 2 airline definitions on 2026-10-03), so a diff would be an index invariant to get wrong for no measurable saving.
	ListColumn->ClearChildren();
	Entries.Reset();
	// NEVER A BLANK DETAIL: with nothing chosen yet, the first row (floor first, then by name - the view model's order) is.
	if (Selected.IsNone() && Rows.Num() > 0)
	{
		Selected = Rows[0].AirlineId;
	}
	for (const FAirlineListRow& Row : Rows)
	{
		UAirlineRowEntry* Entry = NewObject<UAirlineRowEntry>(this);
		Entry->AirlineId = Row.AirlineId;
		Entry->Owner = this;

		UUiButton* Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		Button->SetLabel(Row.Name);
		Button->SetLabelMinWidth(ListWidth * 0.6f);
		// THE INBOX'S ARROW (the view model's Row.Arrow, UOfferViewModel::MoodArrowOf): the newest remembered change, so this list and
		// the inbox row beside it never point opposite ways for one airline. The week's direction is the detail's line.
		Button->SetDetail(!Row.Arrow.IsEmpty()
			? FText::Format(LOCTEXT("ListPctTrend", "{0}% {1}"), FText::AsNumber(Row.SatisfactionPct), FText::FromString(Row.Arrow))
			: FText::Format(LOCTEXT("ListPct", "{0}%"), FText::AsNumber(Row.SatisfactionPct)));
		// A SECONDARY BUTTON, LIT WHEN SELECTED - the radio group's and the bar's "this one" (UUiButton::LookFor), rather than a UUiRow:
		// UUiRow is a UBorder, which handles no click, and a row the player picks from must be one.
		Button->Build(Style, EUiButtonKind::Secondary);
		Button->OnClicked.AddDynamic(Entry, &UAirlineRowEntry::HandleClick);
		Entry->Button = Button;

		UVerticalBoxSlot* RowSlot = ListColumn->AddChildToVerticalBox(Button);
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
		Entries.Add(Entry);
	}
	LightSelectedRow();
	if (NoAirlinesText != nullptr)
	{
		NoAirlinesText->SetVisibility(Rows.Num() == 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}
	++ListRebuilds;
}

void UAirlinesPanelWidget::LightSelectedRow()
{
	for (const TObjectPtr<UAirlineRowEntry>& Entry : Entries)
	{
		if (Entry != nullptr && Entry->Button != nullptr)
		{
			Entry->Button->SetState(true, Entry->AirlineId == Selected);
		}
	}
}

void UAirlinesPanelWidget::PaintDetail(const FAirlineDetail* Built)
{
	if (PanelStyle == nullptr || DetailColumn == nullptr)
	{
		return;
	}
	const UUIStyle& Style = *PanelStyle;
	++DetailRebuilds;
	// VERBOSE, for the PIE check of the memo (about one a real second while an offer's countdown runs, none on a quiet pane at any
	// speed): `log LogRoadBuild Verbose`, then count these per second.
	UE_LOG(LogRoadBuild, Verbose, TEXT("Airlines window: detail rebuilt (%d) for %s"), DetailRebuilds, *Selected.ToString());

	TallyColumn->ClearChildren();
	FleetColumn->ClearChildren();
	OfferColumn->ClearChildren();
	FlightColumn->ClearChildren();
	if (Built == nullptr)
	{
		// No airline at all (an empty roster): the list says so; the detail has nothing to be about.
		DetailColumn->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	DetailColumn->SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	const FAirlineDetail& D = *Built;

	HeaderName->SetText(D.Name);
	// NO STANDING IS A DASH, never "0%": 0% would say the airline hates the airport, when nothing is known about it yet.
	HeaderPercent->SetText(D.bHasStanding
		? FText::Format(LOCTEXT("HeaderPct", "{0}%"), FText::AsNumber(D.SatisfactionPct))
		: FText::FromString(AirlinesDash));
	RateText->SetText(D.RateLine);
	RateText->SetVisibility(D.RateLine.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
	FactorText->SetText(D.FactorLine);
	FactorText->SetVisibility(D.FactorLine.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);

	// THE WEEK: a line once a day has closed, the words before. Today's partial tally shows either way (spec section 2).
	if (D.bHasHistory)
	{
		Trend->SetValues(D.Trend);
	}
	Trend->SetVisibility(D.bHasHistory ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	NoHistoryText->SetVisibility(D.bHasHistory ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);

	auto Cell = [this, &Style](UHorizontalBox& Line, const FText& Text, const FLinearColor& Colour, bool bFill)
	{
		UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Block->SetText(Text);
		Style.ApplyText(*Block, EUITextRole::Label, Colour);
		UHorizontalBoxSlot* CellSlot = Line.AddChildToHorizontalBox(Block);
		CellSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		if (bFill)
		{
			CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		return Block;
	};
	auto NewLine = [this](UVerticalBox& Column)
	{
		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Column.AddChildToVerticalBox(Line);
		return Line;
	};

	for (const FAirlineTallyRow& Tally : D.Tallies)
	{
		UHorizontalBox* Line = NewLine(*TallyColumn);
		Cell(*Line, Tally.Label, Style.Ink, true);
		Cell(*Line, FText::Format(LOCTEXT("TallyCount", "×{0}"), FText::AsNumber(Tally.Count)), Style.InkMuted, false);
		// COLOURED BY THE SIGN OF WHAT IS PRINTED - the row's Points, the view model's one rounding (PointsOf, which DescribeDelta also
		// uses), so a "0%" is never green or red - and read off the number, never parsed back out of the text.
		Cell(*Line, Tally.DeltaText, Tally.Points > 0 ? Style.Positive : Tally.Points < 0 ? Style.Warning : Style.InkMuted, false);
	}

	if (!D.bJudged)
	{
		AddLine(*FleetColumn, LOCTEXT("NotJudged", "Not judged yet"), Style.InkMuted);
	}
	for (const FAirlineFleetRow& Fleet : D.Fleet)
	{
		UHorizontalBox* Line = NewLine(*FleetColumn);
		Cell(*Line, FText::FromString(Fleet.bAdmitted ? AirlinesTick : AirlinesCross), Fleet.bAdmitted ? Style.Positive : Style.Warning, false);
		Cell(*Line, Fleet.TypeName, Style.Ink, true);
		if (!Fleet.bAdmitted && !Fleet.Reason.IsEmpty())
		{
			// THE PLAN'S OWN SENTENCE under its cross (the generator's verdict, not reworded here), wrapped - it carries figures.
			UTextBlock* Reason = AddLine(*FleetColumn, Fleet.Reason, Style.InkMuted, /*bWrap=*/true);
			if (UVerticalBoxSlot* ReasonSlot = Cast<UVerticalBoxSlot>(Reason->Slot))
			{
				ReasonSlot->SetPadding(FMargin(14.0f, 0.0f, 0.0f, 2.0f));
			}
		}
	}

	if (D.Offers.Num() == 0)
	{
		AddLine(*OfferColumn, LOCTEXT("NoOffers", "No offers"), Style.InkMuted);
	}
	for (const FAirlineOfferRow& Offer : D.Offers)
	{
		UHorizontalBox* Line = NewLine(*OfferColumn);
		Cell(*Line, FText::Format(LOCTEXT("OfferWhat", "{0} · {1}"), Offer.Callsign, Offer.TypeName), Style.Ink, true);
		Cell(*Line, Offer.Countdown, Style.InkMuted, false);
	}

	if (D.Flights.Num() == 0)
	{
		AddLine(*FlightColumn, LOCTEXT("NoFlights", "None at the airport"), Style.InkMuted);
	}
	for (const FAirlineFlightRow& Flight : D.Flights)
	{
		UHorizontalBox* Line = NewLine(*FlightColumn);
		Cell(*Line, FText::Format(LOCTEXT("FlightWhat", "{0} · {1}"), Flight.Callsign, Flight.TypeName), Style.Ink, true);
		Cell(*Line, Flight.Phase, Style.InkMuted, false);
		if (!Flight.Contract.IsEmpty())
		{
			// LATE IN WARNING, the arrivals row's colour for the same bool.
			UTextBlock* Contract = AddLine(*FlightColumn, Flight.Contract, Flight.bLate ? Style.Warning : Style.InkMuted);
			if (UVerticalBoxSlot* ContractSlot = Cast<UVerticalBoxSlot>(Contract->Slot))
			{
				ContractSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
			}
		}
	}
}

bool UAirlinesPanelWidget::ClickListRowForTest(int32 Index)
{
	if (!Entries.IsValidIndex(Index) || Entries[Index] == nullptr || Entries[Index]->Button == nullptr)
	{
		return false;
	}
	Entries[Index]->Button->OnClicked.Broadcast();
	return true;
}

bool UAirlinesPanelWidget::IsRowSelectedForTest(int32 Index) const
{
	// WHAT IS DRAWN, not a flag: the button's fill against the style's selected look.
	if (!Entries.IsValidIndex(Index) || Entries[Index] == nullptr || Entries[Index]->Button == nullptr || PanelStyle == nullptr)
	{
		return false;
	}
	return Entries[Index]->Button->GetBackgroundColor() == UUiButton::LookFor(*PanelStyle, EUiButtonKind::Secondary, true, true).Fill;
}

FText UAirlinesPanelWidget::HeaderNameForTest() const
{
	return HeaderName != nullptr ? HeaderName->GetText() : FText::GetEmpty();
}

FText UAirlinesPanelWidget::HeaderPercentForTest() const
{
	return HeaderPercent != nullptr ? HeaderPercent->GetText() : FText::GetEmpty();
}

bool UAirlinesPanelWidget::ShowsTextForTest(const FString& Words) const
{
	// THE UMG VISIBILITY, walked up the parents - NOT UWidget::IsVisible, which asks the Slate widget and answers false for every widget
	// in a headless test (none was ever constructed; Widget.cpp:395), so an empty state would have "passed" as hidden whatever it was.
	auto Drawn = [](const UWidget* Widget)
	{
		for (; Widget != nullptr; Widget = Widget->GetParent())
		{
			const ESlateVisibility V = Widget->GetVisibility();
			if (V == ESlateVisibility::Collapsed || V == ESlateVisibility::Hidden)
			{
				return false;
			}
		}
		return true;
	};
	TArray<UWidget*> All;
	WidgetTree->GetAllWidgets(All);
	for (const UWidget* Each : All)
	{
		const UTextBlock* Text = Cast<UTextBlock>(Each);
		if (Text != nullptr && Drawn(Text) && Text->GetText().ToString() == Words)
		{
			return true;
		}
	}
	return false;
}

#undef LOCTEXT_NAMESPACE
