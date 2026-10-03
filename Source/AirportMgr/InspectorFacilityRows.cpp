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
#include "UI/UiButton.h"
#include "UIStyle.h"

namespace
{
	/** THE ONE TABLE of the BuildActions rows the purchase rows run, in EAction order: OwnsAction and Build's search by id both
	 *  read it. Prefixed against the unity build. */
	const TCHAR* const InspectorFacilityActionIds[] = {
		TEXT("selection.buy_module"), TEXT("selection.buy_vehicle"), TEXT("selection.sell_vehicle"),
		TEXT("selection.fuel_spot"), TEXT("selection.fuel_contract_up"), TEXT("selection.fuel_contract_cancel") };
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
	// THE MODULE ROWS' LIST, empty: Show builds one row per module on offer into it (RebuildModuleRows).
	Panel(ModuleList, UVerticalBox::StaticClass(), TEXT("ModuleList"));
	Panel(FuelRow, UHorizontalBox::StaticClass(), TEXT("FuelRow"));
	RowText(FuelText, TEXT("FuelText"), FuelRow);
	Panel(FuelButtonsRow, UHorizontalBox::StaticClass(), TEXT("FuelButtonsRow"));
	// RowText's rule: built only into a row that exists, never an orphan - and only for an action the registry has.
	auto RowButton = [&](TObjectPtr<UUiButton>& Field, const TCHAR* Name, EAction Action)
	{
		UHorizontalBox* Box = Cast<UHorizontalBox>(FuelButtonsRow);
		if (Field != nullptr || Box == nullptr || !Actions.IsValidIndex(IndexOf(Action))) { return; }
		Field = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), Name);
		Field->SetLabel(Actions[IndexOf(Action)].Label);
		Field->Build(InStyle, EUiButtonKind::Secondary);
		Box->AddChildToHorizontalBox(Field)->SetPadding(FMargin(0.0f, 0.0f, 6.0f, 0.0f));
	};
	RowButton(FuelSpotButton, TEXT("FuelSpotButton"), EAction::FuelSpot);
	RowButton(FuelContractButton, TEXT("FuelContractButton"), EAction::FuelContractUp);
	RowButton(FuelCancelButton, TEXT("FuelCancelButton"), EAction::FuelContractCancel);
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
			UE_LOG(LogInspector, Warning, TEXT("A depot-card row (selection.buy_module/buy_vehicle/sell_vehicle/fuel_*) is missing from BuildActions(): the depot card cannot buy, sell or order"));
			break;
		}
	}

	if (FuelSpotButton != nullptr) { FuelSpotButton->OnClicked.AddDynamic(this, &UInspectorFacilityRows::HandleFuelSpot); }
	if (FuelContractButton != nullptr) { FuelContractButton->OnClicked.AddDynamic(this, &UInspectorFacilityRows::HandleFuelContract); }
	if (FuelCancelButton != nullptr) { FuelCancelButton->OnClicked.AddDynamic(this, &UInspectorFacilityRows::HandleFuelCancel); }
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

