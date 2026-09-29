#include "UI/UiMenuButton.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/MenuAnchor.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

void UUiMenuEntry::HandleClicked()
{
	if (UUiMenuButton* M = Owner.Get())
	{
		M->Choose(Index);
	}
}

void UUiMenuList::Build(const UUIStyle& Style, const TArray<FUiMenuItem>& Items, UUiMenuButton& Owner)
{
	// UUiDropdownList's card, deliberately the same look: a popup is a popup.
	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("MenuCard"));
	Card->SetBrush(FSlateRoundedBoxBrush(Style.Surface, Style.ControlRadius, Style.Rule, 1.0f));
	Card->SetPadding(FMargin(4.0f));
	WidgetTree->RootWidget = Card;
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Card->SetContent(Column);
	for (int32 I = 0; I < Items.Num(); ++I)
	{
		// NAMED by index, so a test can click the line the player would.
		UUiButton* B = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), *FString::Printf(TEXT("Line%d"), I));
		B->SetLabel(Items[I].Label);
		B->Build(Style, EUiButtonKind::Ghost);
		// GREYED BY ITS INK, the bar's rule (UUiButton::LookFor), and the reason on hover.
		B->SetState(Items[I].bEnabled, false);
		if (!Items[I].bEnabled && !Items[I].Why.IsEmpty())
		{
			B->SetToolTipText(Items[I].Why);
		}
		UUiMenuEntry* Entry = NewObject<UUiMenuEntry>(this);
		Entry->Index = I;
		Entry->Owner = &Owner;
		B->OnClicked.AddDynamic(Entry, &UUiMenuEntry::HandleClicked);
		Column->AddChildToVerticalBox(B)->SetHorizontalAlignment(HAlign_Fill);
		Entries.Add(Entry);
		Lines.Add(B);
	}
}

void UUiMenuList::SetLineLabel(int32 Index, const FText& Label)
{
	if (Lines.IsValidIndex(Index) && Lines[Index] != nullptr)
	{
		Lines[Index]->SetLabel(Label);
	}
}

void UUiMenuButton::Build(const UUIStyle& InStyle, const FText& Label)
{
	Style = &InStyle;
	Anchor = WidgetTree->ConstructWidget<UMenuAnchor>(UMenuAnchor::StaticClass(), TEXT("MenuAnchor"));
	Anchor->SetPlacement(MenuPlacement_ComboBox);
	Anchor->OnGetUserMenuContentEvent.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UUiMenuButton, BuildMenu));
	Anchor->OnMenuOpenChanged.AddDynamic(this, &UUiMenuButton::HandleOpenChanged);
	WidgetTree->RootWidget = Anchor;
	Button = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("MenuButton"));
	Button->SetLabel(Label);
	Button->Build(InStyle, EUiButtonKind::Secondary);
	Button->OnClicked.AddDynamic(this, &UUiMenuButton::HandleOpenClicked);
	Anchor->SetContent(Button);
}

void UUiMenuButton::Open()
{
	if (Anchor != nullptr && !Anchor->IsOpen())
	{
		Anchor->Open(/*bFocusMenu=*/true);
	}
}

void UUiMenuButton::Close()
{
	if (Anchor != nullptr && Anchor->IsOpen())
	{
		Anchor->Close();
	}
	Armed = INDEX_NONE;
}

bool UUiMenuButton::IsOpen() const
{
	return Anchor != nullptr && Anchor->IsOpen();
}

void UUiMenuButton::Choose(int32 Index)
{
	if (!Shown.IsValidIndex(Index) || !Shown[Index].bEnabled)
	{
		return;
	}
	if (Shown[Index].bConfirm && Armed != Index)
	{
		Armed = Index;
		if (OpenList != nullptr)
		{
			OpenList->SetLineLabel(Index, Shown[Index].ConfirmLabel.IsEmpty() ? Shown[Index].Label : Shown[Index].ConfirmLabel);
		}
		return;
	}
	Close();
	++Chosen;
	LastChosen = Index;
	OnChosen.Broadcast(Index);
}

UUserWidget* UUiMenuButton::BuildMenu()
{
	if (Style == nullptr)
	{
		return nullptr;
	}
	Shown = Items ? Items() : TArray<FUiMenuItem>();
	Armed = INDEX_NONE;
	OpenList = CreateWidget<UUiMenuList>(this, UUiMenuList::StaticClass());
	OpenList->Build(*Style, Shown, *this);
	return OpenList;
}

void UUiMenuButton::HandleOpenClicked()
{
	if (IsOpen())
	{
		Close();
	}
	else
	{
		Open();
	}
}

void UUiMenuButton::HandleOpenChanged(bool bIsOpen)
{
	// DISARMED ON ANY CLOSE - a click away, Escape, the anchor losing focus - not only Close(): an
	// armed Despawn left waiting for the next time the popup opens is a one-click delete again.
	if (!bIsOpen)
	{
		Armed = INDEX_NONE;
		OpenList = nullptr;
	}
}
