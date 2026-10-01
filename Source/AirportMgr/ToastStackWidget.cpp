#include "ToastStackWidget.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsEvents.h"

#include "Blueprint/WidgetTree.h"
#include "BuildBarWidget.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "NotificationCentre.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/OpsEvents.h"
#include "Present/OpsRuntime.h"
#include "Styling/SlateBrush.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogToasts, Log, All);

void UToastStackWidget::BuildOnce(const UUIStyle& Style)
{
	// Owned by the widget, not by the runtime. The centre holds UI state - what the player
	// has been shown and for how long - and nothing in the sim reads it back; putting it on
	// UOpsRuntime would have made a save-game question out of a reading time.
	Notifications = NewObject<UNotificationCentre>(this);

	// The style itself is UAirportMgrPanelWidget::PanelStyle now (issue #309) - this class's
	// own CachedStyle duplicated it. The icons still need their OWN cache: PanelStyle is a
	// style ASSET, and Style.IconInfo/IconSuccess/IconWarning are separate soft references on
	// it that TickFeed would otherwise LoadSynchronous() three times EVERY TICK - issue #186.
	CachedIconInfo = Style.IconInfo.LoadSynchronous();
	CachedIconSuccess = Style.IconSuccess.LoadSynchronous();
	CachedIconWarning = Style.IconWarning.LoadSynchronous();

	EnsureSlots(&Style);

	// THE ONLY SUBSCRIBER THAT TURNS EVENTS INTO USER-VISIBLE ENTRIES, so there is one place
	// that decides what is worth telling the player (spec section 6.1).
	if (UOpsRuntime* Runtime = OpsRuntime())
	{
		BindTo(*Runtime->GetEvents());
	}
	else
	{
		// Not an error: a headless test builds this with no runtime and posts to the centre
		// directly. Said out loud so a silent feed in PIE has a line to look for.
		UE_LOG(LogToasts, Log, TEXT("No ops runtime: the toast stack is up but subscribed to nothing"));
	}
}

void UToastStackWidget::EnsureSlots(const UUIStyle* Style)
{
	// SelfHitTestInvisible, not Collapsed: see UAirportMgrPanelWidget::BuildOnce. A toast is
	// information, not a control, so this also means it never eats a click meant for the
	// world underneath it - the whole difference from an offer.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	if (WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ToastRoot"));
		WidgetTree->RootWidget = Root;

		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("ToastColumn"));
		UCanvasPanelSlot* ColumnSlot = Root->AddChildToCanvas(Column);
		ColumnSlot->SetAnchors(FAnchors(1.0f, 1.0f, 1.0f, 1.0f));
		ColumnSlot->SetAlignment(FVector2D(1.0, 1.0));
		ColumnSlot->SetAutoSize(true);

		// ASK THE BAR HOW TALL IT IS rather than retyping a number: the bar derives its own
		// height from the style, so a ButtonSize change would otherwise leave the toasts
		// behind it - visible in a screenshot only if somebody happened to take one.
		const float Clearance = UBuildBarWidget::BarHeightFor(*Style) + BottomOffset;
		ColumnSlot->SetPosition(FVector2D(-12.0, -Clearance));

		ToastColumn = Column;
		UE_LOG(LogToasts, Log, TEXT("No toast asset: building the code-only stack, %.0f uu clear of the bottom"), Clearance);
	}
}

void UToastStackWidget::BindTo(UOpsEvents& Events)
{
	Events.OnSaveSlot.AddUniqueDynamic(this, &UToastStackWidget::OnSaveSlot);
	Events.OnPurchase.AddUniqueDynamic(this, &UToastStackWidget::OnPurchase);
	Events.OnArrivalRefused.AddUniqueDynamic(this, &UToastStackWidget::OnArrivalRefused);
	Events.OnAlertRaised.AddUniqueDynamic(this, &UToastStackWidget::OnAlertRaised);
	Events.OnAlertCleared.AddUniqueDynamic(this, &UToastStackWidget::OnAlertCleared);
	Events.OnBuildRefused.AddUniqueDynamic(this, &UToastStackWidget::OnBuildRefused);
	Events.OnLandRefused.AddUniqueDynamic(this, &UToastStackWidget::OnLandRefused);
	Events.OnBalanceSignChanged.AddUniqueDynamic(this, &UToastStackWidget::OnBalanceSignChanged);
}

