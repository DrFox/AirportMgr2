#include "InspectorFacilityRows.h"

#include "Blueprint/WidgetTree.h"
#include "BuildActions.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "InspectorLog.h"
#include "RoadBuildController.h"
#include "UI/UiButton.h"
#include "UIStyle.h"

namespace
{
	/** THE ONE TABLE of the BuildActions rows the purchase rows run, in EAction order: OwnsAction and Build's search by id both
	 *  read it. Prefixed against the unity build. */
	const TCHAR* const InspectorFacilityActionIds[] = {
		TEXT("selection.buy_module"), TEXT("selection.buy_vehicle"), TEXT("selection.sell_vehicle") };
	static_assert(UE_ARRAY_COUNT(InspectorFacilityActionIds) == static_cast<int32>(UInspectorFacilityRows::EAction::Count),
		"an EAction needs its BuildActions id in the table, in the same order");
}

bool UInspectorFacilityRows::OwnsAction(FName Id)
{
	for (const TCHAR* Each : InspectorFacilityActionIds)
	{
		if (Id == FName(Each)) { return true; }
	}
	return false;
}

void UInspectorFacilityRows::Build(const UUIStyle& InStyle)
{
	Style = &InStyle;
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		if (Actions[Index].Section != EActionSection::Selection) { continue; }
		for (int32 Each = 0; Each < UE_ARRAY_COUNT(InspectorFacilityActionIds); ++Each)
		{
			if (Actions[Index].Id == FName(InspectorFacilityActionIds[Each])) { ActionIndex[Each] = Index; }
		}
	}

	// THE PURCHASE ROWS, built empty; Show fills them from the one quote. A root column only where the asset gave none -
	// the inspector's own EnsureContentRoot rule: a designer's layout is not replaced.
	UVerticalBox* Column = nullptr;
	if (WidgetTree->RootWidget == nullptr)
	{
		Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("FacilityRows"));
		WidgetTree->RootWidget = Column;
	}
	auto Panel = [&](TObjectPtr<UPanelWidget>& Field, UClass* Class, const TCHAR* Name)
	{
		if (Field != nullptr || Column == nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UPanelWidget>(Class, Name);
		Column->AddChildToVerticalBox(Field)->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		Field->SetVisibility(ESlateVisibility::Collapsed);
	};
	// Only into a row that exists: with an asset's content and no code-built row, a text block built here
	// would be parented nowhere and never drawn.
	auto RowText = [&](TObjectPtr<UTextBlock>& Field, const TCHAR* Name, UPanelWidget* Into)
	{
		UHorizontalBox* Box = Cast<UHorizontalBox>(Into);
		if (Field != nullptr || Box == nullptr) { return; }
		Field = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
		InStyle.ApplyText(*Field, EUITextRole::Body, InStyle.InkMuted);
		Box->AddChildToHorizontalBox(Field)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
	};
	Panel(ShedsRow, UHorizontalBox::StaticClass(), TEXT("ShedsRow"));
	RowText(ShedsText, TEXT("ShedsText"), ShedsRow);
	// RowText's rule: built only into a row that exists, never an orphan.
	if (UHorizontalBox* Box = Cast<UHorizontalBox>(ShedsRow); Box != nullptr && BuyModuleButton == nullptr && Actions.IsValidIndex(IndexOf(EAction::BuyModule)))
	{
		BuyModuleButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("BuyModuleButton"));
		BuyModuleButton->SetLabel(Actions[IndexOf(EAction::BuyModule)].Label);
		BuyModuleButton->Build(InStyle, EUiButtonKind::Secondary);
		Box->AddChildToHorizontalBox(BuyModuleButton);
	}
	Panel(VehiclesRow, UHorizontalBox::StaticClass(), TEXT("VehiclesRow"));
	RowText(VehiclesText, TEXT("VehiclesText"), VehiclesRow);
	if (UHorizontalBox* Box = Cast<UHorizontalBox>(VehiclesRow); Box != nullptr && BuyVehicleMenu == nullptr && Actions.IsValidIndex(IndexOf(EAction::BuyVehicle)))
	{
		BuyVehicleMenu = WidgetTree->ConstructWidget<UUiMenuButton>(UUiMenuButton::StaticClass(), TEXT("BuyVehicleMenu"));
		BuyVehicleMenu->Build(InStyle, Actions[IndexOf(EAction::BuyVehicle)].Label);
		Box->AddChildToHorizontalBox(BuyVehicleMenu);
	}
	Panel(FleetList, UVerticalBox::StaticClass(), TEXT("FleetList"));
	for (const int32 Found : ActionIndex)
	{
		if (!Actions.IsValidIndex(Found))
		{
			UE_LOG(LogInspector, Warning, TEXT("A selection.buy_module/buy_vehicle/sell_vehicle row is missing from BuildActions(): the depot card cannot buy or sell"));
			break;
		}
	}

	if (BuyModuleButton != nullptr) { BuyModuleButton->OnClicked.AddDynamic(this, &UInspectorFacilityRows::HandleBuyModule); }
	if (BuyVehicleMenu != nullptr)
	{
		// WEAK - a lambda held by a child widget that captured a raw pointer to its owner is the shape that dangles the
		// first time either is rebuilt.
		TWeakObjectPtr<UInspectorFacilityRows> WeakSelf(this);
		BuyVehicleMenu->Items = [WeakSelf]() { return WeakSelf.IsValid() ? WeakSelf->BuyVehicleItems() : TArray<FUiMenuItem>(); };
		BuyVehicleMenu->OnChosen.AddDynamic(this, &UInspectorFacilityRows::HandleBuyVehicleChosen);
	}
	// COLLAPSED until a depot card shows a quote (Show): an empty panel takes no room under the card.
	SetVisibility(ESlateVisibility::Collapsed);
}

