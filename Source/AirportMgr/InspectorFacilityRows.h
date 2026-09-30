#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Model/FacilityPurchases.h"
#include "UI/UiMenuButton.h"
#include "InspectorFacilityRows.generated.h"

class ARoadBuildController;
class UInspectorFacilityRows;
class UPanelWidget;
class UTextBlock;
class UUiButton;
class UUIStyle;

/**
 * One fleet row's Sell click - UBuildBarEntry's reason: a dynamic delegate binds only to a UFUNCTION on a
 * UObject. The first click ARMS (the caption asks again), the second sells: selling cannot be undone
 * (memory: destructive gestures need a deliberate mode).
 */
UCLASS()
class AIRPORTMGR_API UInspectorFleetRow : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 VehicleId = 0;
	UPROPERTY() TObjectPtr<UTextBlock> Line;
	UPROPERTY() TObjectPtr<UUiButton> SellButton;
	UPROPERTY() TWeakObjectPtr<UInspectorFacilityRows> Owner;
	bool bArmed = false;

	UFUNCTION() void HandleSell();
};

/**
 * THE DEPOT CARD'S PURCHASE ROWS (facility-upgrades spec §4; a sub-widget of their own since issue #441, when they left
 * UInspectorWidget): the sheds line and its Buy button, the vehicles line and its buy menu, and one Sell row per vehicle in
 * the fleet - all filled from the depot card's one quote (FInspectorCardView::Quote).
 *
 * It RENDERS THE QUOTE AND NOTHING ELSE - the spec's rule that the card cannot disagree with the rules. It reports intents
 * through the two callbacks its host sets (a controller to act through, a way to run a BuildActions row) and holds no rule of
 * its own; the inspector's card selection, the Unstick popup and the aircraft verbs are none of its business.
 *
 * The C++ base builds the rows asset-free; a Blueprint subclass restyles through the BindWidgetOptional names. A Blueprint of
 * the inspector that bound these names on UInspectorWidget itself would have lost them in the move: the inspector binds one
 * UInspectorFacilityRows slot (FacilityRows) instead.
 */
UCLASS()
class AIRPORTMGR_API UInspectorFacilityRows : public UUserWidget
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ShedsRow;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ShedsText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiButton> BuyModuleButton;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> VehiclesRow;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> VehiclesText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UUiMenuButton> BuyVehicleMenu;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> FleetList;

	/**
	 * The three BuildActions rows these rows run. ONE TABLE (the ids, in this order, in InspectorFacilityRows.cpp) feeds both
	 * OwnsAction and Build's search by id, so the widget's skip and the rows' find cannot name different rows.
	 */
	enum class EAction : uint8 { BuyModule, BuyVehicle, SellVehicle, Count };

	/**
	 * Whether Id is one of those three rows. The inspector's own positional scan of the Selection section skips exactly these
	 * (counted, they would shift the Depart/Follow pair).
	 * ENFORCED BY: AirportMgr.Inspector.FacilityRowsAreWired (names the three ids, refuses Depart/Follow and two other Selection
	 * rows, finds each action's index by id in BuildActions(), and checks Depart's caption did not shift)
	 */
	static bool OwnsAction(FName Id);

	/** Builds the rows the asset did not give, finds the three action rows by id and binds the clicks. Once. */
	void Build(const UUIStyle& Style);
	bool IsBuilt() const { return Style != nullptr; }

	/** The controller the clicks act through - set by the host, weak-captured there: a lambda held by a child widget that
	 *  captured a raw pointer to its owner is the shape that dangles the first time either is rebuilt. */
	TFunction<ARoadBuildController*()> ControllerSource;
	/** Runs BuildActions() row ActionIndex the way the host runs its own verbs (UInspectorWidget::RunAction: its log lines and all). */
	TFunction<void(int32 ActionIndex)> RunActionSource;

	/**
	 * Render the purchase rows from Quote and NOTHING ELSE - the spec's rule that the card cannot disagree
	 * with the rules. A quote that is no facility collapses them. Public so a headless test (no runtime,
	 * no controller) can drive it; the inspector calls it every tick with the shown card's quote - the quote itself is
	 * asked once per card key, not once per tick (FDepotCard).
	 * ENFORCED BY: AirportMgr.Inspector.FacilityCardRendersTheQuote
	 */
	void Show(const FFacilityQuote& Quote);

	/** A new card: close the buy menu, and force the next depot's fleet rows to rebuild - which disarms any sale armed on the old
	 *  card (RebuildFleetRows). */
	void OnNewCard();

	/** Unarm every fleet row and the controller's armed sale - a new card, a deselect or a rebuild. */
	void DisarmSale();

	/** A fleet row's click - see UInspectorFleetRow. */
	void OnFleetSell(UInspectorFleetRow& Row);

	UFUNCTION() void HandleBuyModule();
	UFUNCTION() void HandleBuyVehicleChosen(int32 Index);

	bool AreFacilityRowsShownForTest() const;
	FString ShedsTextForTest() const;
	FString VehiclesTextForTest() const;
	bool IsBuyModuleEnabledForTest() const;
	FString BuyModuleCaptionForTest() const;
	TArray<FUiMenuItem> BuyVehicleItemsForTest() const { return BuyVehicleItems(); }
	int32 FleetRowCountForTest() const { return FleetRows.Num(); }
	bool IsSellEnabledForTest(int32 Row) const;
	/** The card's clicks, raised through each widget's OWN delegate so an unbound button fails the test. */
	void ClickSellForTest(int32 Row);
	FString SellCaptionForTest(int32 Row) const;
	void ClickBuyModuleForTest();
	void ChooseBuyVehicleForTest(int32 Line);
	/** Where Build found Action in BuildActions() - INDEX_NONE when the registry has no row by that id. */
	int32 ActionIndexForTest(EAction Action) const { return ActionIndex[static_cast<int32>(Action)]; }

private:
	UPROPERTY() TObjectPtr<const UUIStyle> Style;

	/** INDICES INTO BuildActions(), never ids to look up - the inspector's RunwayActionIndex reason - found BY ID in Build, by EAction. */
	int32 ActionIndex[static_cast<int32>(EAction::Count)] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
	int32 IndexOf(EAction Action) const { return ActionIndex[static_cast<int32>(Action)]; }

	/** The quote the rows were last drawn from - the buy menu's lines are asked of it as it opens. */
	FFacilityQuote LastQuote;
	/** The fleet's vehicle ids last drawn; the rows are rebuilt only when this changes. */
	FString LastFleetKey;
	/** The type codes the open buy menu lists, by line - HandleBuyVehicleChosen reads the line's code. */
	mutable TArray<FName> ShownVehicleCodes;
	UPROPERTY() TArray<TObjectPtr<UInspectorFleetRow>> FleetRows;

	TArray<FUiMenuItem> BuyVehicleItems() const;
	void RebuildFleetRows();
	ARoadBuildController* Controller() const { return ControllerSource ? ControllerSource() : nullptr; }
};