void UToastStackWidget::OnAlertRaised(const FOpsAlert& Alert)
{
	if (Alert.Key.Kind == EAlertKind::Overdrawn)
	{
		bToldOverdrawn = true;
	}
	// A RE-RAISE AFTER A LOAD is not news: the alert list shows it, the feed does not (stage 1 review).
	if (Notifications != nullptr && !Alert.bReRaised)
	{
		Notifications->PostFeed(Alert.Text, ENotificationSeverity::Warning);
	}
}

void UToastStackWidget::OnAlertCleared(const FOpsAlertKey& Key)
{
	// SILENT: the alert window's count going down says a problem ended, and a toast for each would bury the
	// ones that started. Coming out of the red IS said - by OnBalanceSignChanged, the money event, since
	// stage 3; saying it here too would toast it twice.
}

void UToastStackWidget::OnBalanceSignChanged(bool bOverdrawn)
{
	// ONLY THE WAY OUT, and only after the way in was said: going into the red is the Overdrawn alert's own
	// toast, and a dip and recovery inside one frame raised no alert - so "back in credit" would come from
	// nowhere (stage 3 review). NOT "building unlocked": at exactly zero CanAfford still refuses a price.
	// ENFORCED BY: AirportMgr.UI.ToastsSayAlertsAndRefusals
	if (Notifications != nullptr && !bOverdrawn && bToldOverdrawn)
	{
		bToldOverdrawn = false;
		Notifications->PostFeed(NSLOCTEXT("AirportMgr", "BackInCredit", "Back in credit"), ENotificationSeverity::Info);
	}
}

void UToastStackWidget::OnBuildRefused(const FString& What, const FString& Price, const FString& Balance)
{
	if (Notifications != nullptr)
	{
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "CannotAfford", "Can't afford {0} ({1}; balance {2})"),
			FText::FromString(What), FText::FromString(Price), FText::FromString(Balance)), ENotificationSeverity::Warning);
	}
}

void UToastStackWidget::OnLandRefused(EArrivalRefusal Why, const FString& Sentence)
{
	// THE SAME HANDLER as a dispatch refusal (OnArrivalRefused) - one account of why an aeroplane cannot land, which since
	// #471 carries the sentence on both doors.
	OnArrivalRefused(Why, Sentence);
}

// EVERY CASE BY NAME, NO default, in both functions below: an outcome or a purchase kind added to its enum is a BUILD ERROR
// here (C4062, raised by the macro), not a toast that silently says nothing.
// ENFORCED BY: C4062 as an error, AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void UToastStackWidget::OnSaveSlot(EOpsSaveOutcome Outcome, const FString& SlotName)
{
	if (Notifications == nullptr)
	{
		return;
	}
	// THE WORDS UOpsRuntime USED, moved here with the decision (#445 item 7) - and the severity, which a sentence on a
	// catch-all could not carry: a failed save and a missing one were Info, as plain as a good save.
	const FText Name = FText::FromString(SlotName);
	switch (Outcome)
	{
	case EOpsSaveOutcome::Saved:
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "Saved", "Saved '{0}'"), Name), ENotificationSeverity::Info);
		return;
	case EOpsSaveOutcome::SaveFailed:
		// A WARNING: the save the player asked for did not happen, and the airport is not on disk.
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "SaveFailed", "Save to '{0}' failed"), Name), ENotificationSeverity::Warning);
		return;
	case EOpsSaveOutcome::Loaded:
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "Loaded", "Loaded '{0}'"), Name), ENotificationSeverity::Info);
		return;
	case EOpsSaveOutcome::NoSave:
		// A WARNING, as a refusal is (OnArrivalRefused's reason): the load the player asked for found nothing to load.
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "NoSave", "No save '{0}'"), Name), ENotificationSeverity::Warning);
		return;
	}
}