void UInspectorFacilityRows::Show(const FFacilityQuote& Quote)
{
	LastQuote = Quote;
	const bool bCard = Quote.IsFacility();
	auto Visible = [](UWidget* Widget, bool bShow, ESlateVisibility Shown)
	{
		const ESlateVisibility Wanted = bShow ? Shown : ESlateVisibility::Collapsed;
		if (Widget != nullptr && Widget->GetVisibility() != Wanted) { Widget->SetVisibility(Wanted); }
	};
	auto ShowRow = [&Visible](UWidget* Widget, bool bShow) { Visible(Widget, bShow, ESlateVisibility::Visible); };
	// COMPARED FIRST: SetText has no early-out, and this runs every tick (Refresh's own gate, same reason).
	auto SetIfChanged = [](UTextBlock* Block, const FString& Text)
	{
		if (Block != nullptr && Block->GetText().ToString() != Text) { Block->SetText(FText::FromString(Text)); }
	};
	// THE WIDGET'S OWN ROOT: self-hit-test-invisible, as a UserWidget defaults to, so its gaps let a click through to the window;
	// the buttons inside are Visible and take theirs.
	Visible(this, bCard, ESlateVisibility::SelfHitTestInvisible);
	ShowRow(ShedsRow, bCard && Quote.Modules.Num() > 0);
	ShowRow(VehiclesRow, bCard);
	ShowRow(FleetList, bCard && Quote.Fleet.Num() > 0);

	if (bCard && Quote.Modules.Num() > 0)
	{
		const FModuleOfferQuote& Module = Quote.Modules[0];
		SetIfChanged(ShedsText, FString::Printf(TEXT("%s %d / %d space"), *Module.PluralName.ToString(), Module.Owned, Module.Reserved));
		if (BuyModuleButton != nullptr)
		{
			// A REFUSED BUY SAYS WHY on its own caption - a greyed button with no reason teaches nothing.
			const bool bCan = Module.Refusal == EPurchaseRefusal::None;
			const FText Caption = bCan ? Module.Label
				: FText::Format(NSLOCTEXT("AirportMgr", "InspectorRefusedCaption", "{0} - {1}"), Module.Label, UFacilityPurchases::RefusalText(Module.Refusal));
			const UTextBlock* Current = BuyModuleButton->GetLabel();
			if (Current == nullptr || !Current->GetText().EqualTo(Caption)) { BuyModuleButton->SetLabel(Caption); }
			BuyModuleButton->SetState(bCan, false);
		}
	}
	if (bCard)
	{
		SetIfChanged(VehiclesText, FString::Printf(TEXT("Vehicles %d / %d bays"), Quote.Vehicles, Quote.Bays));
		if (BuyVehicleMenu != nullptr && BuyVehicleMenu->GetButton() != nullptr)
		{
			// The MENU opens whenever there is an offer; each LINE greys with its own reason (BuyVehicleItems).
			BuyVehicleMenu->GetButton()->SetState(Quote.VehicleOffers.Num() > 0, false);
		}
	}

	FString Key;
	for (const FFleetRowQuote& Row : Quote.Fleet) { Key += FString::Printf(TEXT("%d,"), Row.VehicleId); }
	if (Key != LastFleetKey)
	{
		LastFleetKey = Key;
		RebuildFleetRows();
	}
	for (int32 Index = 0; Index < FleetRows.Num() && Index < Quote.Fleet.Num(); ++Index)
	{
		UInspectorFleetRow* Row = FleetRows[Index];
		const FFleetRowQuote& Facts = Quote.Fleet[Index];
		SetIfChanged(Row->Line, Facts.Line);
		const bool bCan = Facts.Refusal == EPurchaseRefusal::None;
		// A vehicle that went busy while armed DISARMS: the confirm was for an idle vehicle.
		if (!bCan) { Row->bArmed = false; }
		const FText Caption = !bCan ? UFacilityPurchases::RefusalText(Facts.Refusal)
			: Row->bArmed ? FText::Format(NSLOCTEXT("AirportMgr", "InspectorSellConfirm", "{0} - click again"), Facts.SellLabel)
			: Facts.SellLabel;
		const UTextBlock* Current = Row->SellButton->GetLabel();
		if (Current == nullptr || !Current->GetText().EqualTo(Caption)) { Row->SellButton->SetLabel(Caption); }
		Row->SellButton->SetState(bCan, Row->bArmed);
	}
}

