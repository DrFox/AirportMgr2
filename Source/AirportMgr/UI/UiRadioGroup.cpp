#include "UI/UiRadioGroup.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

void UUiRadioEntry::HandleClicked()
{
	if (UUiRadioGroup* Group = Owner.Get())
	{
		Group->Choose(Index);
	}
}

void UUiRadioGroup::Build(const UUIStyle& InStyle, const TArray<FText>& Options)
{
	Style = &InStyle;
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	WidgetTree->RootWidget = Row;
	for (int32 I = 0; I < Options.Num(); ++I)
	{
		// NAMED by index, so a test can click the segment the player would.
		UUiButton* B = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), *FString::Printf(TEXT("Segment%d"), I));
		B->SetLabel(Options[I]);
		B->Build(InStyle, EUiButtonKind::Secondary);
		UUiRadioEntry* Entry = NewObject<UUiRadioEntry>(this);
		Entry->Index = I;
		Entry->Owner = this;
		B->OnClicked.AddDynamic(Entry, &UUiRadioEntry::HandleClicked);
		UHorizontalBoxSlot* BSlot = Row->AddChildToHorizontalBox(B);
		BSlot->SetPadding(FMargin(I == 0 ? 0.0f : 2.0f, 0.0f, 0.0f, 0.0f));
		Buttons.Add(B);
		Entries.Add(Entry);
	}
	Paint();
}

void UUiRadioGroup::SetSelected(int32 Index, bool bBroadcast)
{
	// CLAMPED, not ignored: a saved index past the end (a quality level from a build with more)
	// still lands on a real choice, and exactly one segment stays lit (Review Focus 4).
	Selected = Buttons.Num() > 0 ? FMath::Clamp(Index, 0, Buttons.Num() - 1) : 0;
	Paint();
	if (bBroadcast)
	{
		++Broadcasts;
		OnSelectionChanged.Broadcast(Selected);
	}
}

void UUiRadioGroup::Choose(int32 Index)
{
	if (Index == Selected)
	{
		return;   // already lit: nothing changed, so nothing to tell anyone
	}
	SetSelected(Index, /*bBroadcast=*/true);
}

void UUiRadioGroup::Paint()
{
	for (int32 I = 0; I < Buttons.Num(); ++I)
	{
		if (Buttons[I] != nullptr)
		{
			Buttons[I]->SetState(true, I == Selected);
		}
	}
}

int32 UUiRadioGroup::SelectedButtonCountForTest() const
{
	int32 Lit = 0;
	for (const UUiButton* B : Buttons)
	{
		// The lit segment is the one whose fill is Accent - UUiButton::LookFor's "selected".
		if (B != nullptr && Style != nullptr && B->GetBackgroundColor().Equals(Style->Accent))
		{
			++Lit;
		}
	}
	return Lit;
}
