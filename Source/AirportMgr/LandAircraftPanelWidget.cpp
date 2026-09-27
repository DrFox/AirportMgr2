#include "LandAircraftPanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Entities/AircraftType.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildController.h"
#include "RoadBuildLog.h"
#include "Styling/SlateBrush.h"
#include "UIStyle.h"

#define LOCTEXT_NAMESPACE "LandPanel"

void ULandRowEntry::HandleClick()
{
	if (ULandAircraftPanelWidget* Panel = Owner.Get())
	{
		Panel->Choose(Type);
	}
}

void ULandAircraftPanelWidget::BuildOnce(const UUIStyle& Style)
{
	// TOP LEFT. The offer inbox owns the top right and the ledger sits under it; the bar owns
	// the bottom. Found by name afterwards, as the ledger's card is, so the Blueprint path -
	// where EnsureCardRoot returns null - still has something for Toggle to hide.
	if (UVerticalBox* Column = Cast<UVerticalBox>(EnsureCardRoot(TEXT("LandCard"),
		FAnchors(0.0f, 0.0f, 0.0f, 0.0f), FVector2D(0.0, 0.0), FVector2D(12.0, TopOffset), true)))
	{
		if (TitleText == nullptr)
		{
			TitleText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TitleText"));
			TitleText->SetText(LOCTEXT("Title", "LAND AN AIRCRAFT"));
			Style.ApplyText(*TitleText, EUITextRole::Heading, Style.TextMuted);
			Column->AddChildToVerticalBox(TitleText);
		}
		if (RowColumn == nullptr)
		{
			RowColumn = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RowColumn"));
			UVerticalBoxSlot* RowsSlot = Column->AddChildToVerticalBox(RowColumn);
			RowsSlot->SetPadding(FMargin(0.0f, Style.RowGap, 0.0f, 0.0f));
		}
	}

	// SelfHitTestInvisible on the ROOT and Collapsed on the CARD, the split
	// UAirportMgrPanelWidget::BuildOnce documents: the root must stay laid out or the panel
	// never gets another tick to un-hide itself with.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	SetCardShown(false);
}

void ULandAircraftPanelWidget::Toggle()
{
	bShowing = !bShowing;
	SetCardShown(bShowing);

	// JUDGED ON OPEN, not left to the next tick - a panel that appeared empty for a frame and
	// then filled reads as a bug, the ledger's reasoning.
	if (bShowing)
	{
		Refresh();
	}
}

void ULandAircraftPanelWidget::Refresh()
{
	// READ ONCE, on first open. Types are content: they do not appear mid-session, and a
	// registry walk per tick would be the one expensive thing on this panel.
	if (Types.Num() == 0)
	{
		for (UAircraftType* Type : LandChoices::EveryMeshedType())
		{
			Types.Add(Type);
		}
	}

	const ARoadBuildController* C = Controller();
	const ARoadNetworkActor* Target = C != nullptr ? C->GetTarget() : nullptr;
	const URoadNetwork* Network = Target != nullptr ? Target->Network.Get() : nullptr;
	const FVector2D Focus = C != nullptr ? C->GetViewFocus() : FVector2D::ZeroVector;

	TArray<UAircraftType*> Raw;
	Raw.Reserve(Types.Num());
	for (const TObjectPtr<UAircraftType>& Type : Types)
	{
		Raw.Add(Type.Get());
	}
	const TArray<FLandChoice> Choices = LandChoices::Build(Network, Focus, Raw);

	// THE GATE - see PaintedRefusals.
	TArray<FString> Refusals;
	Refusals.Reserve(Choices.Num());
	for (const FLandChoice& Choice : Choices)
	{
		Refusals.Add(Choice.Refusal);
	}
	if (Refusals != PaintedRefusals || Entries.Num() != Choices.Num())
	{
		PaintRows(Choices);
		PaintedRefusals = MoveTemp(Refusals);
	}
}

