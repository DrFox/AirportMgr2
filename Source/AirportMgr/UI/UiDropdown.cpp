#include "UI/UiDropdown.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/MenuAnchor.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

void UUiDropdownEntry::HandleClicked()
{
	if (UUiDropdown* D = Owner.Get())
	{
		D->Choose(Index);
	}
}

void UUiDropdownList::Build(const UUIStyle& Style, const TArray<FText>& Options, UUiDropdown& Owner)
{
	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("DropdownCard"));
	Card->SetBrush(FSlateRoundedBoxBrush(Style.Surface, Style.ControlRadius, Style.Rule, 1.0f));
	Card->SetPadding(FMargin(4.0f));
	WidgetTree->RootWidget = Card;
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Card->SetContent(Column);
	for (int32 I = 0; I < Options.Num(); ++I)
	{
		UUiButton* B = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		B->SetLabel(Options[I]);
		B->Build(Style, EUiButtonKind::Ghost);
		UUiDropdownEntry* Entry = NewObject<UUiDropdownEntry>(this);
		Entry->Index = I;
		Entry->Owner = &Owner;
		B->OnClicked.AddDynamic(Entry, &UUiDropdownEntry::HandleClicked);
		Column->AddChildToVerticalBox(B)->SetHorizontalAlignment(HAlign_Fill);
		Entries.Add(Entry);
	}
}

void UUiDropdown::Build(const UUIStyle& InStyle, const TArray<FText>& InOptions)
{
	Style = &InStyle;
	Options = InOptions;
	Anchor = WidgetTree->ConstructWidget<UMenuAnchor>(UMenuAnchor::StaticClass(), TEXT("DropdownAnchor"));
	Anchor->SetPlacement(MenuPlacement_ComboBox);
	Anchor->OnGetUserMenuContentEvent.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UUiDropdown, BuildMenu));
	WidgetTree->RootWidget = Anchor;
	Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("DropdownButton"));
	// A placeholder label: UUiButton makes its text block only for a non-empty label, and
	// SetSelected below retitles that block with the real choice.
	Button->SetLabel(FText::FromString(TEXT(" ")));
	Button->Build(InStyle, EUiButtonKind::Secondary);
	Button->OnClicked.AddDynamic(this, &UUiDropdown::HandleOpenClicked);
	Anchor->SetContent(Button);
	SetSelected(0);
}

void UUiDropdown::SetSelected(int32 Index, bool bBroadcast)
{
	// CLAMPED, the segmented control's reason: a saved index past the end still lands on a choice.
	Selected = Options.Num() > 0 ? FMath::Clamp(Index, 0, Options.Num() - 1) : 0;
	if (Button != nullptr && Options.IsValidIndex(Selected))
	{
		// The arrow says "this opens a list" - without it the control reads as a plain button.
		Button->SetLabel(FText::Format(INVTEXT("{0}  ▾"), Options[Selected]));
	}
	if (bBroadcast)
	{
		++Broadcasts;
		OnSelectionChanged.Broadcast(Selected);
	}
}

void UUiDropdown::Choose(int32 Index)
{
	if (Anchor != nullptr && Anchor->IsOpen())
	{
		Anchor->Close();
	}
	const int32 Before = Selected;
	SetSelected(Index);
	if (Selected != Before)
	{
		++Broadcasts;
		OnSelectionChanged.Broadcast(Selected);
	}
}

UUserWidget* UUiDropdown::BuildMenu()
{
	if (Style == nullptr)
	{
		return nullptr;
	}
	UUiDropdownList* List = CreateWidget<UUiDropdownList>(this, UUiDropdownList::StaticClass());
	List->Build(*Style, Options, *this);
	return List;
}

void UUiDropdown::HandleOpenClicked()
{
	if (Anchor != nullptr)
	{
		Anchor->Open(/*bFocusMenu=*/true);
	}
}

FString UUiDropdown::LabelForTest() const
{
	return Button != nullptr && Button->GetLabel() != nullptr ? Button->GetLabel()->GetText().ToString() : FString();
}
