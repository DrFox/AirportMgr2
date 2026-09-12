#include "ToastStackWidget.h"

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
#include "Model/OpsEvents.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "Styling/SlateBrush.h"
#include "UIStyle.h"

DEFINE_LOG_CATEGORY_STATIC(LogToasts, Log, All);

bool UToastStackWidget::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || bBuilt || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	bBuilt = true;

	// Owned by the widget, not by the runtime. The centre holds UI state - what the player
	// has been shown and for how long - and nothing in the sim reads it back; putting it on
	// UOpsRuntime would have made a save-game question out of a reading time.
	Notifications = NewObject<UNotificationCentre>(this);

	EnsureSlots();

	// THE ONLY SUBSCRIBER THAT TURNS EVENTS INTO USER-VISIBLE ENTRIES, so there is one place
	// that decides what is worth telling the player (spec section 6.1).
	if (UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
	{
		Runtime->GetEvents()->OnNotification.AddDynamic(this, &UToastStackWidget::OnNotification);
		Runtime->GetEvents()->OnArrivalRefused.AddDynamic(this, &UToastStackWidget::OnArrivalRefused);
	}
	else
	{
		// Not an error: a headless test builds this with no runtime and posts to the centre
		// directly. Said out loud so a silent feed in PIE has a line to look for.
		UE_LOG(LogToasts, Log, TEXT("No ops runtime: the toast stack is up but subscribed to nothing"));
	}
	return bOk;
}

void UToastStackWidget::EnsureSlots()
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();   // never null

	// SelfHitTestInvisible: a toast must never eat a click meant for the world underneath.
	// It is information, not a control - that is the whole difference from an offer.
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

void UToastStackWidget::OnNotification(const FString& Text)
{
	if (Notifications != nullptr)
	{
		Notifications->PostFeed(FText::FromString(Text));
	}
}

void UToastStackWidget::OnArrivalRefused(EArrivalRefusal Why)
{
	// The wording is Airside's, from the reason-only overload of DescribeRefusal: the toast
	// and the log line the model already writes say the SAME sentence rather than two
	// opinions about the same refusal.
	const FString Sentence = ArrivalPlanner::DescribeRefusal(Why);
	if (Notifications != nullptr && !Sentence.IsEmpty())
	{
		// WARNING, not Info: a refusal is the game declining to do what the player asked, and
		// it usually names something they must build. Save and load confirmations stay Info.
		Notifications->PostFeed(FText::FromString(Sentence), ENotificationSeverity::Warning);
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

	Rebuild(*UAirportMgrUISettings::ResolveStyle());
}

FLinearColor UToastStackWidget::ColourFor(const UUIStyle& Style, ENotificationSeverity Severity)
{
	switch (Severity)
	{
	case ENotificationSeverity::Warning: return Style.Warning;
	case ENotificationSeverity::Success: return Style.Positive;
	case ENotificationSeverity::Info:
	default:                             return Style.TextMuted;
	}
}

UTexture2D* UToastStackWidget::IconFor(const UUIStyle& Style, ENotificationSeverity Severity)
{
	switch (Severity)
	{
	case ENotificationSeverity::Warning: return Style.IconWarning.LoadSynchronous();
	case ENotificationSeverity::Success: return Style.IconSuccess.LoadSynchronous();
	case ENotificationSeverity::Info:
	default:                             return Style.IconInfo.LoadSynchronous();
	}
}

void UToastStackWidget::Rebuild(const UUIStyle& Style)
{
	if (ToastColumn == nullptr)
	{
		return;
	}

	// Rebuilt each tick rather than diffed. The list is at most a handful of rows and every
	// one of them changes opacity every frame anyway, so a diff would buy nothing and cost
	// a second model of what is on screen.
	ToastColumn->ClearChildren();

	const double Now = Notifications->Now();
	for (const FNotificationEntry& Entry : Notifications->Entries())
	{
		if (Entry.Kind != ENotificationKind::Feed)
		{
			continue;   // alerts have their own surface; the feed does not carry them
		}

		const FLinearColor Severity = ColourFor(Style, Entry.Severity);

		// A ROUNDED CARD, NOT A TINTED RECTANGLE. UBorder's default brush is a flat box, and
		// SetBrushColor only tints it - which is how these drew as square slabs while
		// UUIStyle::CornerRadius sat in the asset unread. FSlateRoundedBoxBrush is the only
		// thing in Slate that actually rounds a corner, and it takes the radius and an
		// outline in one construction.
		//
		// PanelDark, not Panel: a toast floats OVER the world and sits directly above a
		// Panel-coloured bar, so drawing it in Panel made it read as part of the bar.
		UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Row->SetBrush(FSlateRoundedBoxBrush(Style.PanelDark, Style.CornerRadius,
			FLinearColor(Severity.R, Severity.G, Severity.B, 0.85f), ToastOutlineWidth));
		Row->SetPadding(FMargin(12.0f, 9.0f));

		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->SetContent(Line);

		// The severity says itself twice - icon and outline - because colour alone is not a
		// message to a player who cannot tell the brick from the sage.
		if (UTexture2D* Icon = IconFor(Style, Entry.Severity))
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
		Words->SetColorAndOpacity(FSlateColor(Style.Text));
		FSlateFontInfo Font = Style.LabelFont.HasValidFont() ? Style.LabelFont : Words->GetFont();
		Font.Size = 12;
		Words->SetFont(Font);

		// WRAPPED, and this is most of what made the old row look clunky: "Arrival refused:
		// the runway is in use. Wait for it to clear." on one line is a 400 uu ribbon across
		// the corner of the screen. Wrapped to a card width it is two short lines the eye
		// takes in at once.
		Words->SetAutoWrapText(true);
		Words->SetWrapTextAt(ToastWrapWidth);
		UHorizontalBoxSlot* WordSlot = Line->AddChildToHorizontalBox(Words);
		WordSlot->SetVerticalAlignment(VAlign_Center);

		// Fades over its last two seconds rather than vanishing, so the eye is not pulled to
		// a sudden disappearance at the edge of vision. Never below 0.15: a toast that has
		// faded out but not yet expired would still be taking up a row, and an invisible
		// row that pushes the others around reads as a glitch.
		const double Remaining = Notifications->FeedLifetimeRealSeconds - (Now - Entry.RaisedAtRealSeconds);
		Row->SetRenderOpacity(static_cast<float>(FMath::Clamp(Remaining / 2.0, 0.15, 1.0)));

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
	}
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