void ULandAircraftPanelWidget::PaintRows(const TArray<FLandChoice>& Choices)
{
	if (RowColumn == nullptr || PanelStyle == nullptr)
	{
		return;
	}
	const UUIStyle& Style = *PanelStyle;

	// REBUILT WHOLE, as the ledger's rows are: when the runway changes, most rows' state
	// changes with it, so a diff would touch nearly everything with an index invariant to
	// get wrong.
	RowColumn->ClearChildren();
	Entries.Reset();

	for (const FLandChoice& Choice : Choices)
	{
		ULandRowEntry* Entry = NewObject<ULandRowEntry>(this);
		Entry->Type = Choice.Type;
		Entry->Owner = this;

		UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		// Rounded, white brushes tinted by SetBackgroundColor - UOfferInboxWidget::
		// MakeAnswerButton's recipe and its reason.
		FButtonStyle ButtonStyle = Button->GetStyle();
		const FSlateRoundedBoxBrush Rounded(FLinearColor::White, Style.CornerRadius);
		ButtonStyle.SetNormal(Rounded);
		ButtonStyle.SetHovered(Rounded);
		ButtonStyle.SetPressed(Rounded);
		ButtonStyle.SetDisabled(Rounded);
		ButtonStyle.SetNormalPadding(Style.ButtonPadding);
		ButtonStyle.SetPressedPadding(Style.ButtonPadding);
		Button->SetStyle(ButtonStyle);
		Button->SetBackgroundColor(Choice.bAdmitted ? Style.Button : Style.PanelDark);
		// DISABLED, not merely tinted: a greyed row is a click the arrival would refuse, so
		// it must not be clickable at all.
		Button->SetIsEnabled(Choice.bAdmitted);
		Button->OnClicked.AddDynamic(Entry, &ULandRowEntry::HandleClick);
		Entry->Button = Button;

		UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UTextBlock* Name = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Name->SetText(Choice.Label);
		Style.ApplyText(*Name, EUITextRole::Label, Choice.bAdmitted ? Style.Text : Style.TextMuted);
		USizeBox* NameBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		NameBox->SetWidthOverride(NameWidth);
		NameBox->SetContent(Name);
		Line->AddChildToHorizontalBox(NameBox);

		if (!Choice.Refusal.IsEmpty())
		{
			UTextBlock* Why = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Why->SetText(FText::FromString(Choice.Refusal));
			Style.ApplyText(*Why, EUITextRole::Label, Style.TextMuted);
			Line->AddChildToHorizontalBox(Why);
		}
		Button->AddChild(Line);

		UVerticalBoxSlot* RowSlot = RowColumn->AddChildToVerticalBox(Button);
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
		Entries.Add(Entry);
	}
}

void ULandAircraftPanelWidget::Choose(UAircraftType* Type)
{
	if (Type == nullptr)
	{
		return;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Land panel: %s chosen"), *Type->GetName());
	if (ARoadBuildController* C = Controller())
	{
		ChooseFor(*C, Type);
	}
}

void ULandAircraftPanelWidget::ChooseFor(ARoadBuildController& C, UAircraftType* Type)
{
	C.LandAircraftNearViewFocus(Type);
}

int32 ULandAircraftPanelWidget::RowWidgetCountForTest() const
{
	return RowColumn != nullptr ? RowColumn->GetChildrenCount() : 0;
}

int32 ULandAircraftPanelWidget::RowIndexOfForTest(const TCHAR* AssetName) const
{
	return Entries.IndexOfByPredicate([AssetName](const TObjectPtr<ULandRowEntry>& Entry)
	{
		return Entry != nullptr && Entry->Type != nullptr && Entry->Type->GetName() == AssetName;
	});
}

bool ULandAircraftPanelWidget::IsRowBoundForTest(int32 Index) const
{
	return Entries.IsValidIndex(Index) && Entries[Index] != nullptr && Entries[Index]->Button != nullptr
		&& Entries[Index]->Button->OnClicked.Contains(Entries[Index].Get(),
			GET_FUNCTION_NAME_CHECKED(ULandRowEntry, HandleClick));
}

void ULandAircraftPanelWidget::ClickRowForTest(int32 Index, ARoadBuildController& C)
{
	if (Entries.IsValidIndex(Index) && Entries[Index] != nullptr)
	{
		ChooseFor(C, Entries[Index]->Type);
	}
}

void ULandAircraftPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// ONLY WHILE OPEN - a closed panel asks nothing.
	if (bShowing)
	{
		Refresh();
	}
}

#undef LOCTEXT_NAMESPACE