void UInspectorFacilityRows::OnNewCard()
{
	// A POPUP FOR THE OLD DEPOT closes with its card, and a key no fleet has forces the next depot's rows to rebuild -
	// which disarms any sale armed on the old card (RebuildFleetRows).
	if (BuyVehicleMenu != nullptr) { BuyVehicleMenu->Close(); }
	LastFleetKey = TEXT("!");
	// DISARMED HERE, not only by the rebuild the key above forces: a card that returns early (an
	// agent that vanished) never reaches Show, and the old depot's sale stays armed.
	DisarmSale();
}

void UInspectorFacilityRows::RebuildFleetRows()
{
	// A REBUILD DISARMS: the armed vehicle may be the one that just left the list.
	DisarmSale();
	if (FleetList != nullptr) { FleetList->ClearChildren(); }
	FleetRows.Reset();
	if (FleetList == nullptr || Style == nullptr) { return; }
	for (const FFleetRowQuote& Facts : LastQuote.Fleet)
	{
		UInspectorFleetRow* Row = NewObject<UInspectorFleetRow>(this);
		Row->VehicleId = Facts.VehicleId;
		Row->Owner = this;
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Style->ApplyText(*Row->Line, EUITextRole::Body, Style->InkMuted);
		Row->Line->SetText(FText::FromString(Facts.Line));
		Box->AddChildToHorizontalBox(Row->Line)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		Row->SellButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		Row->SellButton->SetLabel(Facts.SellLabel);
		Row->SellButton->Build(*Style, EUiButtonKind::Secondary);
		Row->SellButton->OnClicked.AddDynamic(Row, &UInspectorFleetRow::HandleSell);
		Box->AddChildToHorizontalBox(Row->SellButton);
		FleetList->AddChild(Box);
		FleetRows.Add(Row);
	}
}

void UInspectorFleetRow::HandleSell()
{
	if (UInspectorFacilityRows* Rows = Owner.Get())
	{
		Rows->OnFleetSell(*this);
	}
}

void UInspectorFacilityRows::OnFleetSell(UInspectorFleetRow& Row)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr)
	{
		UE_LOG(LogInspector, Warning, TEXT("Sell click on vehicle %d ignored: no controller"), Row.VehicleId);
		return;
	}
	if (!Row.bArmed)
	{
		// ONE ARMED ROW: arming a second row disarms the first, so "click again" is never ambiguous.
		for (UInspectorFleetRow* Other : FleetRows) { if (Other != nullptr) { Other->bArmed = false; } }
		Row.bArmed = true;
		C->ArmSellVehicle(Row.VehicleId);
		return;
	}
	Row.bArmed = false;
	if (RunActionSource) { RunActionSource(IndexOf(EAction::SellVehicle)); }
}

