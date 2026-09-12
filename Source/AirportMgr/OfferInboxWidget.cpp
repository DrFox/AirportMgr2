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
#include "Components/VerticalBox.h"
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

bool UOfferInboxWidget::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || bBuilt || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	bBuilt = true;
	Inbox = NewObject<UOfferInboxViewModel>(this);
	EnsureSlots();

	// THE ROOT IS NEVER COLLAPSED, for the reason UInspectorWidget's own comment gives: Slate
	// ticks a widget from its paint pass, so a collapsed widget never ticks, and the tick is
	// the only thing that would un-collapse it.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	return bOk;
}

void UOfferInboxWidget::EnsureSlots()
{
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();   // never null

	// Code-built chrome only where the asset gave none - the same rule as the bar and the
	// inspector. A TOP-right card: a title with a count, then one row per offer. Top, not
	// bottom, because the feed owns the bottom-right corner now and the two-row bar is tall
	// enough to have swallowed the old placement (spec section 6.2).
	if (WidgetTree->RootWidget == nullptr)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("InboxRoot"));
		WidgetTree->RootWidget = Root;

		UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("InboxCard"));
		Card->SetBrushColor(Style->Panel);
		Card->SetPadding(FMargin(12.0f, 10.0f));

		UCanvasPanelSlot* CardSlot = Root->AddChildToCanvas(Card);
		CardSlot->SetAnchors(FAnchors(1.0f, 0.0f, 1.0f, 0.0f));
		CardSlot->SetAlignment(FVector2D(1.0, 0.0));
		CardSlot->SetAutoSize(true);
		CardSlot->SetPosition(FVector2D(-12.0, TopOffset));

		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("InboxColumn"));
		Card->SetContent(Column);

		TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InboxTitle"));
		TitleText->SetText(NSLOCTEXT("AirportMgr", "InboxTitle", "OFFERS"));
		TitleText->SetColorAndOpacity(FSlateColor(Style->TextMuted));
		FSlateFontInfo TitleFont = Style->LabelFont.HasValidFont() ? Style->LabelFont : TitleText->GetFont();
		TitleFont.Size = 9;
		TitleFont.LetterSpacing = 120;   // the same heading treatment the bar's sections take
		TitleText->SetFont(TitleFont);
		Column->AddChildToVerticalBox(TitleText);

		BadgeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("InboxBadge"));
		BadgeText->SetColorAndOpacity(FSlateColor(Style->Text));
		Column->AddChildToVerticalBox(BadgeText);

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
		BadgeText->SetText(FText::AsNumber(Inbox->GetPendingCount()));
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

	// The code-only path. Rebuilt when the COUNT changes rather than every tick: a rebuild
	// every frame would drop a half-pressed button and churn the widget tree.
	const UUIStyle* Style = UAirportMgrUISettings::ResolveStyle();

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

			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(
				UHorizontalBox::StaticClass(), *FString::Printf(TEXT("OfferRow%d"), Index));

			UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(
				UTextBlock::StaticClass(), *FString::Printf(TEXT("OfferLabel%d"), Index));
			Label->SetColorAndOpacity(FSlateColor(Style->Text));
			Row->AddChildToHorizontalBox(Label)->SetVerticalAlignment(VAlign_Center);

			UButton* AcceptButton = WidgetTree->ConstructWidget<UButton>(
				UButton::StaticClass(), *FString::Printf(TEXT("OfferAccept%d"), Index));
			AcceptButton->OnClicked.AddDynamic(Entry, &UOfferRowEntry::HandleAccept);
			UTextBlock* AcceptLabel = WidgetTree->ConstructWidget<UTextBlock>(
				UTextBlock::StaticClass(), *FString::Printf(TEXT("OfferAcceptText%d"), Index));
			AcceptLabel->SetText(NSLOCTEXT("AirportMgr", "OfferAccept", "Accept"));
			AcceptLabel->SetColorAndOpacity(FSlateColor(Style->PanelDark));
			AcceptButton->AddChild(AcceptLabel);
			Row->AddChildToHorizontalBox(AcceptButton)->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));

			UButton* DeclineButton = WidgetTree->ConstructWidget<UButton>(
				UButton::StaticClass(), *FString::Printf(TEXT("OfferDecline%d"), Index));
			DeclineButton->OnClicked.AddDynamic(Entry, &UOfferRowEntry::HandleDecline);
			UTextBlock* DeclineLabel = WidgetTree->ConstructWidget<UTextBlock>(
				UTextBlock::StaticClass(), *FString::Printf(TEXT("OfferDeclineText%d"), Index));
			DeclineLabel->SetText(NSLOCTEXT("AirportMgr", "OfferDecline", "Decline"));
			DeclineLabel->SetColorAndOpacity(FSlateColor(Style->Text));
			DeclineButton->SetBackgroundColor(Style->Button);
			DeclineButton->AddChild(DeclineLabel);
			Row->AddChildToHorizontalBox(DeclineButton)->SetPadding(FMargin(6.0f, 0.0f, 0.0f, 0.0f));

			OfferColumn->AddChildToVerticalBox(Row);
		}
	}

	// Text and enabled state are repainted every refresh, because the ETA counts down and a
	// stand freeing makes a greyed-out offer acceptable again without the count changing.
	for (int32 Index = 0; Index < Rows.Num() && Index < OfferColumn->GetChildrenCount(); ++Index)
	{
		const UOfferViewModel* Row = Rows[Index];
		UHorizontalBox* Box = Cast<UHorizontalBox>(OfferColumn->GetChildAt(Index));
		if (Row == nullptr || Box == nullptr || Box->GetChildrenCount() < 3)
		{
			continue;
		}

		if (UTextBlock* Label = Cast<UTextBlock>(Box->GetChildAt(0)))
		{
			Label->SetText(FText::Format(
				NSLOCTEXT("AirportMgr", "OfferRow", "{0}  {1}  {2}  {3}"),
				Row->GetAirline(), Row->GetTypeName(), Row->GetEta(), Row->GetRefusal()));
		}
		if (UButton* AcceptButton = Cast<UButton>(Box->GetChildAt(1)))
		{
			// DISABLED, not hidden: the player needs to see the offer and the reason it
			// cannot be taken, which is what tells them to build another stand.
			AcceptButton->SetIsEnabled(Row->IsAcceptable());

			// ACCEPT IS THE ONE THING ON THIS CARD THAT TAKES ACCENT. The bar spends that
			// colour on the armed tool and nothing else; here it is the affirmative verb,
			// and the two never share a screen region - a Decline in the same yellow would
			// cost the player the glance that tells the two buttons apart.
			AcceptButton->SetBackgroundColor(Row->IsAcceptable() ? Style->Accent : Style->Button);
		}
	}
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