void UToastStackWidget::OnPurchase(const FOpsPurchase& Purchase)
{
	if (Notifications == nullptr)
	{
		return;
	}
	// A FIGURE ONLY WHEN MONEY MOVED (#487): a sale or a removal worth nothing says so in words - "Sold X", "X withdrawn" -
	// rather than "credited $0". The nouns are the owners' (see FOpsPurchase); the sentences were UOpsRuntime's until #445.
	const bool bPaid = Purchase.Amount > 0.0;
	switch (Purchase.Kind)
	{
	case EOpsPurchaseKind::VehicleBought:
	case EOpsPurchaseKind::ModuleBought:
		Notifications->PostFeed(FText::Format(NSLOCTEXT("AirportMgr", "Bought", "Bought {0} — {1}"), Purchase.Name, Purchase.Money),
			ENotificationSeverity::Info);
		return;
	case EOpsPurchaseKind::VehicleSold:
		Notifications->PostFeed(bPaid
			? FText::Format(NSLOCTEXT("AirportMgr", "Sold", "Sold {0} — {1}"), Purchase.Name, Purchase.Money)
			: FText::Format(NSLOCTEXT("AirportMgr", "SoldForNothing", "Sold {0}"), Purchase.Name), ENotificationSeverity::Info);
		return;
	case EOpsPurchaseKind::VehicleWithdrawn:
		// A WITHDRAWN VEHICLE IS NEWS (#443): its depot went and the fleet's door credited it, and the feed says where that
		// money came from.
		Notifications->PostFeed(bPaid
			? FText::Format(NSLOCTEXT("AirportMgr", "DepotRemovedCredited", "Depot removed — {0} credited {1}"), Purchase.Name, Purchase.Money)
			: FText::Format(NSLOCTEXT("AirportMgr", "DepotRemovedWithdrawn", "Depot removed — {0} withdrawn"), Purchase.Name),
			ENotificationSeverity::Info);
		return;
	case EOpsPurchaseKind::ModulesRefunded:
		// THE REPAIR'S TOAST, A WARNING (#266): the player did not ask for it, the depot now holds less than it did, and they
		// may want to redraw the plot. The log's Warning (RemoveUnseated's) carries the depot and the counts.
		Notifications->PostFeed(bPaid
			? FText::Format(NSLOCTEXT("AirportMgr", "ModulesRefunded", "No room on its plot — {0} {1} removed, {2} refunded"),
				FText::AsNumber(Purchase.Count), Purchase.Name, Purchase.Money)
			: FText::Format(NSLOCTEXT("AirportMgr", "ModulesRemoved", "No room on its plot — {0} {1} removed"),
				FText::AsNumber(Purchase.Count), Purchase.Name),
			ENotificationSeverity::Warning);
		return;
	}
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void UToastStackWidget::OnArrivalRefused(EArrivalRefusal Why, const FString& Sentence)
{
	// THE REFUSAL'S OWN SENTENCE when it has one (#456 review; a dispatch refusal's too since #471) - the plan's, with its
	// figures and admission, or the airport's gate. The reason-only wording reads "not admitted to that runway" for an
	// arrivals-only field, whose real reason is that nothing can take the departure.
	// The wording is Airside's either way - the plan's sentence, or the reason-only overload of DescribeRefusal for a
	// caller that had none: the toast and the log line the model already writes say the SAME sentence rather than two
	// opinions about the same refusal.
	const FString Said = !Sentence.IsEmpty() ? Sentence : ArrivalPlanner::DescribeRefusal(Why);
	if (Notifications != nullptr && !Said.IsEmpty())
	{
		// WARNING, not Info: a refusal is the game declining to do what the player asked, and
		// it usually names something they must build. Save and load confirmations stay Info.
		Notifications->PostFeed(FText::FromString(Said), ENotificationSeverity::Warning);
	}
}

void UToastStackWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	TickFeed(InDeltaTime);
}

void UToastStackWidget::TickFeed(float RealDeltaSeconds)
{
	if (Notifications == nullptr)
	{
		return;
	}

	// RAW FRAME TIME, unscaled. Not TimeScale() - the clock runs 72x for the day
	// compression. Not Multiplier() either, which is the subtler one: at x32 that would
	// give an eight-second toast a quarter of a second on screen. A toast is a piece of UI
	// a HUMAN reads, so its lifetime belongs to the human and not to the simulation.
	Notifications->Advance(RealDeltaSeconds);

	// PanelStyle is set in Initialize before BuildOnce ever runs (issue #309, was this class's
	// own CachedStyle - see BuildOnce's comment); the ResolveStyle() fallback only covers a
	// test that drove TickFeed without going through Initialize.
	SyncCards(PanelStyle != nullptr ? *PanelStyle : *UAirportMgrUISettings::ResolveStyle());
}

FLinearColor UToastStackWidget::ColourFor(const UUIStyle& Style, ENotificationSeverity Severity)
{
	switch (Severity)
	{
	case ENotificationSeverity::Warning: return Style.Warning;
	case ENotificationSeverity::Success: return Style.Positive;
	case ENotificationSeverity::Info:
	default:                             return Style.InkMuted;
	}
}

UTexture2D* UToastStackWidget::IconFor(ENotificationSeverity Severity) const
{
	switch (Severity)
	{
	case ENotificationSeverity::Warning: return CachedIconWarning;
	case ENotificationSeverity::Success: return CachedIconSuccess;
	case ENotificationSeverity::Info:
	default:                             return CachedIconInfo;
	}
}

