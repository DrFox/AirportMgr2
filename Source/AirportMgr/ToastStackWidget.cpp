#include "ToastStackWidget.h"

#include "Blueprint/WidgetTree.h"
#include "BuildBarWidget.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "NotificationCentre.h"
#include "Model/OpsEvents.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
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
		Notifications->PostFeed(FText::FromString(Sentence));
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

		UBorder* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Row->SetBrushColor(Style.Panel);
		Row->SetPadding(FMargin(10.0f, 5.0f));

		UTextBlock* Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Line->SetText(Entry.Text);
		Line->SetColorAndOpacity(FSlateColor(Style.Text));
		FSlateFontInfo Font = Style.LabelFont.HasValidFont() ? Style.LabelFont : Line->GetFont();
		Font.Size = 11;
		Line->SetFont(Font);
		Row->SetContent(Line);

		// Fades over its last two seconds rather than vanishing, so the eye is not pulled to
		// a sudden disappearance at the edge of vision. Never below 0.15: a toast that has
		// faded out but not yet expired would still be taking up a row, and an invisible
		// row that pushes the others around reads as a glitch.
		const double Remaining = Notifications->FeedLifetimeRealSeconds - (Now - Entry.RaisedAtRealSeconds);
		Row->SetRenderOpacity(static_cast<float>(FMath::Clamp(Remaining / 2.0, 0.15, 1.0)));

		// NEWEST AT THE BOTTOM, which is what append gives: the eye that just looked at the
		// bar is already at the bottom of the screen, and a new toast appearing under the
		// last one it read is the shortest distance for it to travel.
		ToastColumn->AddChild(Row);
	}
}

int32 UToastStackWidget::ToastCountForTest() const
{
	return ToastColumn != nullptr ? ToastColumn->GetChildrenCount() : 0;
}