void UInspectorFacilityRows::Show(const FFacilityQuote& Quote, const FDepotFuelView& Fuel)
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
	// A BUTTON'S CAPTION, ENABLED STATE AND TOOLTIP, each set only when it moved - SetLabel and SetToolTipText rebuild on every call.
	auto Paint = [](UUiButton* Button, const FText& Caption, bool bCan, const FText& Tip)
	{
		if (Button == nullptr) { return; }
		const UTextBlock* Current = Button->GetLabel();
		if (Current == nullptr || !Current->GetText().EqualTo(Caption)) { Button->SetLabel(Caption); }
		if (!Button->GetToolTipText().EqualTo(Tip)) { Button->SetToolTipText(Tip); }
		Button->SetState(bCan, false);
	};
	// THE WIDGET'S OWN ROOT: self-hit-test-invisible, as a UserWidget defaults to, so its gaps let a click through to the window;
	// the buttons inside are Visible and take theirs.
	Visible(this, bCard, ESlateVisibility::SelfHitTestInvisible);

	// ONE ROW PER MODULE, IN KIND ORDER (Modules is TMap order, which is not stable - a shed that swapped places with the tank between
	// two quotes would rebuild the rows and move the button under the cursor). Rebuilt only when the kinds on offer change.
	TArray<const FModuleOfferQuote*> Modules;
	if (bCard)
	{
		for (const FModuleOfferQuote& Module : Quote.Modules) { Modules.Add(&Module); }
		Modules.Sort([](const FModuleOfferQuote& A, const FModuleOfferQuote& B) { return A.Module < B.Module; });
	}
	FString ModuleKey;
	for (const FModuleOfferQuote* Module : Modules) { ModuleKey += FString::Printf(TEXT("%d,"), static_cast<int32>(Module->Module)); }
	if (ModuleKey != LastModuleKey)
	{
		LastModuleKey = ModuleKey;
		RebuildModuleRows(Modules);
	}
	ShowRow(ModuleList, Modules.Num() > 0);
	for (int32 Index = 0; Index < ModuleRows.Num() && Index < Modules.Num(); ++Index)
	{
		const FModuleOfferQuote& Module = *Modules[Index];
		UInspectorModuleRow* Row = ModuleRows[Index];
		SetIfChanged(Row->Line, FString::Printf(TEXT("%s %d / %d space"), *Module.PluralName.ToString(), Module.Owned, Module.Reserved));
		// A REFUSED BUY SAYS WHY on its own caption - a greyed button with no reason teaches nothing.
		const bool bCan = Module.Refusal == EPurchaseRefusal::None;
		const FText Caption = bCan ? Module.Label
			: FText::Format(NSLOCTEXT("AirportMgr", "InspectorRefusedCaption", "{0} - {1}"), Module.Label, UFacilityPurchases::RefusalText(Module.Refusal));
		Paint(Row->BuyButton, Caption, bCan, Module.Label);
	}

	// THE FUEL ROW: the line, and the three buttons from the same view - Sign OR Cancel, by whether a contract runs (FDepotFuelView).
	const bool bFuel = bCard && Fuel.bShown;
	ShowRow(FuelRow, bFuel);
	ShowRow(FuelButtonsRow, bFuel);
	if (bFuel)
	{
		SetIfChanged(FuelText, Fuel.Line);
		Paint(FuelSpotButton, Fuel.SpotCaption, Fuel.Spot == EFuelOrderRefusal::None, Fuel.SpotTip);
		Paint(FuelContractButton, Fuel.SignCaption, Fuel.Sign == EFuelOrderRefusal::None, Fuel.SignTip);
		Paint(FuelCancelButton, Fuel.CancelCaption, Fuel.Cancel == EFuelOrderRefusal::None, Fuel.CancelTip);
		ShowRow(FuelContractButton, Fuel.bOfferSign);
		ShowRow(FuelCancelButton, !Fuel.bOfferSign);
	}

	ShowRow(VehiclesRow, bCard);
	ShowRow(FleetList, bCard && Quote.Fleet.Num() > 0);
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

void UInspectorFacilityRows::RebuildModuleRows(const TArray<const FModuleOfferQuote*>& Modules)
{
	// THE FLEET ROWS' SHAPE: a row object per line, holding what its click needs (its module), built into the list.
	if (ModuleList != nullptr) { ModuleList->ClearChildren(); }
	ModuleRows.Reset();
	if (ModuleList == nullptr || Style == nullptr) { return; }
	for (const FModuleOfferQuote* Module : Modules)
	{
		UInspectorModuleRow* Row = NewObject<UInspectorModuleRow>(this);
		Row->Module = Module->Module;
		Row->Owner = this;
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Row->Line = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		Style->ApplyText(*Row->Line, EUITextRole::Body, Style->InkMuted);
		Box->AddChildToHorizontalBox(Row->Line)->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
		Row->BuyButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass());
		Row->BuyButton->SetLabel(Module->Label);
		Row->BuyButton->Build(*Style, EUiButtonKind::Secondary);
		Row->BuyButton->OnClicked.AddDynamic(Row, &UInspectorModuleRow::HandleBuy);
		Box->AddChildToHorizontalBox(Row->BuyButton);
		ModuleList->AddChild(Box);
		ModuleRows.Add(Row);
	}
}

void UInspectorModuleRow::HandleBuy()
{
	if (UInspectorFacilityRows* Rows = Owner.Get())
	{
		Rows->OnModuleBuy(*this);
	}
}

void UInspectorFacilityRows::OnModuleBuy(const UInspectorModuleRow& Row)
{
	if (!RunActionSource)
	{
		UE_LOG(LogInspector, Warning, TEXT("Buy %s click ignored: nothing to run it through"), *UEnum::GetValueAsString(Row.Module));
		return;
	}
	// THE ROW'S MODULE IS THE ARGUMENT of the run (DepotModuleCode, the one spelling the verb matches) - the sell row's shape.
	RunActionSource(IndexOf(EAction::BuyModule), FBuildActionArg::OfCode(DepotModuleCode(Row.Module)));
}

void UInspectorFacilityRows::RunPlain(EAction Action)
{
	if (!RunActionSource)
	{
		UE_LOG(LogInspector, Warning, TEXT("Fuel click (action %d) ignored: nothing to run it through"), static_cast<int32>(Action));
		return;
	}
	RunActionSource(IndexOf(Action), FBuildActionArg());
}

void UInspectorFacilityRows::HandleFuelSpot() { RunPlain(EAction::FuelSpot); }
void UInspectorFacilityRows::HandleFuelContract() { RunPlain(EAction::FuelContractUp); }
void UInspectorFacilityRows::HandleFuelCancel() { RunPlain(EAction::FuelContractCancel); }