float UToastStackWidget::OpacityFor(const UUIStyle& Style, const FNotificationEntry& Entry) const
{
	// Fades over its last ToastFadeDuration seconds rather than vanishing, so the eye is not
	// pulled to a sudden disappearance at the edge of vision. Never below ToastFadeFloor: a
	// toast that has faded out but not yet expired would still be taking up a row, and an
	// invisible row that pushes the others around reads as a glitch. Both were bare literals
	// (2.0 / 0.15) until issue #192 - see UUIStyle::ToastFadeDuration's own comment.
	const double Remaining = Notifications->FeedLifetimeRealSeconds
		- (Notifications->Now() - Entry.RaisedAtRealSeconds);
	return static_cast<float>(FMath::Clamp(Remaining / Style.ToastFadeDuration, Style.ToastFadeFloor, 1.0));
}

void UToastStackWidget::SyncCards(const UUIStyle& Style)
{
	if (ToastColumn == nullptr)
	{
		return;
	}

	const TConstArrayView<FNotificationEntry> Entries = Notifications->Entries();

	// CHECKED AT EVERY INDEX, NOT JUST THE HEAD. Today Notifications->Entries() is a queue -
	// PostFeed only appends at the back, and both ways an entry leaves (Advance()'s
	// oldest-first expiry and PostFeed's MaxEntries cap) only ever drop the FRONT - so in
	// practice Cards[0] disagreeing with Entries[0] is the only disagreement there ever is,
	// and this loop exits after one comparison per tick. But that is because every entry
	// shares ONE FeedLifetimeRealSeconds; a future per-severity lifetime would let a Warning
	// outlive an Info raised earlier, expiring an entry out of the MIDDLE of the list, and a
	// front-only trim would silently hand a survivor's opacity (and, if this ever grows a
	// countdown label, its text) to the wrong card with no test going red. Walking every
	// index costs nothing while the invariant holds and stays correct the day it does not -
	// see AirportMgr.UI.ToastCardsSurviveARemovalFromTheMiddle, which removes from the middle
	// on purpose.
	int32 WalkIndex = 0;
	while (WalkIndex < Cards.Num() && WalkIndex < Entries.Num())
	{
		if (Cards[WalkIndex].EntryId != Entries[WalkIndex].Id)
		{
			if (Cards[WalkIndex].Card != nullptr)
			{
				Cards[WalkIndex].Card->RemoveFromParent();
			}
			Cards.RemoveAt(WalkIndex);
			continue;   // re-test THIS index against the array as it now stands
		}
		++WalkIndex;
	}

	// Anything past Entries.Num() belongs to no live entry either - the walk above only ever
	// compares up to Entries.Num(), so a removal at the very tail needs its own pass. Not
	// reachable while removal is front-only, same caveat as above.
	while (Cards.Num() > Entries.Num())
	{
		const int32 Last = Cards.Num() - 1;
		if (Cards[Last].Card != nullptr)
		{
			Cards[Last].Card->RemoveFromParent();
		}
		Cards.RemoveAt(Last);
	}

	// Append a card for every entry that arrived since the last tick. BuildCard is the ONLY
	// place a toast's widgets are constructed - everything about it but its opacity is fixed
	// for the entry's whole life.
	for (int32 Index = Cards.Num(); Index < Entries.Num(); ++Index)
	{
		FToastCard NewCard;
		NewCard.EntryId = Entries[Index].Id;
		NewCard.Card = BuildCard(Style, Entries[Index]);
		Cards.Add(NewCard);
	}

	// The only per-frame write for a survivor: SetRenderOpacity on a widget that already
	// exists. No ConstructWidget, no LoadSynchronous, no ClearChildren.
	for (int32 Index = 0; Index < Entries.Num() && Index < Cards.Num(); ++Index)
	{
		if (Cards[Index].Card != nullptr)
		{
			Cards[Index].Card->SetRenderOpacity(OpacityFor(Style, Entries[Index]));
		}
	}
}