TArray<FUiMenuItem> UInspectorFacilityRows::BuyVehicleItems() const
{
	TArray<FUiMenuItem> Out;
	ShownVehicleCodes.Reset();
	for (const FVehicleOfferQuote& Offer : LastQuote.VehicleOffers)
	{
		FUiMenuItem& Item = Out.AddDefaulted_GetRef();
		Item.Label = Offer.Label;
		Item.bEnabled = Offer.Refusal == EPurchaseRefusal::None;
		Item.Why = UFacilityPurchases::RefusalText(Offer.Refusal);
		ShownVehicleCodes.Add(Offer.TypeCode);
	}
	return Out;
}

void UInspectorFacilityRows::HandleBuyVehicleChosen(int32 Index)
{
	ARoadBuildController* C = Controller();
	if (C == nullptr || !ShownVehicleCodes.IsValidIndex(Index))
	{
		UE_LOG(LogInspector, Warning, TEXT("Buy vehicle line %d ignored: no controller or no such line"), Index);
		return;
	}
	C->ChooseVehicleToBuy(ShownVehicleCodes[Index]);
	if (RunActionSource) { RunActionSource(IndexOf(EAction::BuyVehicle)); }
}

void UInspectorFacilityRows::HandleBuyModule()
{
	if (RunActionSource) { RunActionSource(IndexOf(EAction::BuyModule)); }
}

void UInspectorFacilityRows::DisarmSale()
{
	// BOTH HALVES: the row's caption state and the controller's armed id. The rows need no controller, so
	// a panel with none still stops saying "click again".
	for (UInspectorFleetRow* Row : FleetRows) { if (Row != nullptr) { Row->bArmed = false; } }
	if (ARoadBuildController* C = Controller()) { C->ArmSellVehicle(0); }
}

void UInspectorFacilityRows::ClickSellForTest(int32 Row)
{
	// THROUGH THE BUTTON'S DELEGATE, so an unbound Sell goes red here rather than passing.
	if (FleetRows.IsValidIndex(Row) && FleetRows[Row]->SellButton != nullptr) { FleetRows[Row]->SellButton->OnClicked.Broadcast(); }
}

FString UInspectorFacilityRows::SellCaptionForTest(int32 Row) const
{
	const UTextBlock* Caption = FleetRows.IsValidIndex(Row) && FleetRows[Row]->SellButton != nullptr ? FleetRows[Row]->SellButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetText().ToString() : FString();
}

void UInspectorFacilityRows::ClickBuyModuleForTest()
{
	if (BuyModuleButton != nullptr) { BuyModuleButton->OnClicked.Broadcast(); }
}

void UInspectorFacilityRows::ChooseBuyVehicleForTest(int32 Line)
{
	// AS AN OPEN WOULD: the lines are asked first (that is what fills ShownVehicleCodes), then the
	// menu's own delegate raises the choice.
	if (BuyVehicleMenu == nullptr || !BuyVehicleMenu->Items) { return; }
	BuyVehicleMenu->Items();
	BuyVehicleMenu->OnChosen.Broadcast(Line);
}

bool UInspectorFacilityRows::AreFacilityRowsShownForTest() const
{
	return VehiclesRow != nullptr && VehiclesRow->GetVisibility() == ESlateVisibility::Visible;
}
FString UInspectorFacilityRows::ShedsTextForTest() const { return ShedsText != nullptr ? ShedsText->GetText().ToString() : FString(); }
FString UInspectorFacilityRows::VehiclesTextForTest() const { return VehiclesText != nullptr ? VehiclesText->GetText().ToString() : FString(); }
bool UInspectorFacilityRows::IsBuyModuleEnabledForTest() const { return BuyModuleButton != nullptr && BuyModuleButton->GetIsEnabled(); }
FString UInspectorFacilityRows::BuyModuleCaptionForTest() const
{
	const UTextBlock* Caption = BuyModuleButton != nullptr ? BuyModuleButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetText().ToString() : FString();
}
bool UInspectorFacilityRows::IsSellEnabledForTest(int32 Row) const
{
	return FleetRows.IsValidIndex(Row) && FleetRows[Row]->SellButton != nullptr && FleetRows[Row]->SellButton->GetIsEnabled();
}