void UInspectorFleetRow::HandleSell()
{
	if (UInspectorFacilityRows* Rows = Owner.Get())
	{
		Rows->OnFleetSell(*this);
	}
}

void UInspectorFacilityRows::OnFleetSell(UInspectorFleetRow& Row)
{
	if (!Row.bArmed)
	{
		// ONE ARMED ROW: arming a second row disarms the first, so "click again" is never ambiguous. THE ROW IS WHERE IT IS ARMED:
		// the controller used to hold the id as well (#448), and a second caller of the sell row ran with it.
		for (UInspectorFleetRow* Other : FleetRows) { if (Other != nullptr) { Other->bArmed = false; } }
		Row.bArmed = true;
		return;
	}
	Row.bArmed = false;
	if (!RunActionSource)
	{
		UE_LOG(LogInspector, Warning, TEXT("Sell click on vehicle %d ignored: nothing to run it through"), Row.VehicleId);
		return;
	}
	// THE SECOND CLICK SELLS THIS ROW'S VEHICLE: the id is the argument of the run, not state left for the verb to find.
	RunActionSource(IndexOf(EAction::SellVehicle), FBuildActionArg::OfId(Row.VehicleId));
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
	if (!RunActionSource || !ShownVehicleCodes.IsValidIndex(Index))
	{
		UE_LOG(LogInspector, Warning, TEXT("Buy vehicle line %d ignored: nothing to run it through or no such line"), Index);
		return;
	}
	// THE LINE'S KIND IS THE ARGUMENT of the run - the controller used to be told which kind was chosen one line before the row ran.
	RunActionSource(IndexOf(EAction::BuyVehicle), FBuildActionArg::OfCode(ShownVehicleCodes[Index]));
}

void UInspectorFacilityRows::DisarmSale()
{
	// THE ROWS' OWN CAPTION STATE, which is all there is to disarm (#448). It was BOTH HALVES - the row's bArmed and the controller's
	// armed id - until the id travelled with the run instead; a panel with no rows has nothing armed to forget.
	for (UInspectorFleetRow* Row : FleetRows) { if (Row != nullptr) { Row->bArmed = false; } }
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

void UInspectorFacilityRows::ClickBuyModuleForTest(int32 Row)
{
	if (ModuleRows.IsValidIndex(Row) && ModuleRows[Row]->BuyButton != nullptr) { ModuleRows[Row]->BuyButton->OnClicked.Broadcast(); }
}

UUiButton* UInspectorFacilityRows::FuelButtonForTest(EAction Action) const
{
	switch (Action)
	{
	case EAction::FuelSpot:           return FuelSpotButton;
	case EAction::FuelContractUp:     return FuelContractButton;
	case EAction::FuelContractCancel: return FuelCancelButton;
	default:                          return nullptr;
	}
}

void UInspectorFacilityRows::ClickFuelForTest(EAction Action)
{
	if (UUiButton* Button = FuelButtonForTest(Action)) { Button->OnClicked.Broadcast(); }
}

bool UInspectorFacilityRows::IsFuelRowShownForTest() const
{
	return FuelRow != nullptr && FuelRow->GetVisibility() == ESlateVisibility::Visible;
}

FString UInspectorFacilityRows::FuelTextForTest() const { return FuelText != nullptr ? FuelText->GetText().ToString() : FString(); }

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
FString UInspectorFacilityRows::ModuleTextForTest(int32 Row) const
{
	return ModuleRows.IsValidIndex(Row) && ModuleRows[Row]->Line != nullptr ? ModuleRows[Row]->Line->GetText().ToString() : FString();
}
FString UInspectorFacilityRows::VehiclesTextForTest() const { return VehiclesText != nullptr ? VehiclesText->GetText().ToString() : FString(); }
bool UInspectorFacilityRows::IsBuyModuleEnabledForTest(int32 Row) const
{
	return ModuleRows.IsValidIndex(Row) && ModuleRows[Row]->BuyButton != nullptr && ModuleRows[Row]->BuyButton->GetIsEnabled();
}
FString UInspectorFacilityRows::BuyModuleCaptionForTest(int32 Row) const
{
	const UTextBlock* Caption = ModuleRows.IsValidIndex(Row) && ModuleRows[Row]->BuyButton != nullptr ? ModuleRows[Row]->BuyButton->GetLabel() : nullptr;
	return Caption != nullptr ? Caption->GetText().ToString() : FString();
}
bool UInspectorFacilityRows::IsSellEnabledForTest(int32 Row) const
{
	return FleetRows.IsValidIndex(Row) && FleetRows[Row]->SellButton != nullptr && FleetRows[Row]->SellButton->GetIsEnabled();
}