UBorder* UToastStackWidget::BuildCard(const UUIStyle& Style, const FNotificationEntry& Entry)
{
	++CardsConstructed;

	const FLinearColor Severity = ColourFor(Style, Entry.Severity);

	// A ROUNDED CARD, NOT A TINTED RECTANGLE. UBorder's default brush is a flat box, and
	// SetBrushColor only tints it - which is how these drew as square slabs while
	// UUIStyle::CornerRadius (now WindowRadius) sat in the asset unread. FSlateRoundedBoxBrush is the only
	// thing in Slate that actually rounds a corner, and it takes the radius and an
	// outline in one construction.
	//
	// Surface, not Well: a toast floats OVER the world and sits directly above a
	// bar, so drawing it in the bar's own section colour made it read as part of the bar.
	UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	// UUIStyle::OutlineAlpha, not a literal here: see its own comment (issue #192).
	Row->SetBrush(FSlateRoundedBoxBrush(Style.Surface, Style.WindowRadius,
		FLinearColor(Severity.R, Severity.G, Severity.B, Style.OutlineAlpha), ToastOutlineWidth));
	Row->SetPadding(FMargin(12.0f, 9.0f));

	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Row->SetContent(Line);

	// The severity says itself twice - icon and outline - because colour alone is not a
	// message to a player who cannot tell the brick from the sage.
	if (UTexture2D* Icon = IconFor(Entry.Severity))
	{
		UImage* Chip = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		Chip->SetBrushFromTexture(Icon, false);
		Chip->SetDesiredSizeOverride(FVector2D(ToastIconSize));
		Chip->SetColorAndOpacity(Severity);
		UHorizontalBoxSlot* ChipSlot = Line->AddChildToHorizontalBox(Chip);
		ChipSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));
		ChipSlot->SetVerticalAlignment(VAlign_Center);
	}

	UTextBlock* Words = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Words->SetText(Entry.Text);
	Style.ApplyText(*Words, EUITextRole::Body, Style.Ink);

	// WRAPPED, and this is most of what made the old row look clunky: "Arrival refused:
	// the runway is in use. Wait for it to clear." on one line is a 400 uu ribbon across
	// the corner of the screen. Wrapped to a card width it is two short lines the eye
	// takes in at once.
	Words->SetAutoWrapText(true);
	Words->SetWrapTextAt(ToastWrapWidth);
	UHorizontalBoxSlot* WordSlot = Line->AddChildToHorizontalBox(Words);
	WordSlot->SetVerticalAlignment(VAlign_Center);

	// Set once at birth rather than left at 1.0: a toast built partway through its fade (the
	// tail of a burst posted in one frame with an already-ticked one) must not flash at full
	// opacity for a frame before the next tick corrects it.
	Row->SetRenderOpacity(OpacityFor(Style, Entry));

	// NEWEST AT THE BOTTOM, which is what append gives: the eye that just looked at the
	// bar is already at the bottom of the screen, and a new toast appearing under the
	// last one it read is the shortest distance for it to travel.
	//
	// A GAP BETWEEN CARDS. Without it the rounded corners meet and the stack fuses back
	// into the one slab the rounding was there to break up.
	UVerticalBoxSlot* RowSlot = Cast<UVerticalBoxSlot>(ToastColumn->AddChild(Row));
	if (RowSlot != nullptr)
	{
		RowSlot->SetPadding(FMargin(0.0f, ToastGap, 0.0f, 0.0f));
		RowSlot->SetHorizontalAlignment(HAlign_Right);
	}

	return Row;
}

int32 UToastStackWidget::ToastCountForTest() const
{
	return ToastColumn != nullptr ? ToastColumn->GetChildrenCount() : 0;
}

bool UToastStackWidget::FirstToastBrushForTest(FSlateBrush& OutBrush) const
{
	if (ToastColumn == nullptr || ToastColumn->GetChildrenCount() == 0)
	{
		return false;
	}
	const UBorder* Row = Cast<UBorder>(ToastColumn->GetChildAt(0));
	if (Row == nullptr)
	{
		return false;
	}
	OutBrush = Row->Background;
	return true;
}

UBorder* UToastStackWidget::NthToastForTest(int32 Index) const
{
	return (ToastColumn != nullptr && Index >= 0 && Index < ToastColumn->GetChildrenCount())
		? Cast<UBorder>(ToastColumn->GetChildAt(Index))
		: nullptr;
}

bool UToastStackWidget::NthToastTextForTest(int32 Index, FText& OutText) const
{
	const UBorder* Row = NthToastForTest(Index);
	const UHorizontalBox* Line = Row != nullptr ? Cast<UHorizontalBox>(Row->GetContent()) : nullptr;
	if (Line == nullptr)
	{
		return false;
	}

	// The text block is the LAST child: the icon, when the severity has one, is added first
	// in BuildCard. Searching from the end rather than assuming an index keeps this working
	// whether or not this entry got an icon.
	for (int32 ChildIndex = Line->GetChildrenCount() - 1; ChildIndex >= 0; --ChildIndex)
	{
		if (const UTextBlock* Words = Cast<UTextBlock>(Line->GetChildAt(ChildIndex)))
		{
			OutText = Words->GetText();
			return true;
		}
	}
	return false;
}
