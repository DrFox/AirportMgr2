#include "LedgerPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "LedgerViewModels.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "UIStyle.h"

#define LOCTEXT_NAMESPACE "Ledger"

void ULedgerPanelWidget::BuildOnce(const UUIStyle& Style)
{
	// PanelStyle is the base class's now (issue #187) - set before this runs.
	Panel = NewObject<ULedgerPanelViewModel>(this);

	EnsureSlots(&Style);

	// SelfHitTestInvisible on the ROOT and Collapsed on the CARD, the split
	// UAirportMgrPanelWidget::BuildOnce documents: the root must stay laid out or the panel
	// never gets another tick to un-hide itself with.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetCardShown(false);
}

void ULedgerPanelWidget::EnsureSlots(const UUIStyle* Style)
{
	// TOP RIGHT, UNDER THE INBOX. The inbox owns that corner and must never be covered - see
	// its TopOffset comment, missing an offer costs money - so this sits below it. The card is
	// found by name afterwards so the Blueprint path, where EnsureCardRoot returns null, still
	// has something for Toggle to hide.
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureCardRoot(TEXT("LedgerCard"),
		FAnchors(1.0f, 0.0f, 1.0f, 0.0f), FVector2D(1.0, 0.0), FVector2D(-12.0, TopOffset), true)))
	{
		UHorizontalBox* HeadingRow = WidgetTree->ConstructWidget<UHorizontalBox>(
			UHorizontalBox::StaticClass(), TEXT("LedgerHeading"));

		if (TitleText == nullptr)
		{
			TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
				TEXT("TitleText"));
			TitleText->SetText(LOCTEXT("LedgerTitle", "LEDGER"));
			Style->ApplyText(*TitleText, EUITextRole::Heading, Style->TextMuted);
			HeadingRow->AddChildToHorizontalBox(TitleText);
		}
		if (BalanceText == nullptr)
		{
			BalanceText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),
				TEXT("BalanceText"));
			Style->ApplyText(*BalanceText, EUITextRole::Title, Style->Text);
			UHorizontalBoxSlot* BalanceSlot = HeadingRow->AddChildToHorizontalBox(BalanceText);
			BalanceSlot->SetPadding(FMargin(16.0f, 0.0f, 0.0f, 0.0f));
		}
		Column->AddChildToVerticalBox(HeadingRow);

		if (RowColumn == nullptr)
		{
			RowColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),
				TEXT("RowColumn"));
			UVerticalBoxSlot* RowsSlot = Column->AddChildToVerticalBox(RowColumn);
			RowsSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		}
	}
	// CardWidget is found and cached by EnsureCardRoot itself now (issue #187) - see its own comment.
}

void ULedgerPanelWidget::Toggle()
{
	bShowing = !bShowing;
	SetCardShown(bShowing);

	// REPAINTED ON OPEN, not left to the next tick. A panel that appeared empty for a frame
	// and then filled would read as a bug in the ledger rather than as a frame of latency.
	if (bShowing)
	{
		Refresh();
	}
}

void ULedgerPanelWidget::Refresh()
{
	if (Panel == nullptr)
	{
		return;
	}

	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	const ULedger* Ledger = Runtime != nullptr ? Runtime->GetLedger() : nullptr;
	const UPricing* Pricing = Runtime != nullptr ? Runtime->GetPricing() : nullptr;
	const USimClock* Clock = Runtime != nullptr ? Runtime->GetClock() : nullptr;
	if (Ledger == nullptr || Pricing == nullptr || Clock == nullptr)
	{
		return;
	}

	// THE GATE. Refresh returns whether it rebuilt, and the rows are only repainted when it
	// did - re-deriving forty rows every tick to discover nothing had happened is the
	// expensive kind of correct. See ULedger::Revision.
	const bool bRebuilt = Panel->Refresh(*Ledger, *Clock, *Pricing);

	if (BalanceText != nullptr)
	{
		BalanceText->SetText(Panel->GetBalance());
		if (PanelStyle != nullptr)
		{
			BalanceText->SetColorAndOpacity(FSlateColor(
				Panel->IsOverdrawn() ? PanelStyle->Warning : PanelStyle->Text));
		}
	}

	if (bRebuilt)
	{
		PaintRows();
	}
}

void ULedgerPanelWidget::PaintRows()
{
	if (RowColumn == nullptr || Panel == nullptr || PanelStyle == nullptr)
	{
		return;
	}

	// REBUILT WHOLE rather than diffed. The row count changes on nearly every post - a new
	// entry pushes the oldest off the bottom - so a diff would be a loop that almost always
	// touched every row anyway, with an index-matching invariant to get wrong.
	RowColumn->ClearChildren();
	for (const TObjectPtr<ULedgerRowViewModel>& Row : Panel->Rows())
	{
		if (Row != nullptr)
		{
			RowColumn->AddChildToVerticalBox(BuildRow(*PanelStyle, *Row));
		}
	}
}

UWidget* ULedgerPanelWidget::BuildRow(const UUIStyle& Style, const ULedgerRowViewModel& Row)
{
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());

	// FIXED COLUMN WIDTHS, through SizeBox. Without them each row sizes to its own text and
	// the four fields rag down the panel - which is exactly what makes a table of numbers
	// unreadable, and this table exists to be read.
	auto AddCell = [&](const FText& Text, float Width, EUITextRole Role, FLinearColor Colour,
		bool bRightAlign)
	{
		UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Block->SetText(Text);
		Style.ApplyText(*Block, Role, Colour);

		if (Width <= 0.0f)
		{
			// The What column takes whatever is left, so a long description is not truncated
			// by a width nobody chose.
			UHorizontalBoxSlot* FillSlot = Line->AddChildToHorizontalBox(Block);
			FillSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			FillSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
			return;
		}

		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->SetContent(Block);
		if (bRightAlign)
		{
			Block->SetJustification(ETextJustify::Right);
		}
		UHorizontalBoxSlot* CellSlot = Line->AddChildToHorizontalBox(Box);
		CellSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
	};

	AddCell(Row.GetWhen(), WhenWidth, EUITextRole::Label, Style.TextMuted, false);
	AddCell(Row.GetCategory(), CategoryWidth, EUITextRole::Label, Style.TextMuted, false);
	AddCell(Row.GetWhat(), 0.0f, EUITextRole::Label, Style.Text, false);

	// THE ONE PIECE OF COLOUR IN THE ROW, and it is semantic: money out is Warning, money in
	// is Positive, both from the style's slots rather than a literal. Read off the viewmodel's
	// bool and never by parsing a minus sign back out of formatted text.
	AddCell(Row.GetAmount(), AmountWidth, EUITextRole::Label,
		Row.IsOutgoing() ? Style.Warning : Style.Positive, true);

	return Line;
}

int32 ULedgerPanelWidget::RowWidgetCountForTest() const
{
	return RowColumn != nullptr ? RowColumn->GetChildrenCount() : 0;
}

void ULedgerPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// ONLY WHILE OPEN. A closed panel costs nothing - the ledger's own revision gate would
	// make the work cheap anyway, but a panel nobody is looking at should not be asking.
	if (bShowing)
	{
		Refresh();
	}
}

#undef LOCTEXT_NAMESPACE
