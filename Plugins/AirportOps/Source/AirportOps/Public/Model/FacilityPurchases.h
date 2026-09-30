#pragma once

#include "CoreMinimal.h"
#include "Model/DepotCapability.h"
#include "Model/OpsDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadHandles.h"
#include "UObject/Object.h"
#include "FacilityPurchases.generated.h"

class FOpsEventBus;
class UJobBoard;
class ULedger;
class UPricing;
class URoadNetwork;
class USimClock;

/** Why a purchase or sale was refused (spec 2026-09-29-facility-upgrades §3). The UI's disabled reason. */
UENUM()
enum class EPurchaseRefusal : uint8
{
	None,
	/** Not a live facility: a stand, a removed depot, an unknown vehicle, or no world hook. */
	NotAFacility,
	CannotAfford,
	/** R9: no free reserved (ghost) slot for the module. */
	NoSlotReserved,
	/** R2: every bay has a vehicle. */
	NoFreeBay,
	/** No offer for it: a vehicle row absent from the scenario, or a module with no FModuleOffer. */
	UnknownType,
	/** R5: not idle at home with an empty queue. */
	VehicleBusy
};

/** One module offer as the card shows it. Label is "Buy Shed ¤40,000". */
struct FModuleOfferQuote
{
	EDepotModule Module = EDepotModule::Shed;
	FText Name;
	FText PluralName;
	double Price = 0.0;
	double UpkeepPerDay = 0.0;
	int32 Owned = 0;
	int32 Reserved = 0;
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	FText Label;
};

/** One vehicle offer. Label is "Bowser ¤90,000 · 10,000 L". */
struct FVehicleOfferQuote
{
	FName TypeCode;
	FText Name;
	double Price = 0.0;
	double UpkeepPerDay = 0.0;
	double CapacityLitres = 0.0;
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	FText Label;
};

/** One vehicle at the depot, and whether it can be sold. SellLabel is "Sell ¤45,000". */
struct FFleetRowQuote
{
	int32 VehicleId = 0;
	FName TypeCode;
	FString Line;
	double Refund = 0.0;
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	FText SellLabel;
};

/**
 * Everything the depot card shows, and nothing it computes (spec §3: the UI renders ONLY the quote).
 * A PLAIN STRUCT, like FBuildQuote: never saved, never reflected, rebuilt on every ask.
 */
struct FFacilityQuote
{
	/** None for a live facility; NotAFacility otherwise, with every array empty. */
	EPurchaseRefusal Refusal = EPurchaseRefusal::NotAFacility;
	int32 Bays = 0;
	int32 Vehicles = 0;
	TArray<FModuleOfferQuote> Modules;
	TArray<FVehicleOfferQuote> VehicleOffers;
	TArray<FFleetRowQuote> Fleet;

	bool IsFacility() const { return Refusal == EPurchaseRefusal::None; }
};

/** What a command did. Amount is what was charged (buy) or credited (sell); 0 when refused. */
struct FPurchaseResult
{
	EPurchaseRefusal Refusal = EPurchaseRefusal::None;
	double Amount = 0.0;
	/** The bought vehicle's id; 0 otherwise. */
	int32 VehicleId = 0;

	bool Succeeded() const { return Refusal == EPurchaseRefusal::None; }
};

/** A day's facility upkeep, two figures so the ledger shows them as two lines (spec §3). */
struct FFacilityUpkeep
{
	double Modules = 0.0;
	double Fleet = 0.0;
};

/**
 * Buying a building's modules and vehicles, and selling its vehicles - one purchase service for every
 * building with modules and a fleet (spec 2026-09-29-facility-upgrades, R1/R7). Today: fuel depots.
 *
 * PATTERN: a Command service over three owners, none of which it duplicates. The network owns Modules
 * (written only through ApplyModulePurchase), UJobBoard owns Vehicles (through FServiceFleet, the fleet's membership
 * door: its Add and Withdraw take the charge, the credit, the FleetChanged event and the re-open of refused jobs, so
 * a purchase, a starter fleet's seeding and a removed depot's withdrawal owe the same things - #443), ULedger owns the
 * money. Capacity is DERIVED on every ask - offers' VehicleSlots over owned modules - never stored.
 *
 * QUOTE AND COMMAND SHARE THEIR JUDGEMENT: each command re-runs the private Judge* its quote row was
 * built from, so a button can only be lit for a command that will succeed.
 * ENFORCED BY: AirportOps.Model.Facility.QuoteEqualsCommandForEveryOffer
 *
 * WORLD-FREE (Model/): the reserved-slot ceiling (a plot solve in Airside Build/) and the module write
 * (a facade edit with a rebuild and an undo checkpoint) come in as TFunction hooks set by UOpsRuntime at
 * attach - UJobBoard::DesignVehicleOf's pattern. The network is passed PER CALL, not held: the actor's
 * network object is replaced by a clear, a load and every undo (§6 deviation 3).
 */
UCLASS()
class AIRPORTOPS_API UFacilityPurchases : public UObject
{
	GENERATED_BODY()

public:
	/** Copied from UScenario::ModuleOffers at attach. A module with no row is not for sale. */
	UPROPERTY() TMap<EDepotModule, FModuleOffer> ModuleOffers;

	/** Set by UOpsRuntime's constructor, like UAgentRescue's boards. Null refuses everything NotAFacility. */
	UPROPERTY() TObjectPtr<UJobBoard> JobBoard = nullptr;

	/** The money. Null in a test that does not care: then everything is free, IBuildPurse's rule. THE JUDGEMENT reads
	 *  it (CanPay) and a MODULE purchase posts to it; a vehicle's charge and credit are posted by the fleet's door, from
	 *  UJobBoard::Ledger - which UOpsRuntime wires to this same ledger. */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;

	/** Formats prices for the labels; null formats plain numbers. */
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/** Dates the ledger entries; null dates them 0, ULedger::NowOrZero's rule. */
	UPROPERTY() TObjectPtr<USimClock> Clock = nullptr;

	/** Published to on success only: FFacilityUpgradedEvent. Owned by UOpsRuntime, like UAirlineRoster::Bus. A vehicle's
	 *  FleetChanged is the fleet door's (UJobBoard::Bus). */
	FOpsEventBus* Bus = nullptr;

	/**
	 * How many of Module the placed plot of Depot can hold - PlotYard's reservation ceiling. UNSET (a bare
	 * NewObject) answers 0, so no module can be bought: refusing is the honest default for a question
	 * this layer cannot answer. THE SAME CEILING seats what the depot already owns (VehicleSlotsOf reads FDepotCapability
	 * through it), so a purchase, a quote and the bays the depot grants are one plot's answer (#443). UOpsRuntime sets the
	 * board's own ModuleCeilingOf to this same function.
	 */
	FModuleCeilingFn ReservedSlotsOf;

	/**
	 * Append Module to the depot, rebuild its yard and checkpoint undo - URoadEditFacade::AddEntityModule
	 * in production. UNSET or false refuses NotAFacility, uncharged - and UNSET is quoted NotAFacility too
	 * (JudgeModule), so a card never lights a Buy the command can only refuse. FALSE cannot be quoted: the
	 * facade refuses during an open drag, a state no quote sees (a click between drags in practice).
	 */
	TFunction<bool(FEntityInstanceId Id, EDepotModule Module)> ApplyModulePurchase;

	/**
	 * Remove up to Count of Module from the depot, rebuild its yard, push no undo step and clear the history -
	 * URoadEditFacade::RemoveUnseatedModules in production; returns how many went. UNSET removes nothing, so
	 * RemoveUnseated refunds nothing: a refund with no removal would be free money. ApplyModulePurchase's pattern.
	 */
	TFunction<int32(FEntityInstanceId Id, EDepotModule Module, int32 Count)> ApplyModuleRemoval;

	/**
	 * Vehicle bays Depot's SEATED modules grant (#443, ruled 2026-09-30): the modules the plot could not seat grant
	 * nothing, so the card never quotes a bay the player cannot see. With no ceiling hook (a world-free shop) the owned
	 * list stands - see FDepotCapability::Of.
	 * ENFORCED BY: AirportOps.Model.Facility.UnseatedModulesGrantNeitherBaysNorPumps
	 */
	int32 VehicleSlotsOf(FEntityInstanceId Id, const FEntityInstance& Depot) const;

	FFacilityQuote Quote(const URoadNetwork& Network, FEntityInstanceId Entity) const;
	FPurchaseResult BuyModule(const URoadNetwork& Network, FEntityInstanceId Entity, EDepotModule Module);
	FPurchaseResult BuyVehicle(const URoadNetwork& Network, FEntityInstanceId Entity, FName TypeCode);
	FPurchaseResult SellVehicle(int32 VehicleId);

	/**
	 * One day's upkeep: every owned module with an offer on every live facility, and every vehicle on the
	 * board at its row's UpkeepPerDay (R6). Two figures, posted as two described lines by UOpsRuntime.
	 * ENFORCED BY: AirportOps.Model.Facility.UpkeepSumsModulesAndFleet
	 */
	FFacilityUpkeep DailyUpkeep(const URoadNetwork& Network) const;

	/**
	 * THE REPAIR (#266, owner 2026-09-30, option b: "there must never be unplaced modules"): every live depot that owns
	 * more of a kind than its plot SEATS has the excess removed (ApplyModuleRemoval) and refunded here, one ELedgerCategory::
	 * Refund line per depot and kind, with a Warning log line naming the depot and the modules and an FModulesRefundedEvent
	 * for the toast. Returns how many modules went. owned == placed afterwards, so upkeep (DailyUpkeep, over the owned list)
	 * never charges for a module that is not standing.
	 *
	 * WHY HERE AND NOT IN AIRSIDE: the removal and the refund are one act - removing without paying back takes the player's
	 * money, paying back without removing is money for nothing - and only this service holds both the module prices and a
	 * door to the network. Airside answers the question (the seat, through ReservedSlotsOf - its one plot solve) and makes
	 * the write (the facade door); this decides and pays. THE MONEY DOOR FOR A REMOVED MODULE, as BuyModule is for a
	 * bought one: the removal it pays for has no other production caller (Check-Architecture rule 4 row 'module removal').
	 *
	 * THE REFUND IS THE OFFER'S Price, what the shop charges for one: a module did not stop being worth that because the
	 * plot shrank. A kind the shop does not sell (no FModuleOffer - the tank and pump this slice) was never priced alone -
	 * it came in the plot's kit - so it is removed with nothing to post and the log says so.
	 *
	 * RUN BY UOpsRuntime's "ModuleRepair" bus pass: on attach and after a load (MarkAllDirty, the catch-up), and on every
	 * FNetworkChangedEvent - the moments a plot's seat can change. Plotless depots and an unset ceiling hook are left alone
	 * (FDepotCapability::Of: no solve, nothing to be smaller than).
	 * ENFORCED BY: AirportOps.Model.Facility.RepairRemovesAndRefundsTheExcess, AirportOps.Present.Facility.RepairRemovesAndRefundsUnseated
	 */
	int32 RemoveUnseated(const URoadNetwork& Network);

	/** "No space", "Can't afford" - the disabled button's reason. The wording is the contract. */
	static FText RefusalText(EPurchaseRefusal Why);

private:
	/** Entity if it is a live facility on Network, else null. */
	static const FEntityInstance* FacilityAt(const URoadNetwork& Network, FEntityInstanceId Entity);

	int32 ReservedOf(FEntityInstanceId Entity, const FEntityInstance& Facility, EDepotModule Module) const;
	static int32 OwnedOf(const FEntityInstance& Facility, EDepotModule Module);

	EPurchaseRefusal JudgeModule(const FEntityInstance* Facility, EDepotModule Module, int32 Reserved) const;
	EPurchaseRefusal JudgeVehicle(const FEntityInstance* Facility, FEntityInstanceId Entity, FName TypeCode) const;
	EPurchaseRefusal JudgeSale(int32 VehicleId) const;

	bool CanPay(double Price) const;
	double NowOrZero() const;
	FText Money(double Amount) const;
	FText VehicleName(FName TypeCode) const;
	double RefundOf(FName TypeCode) const;
	void LogRefused(int32 Depot, const FString& What, EPurchaseRefusal Why) const;
};
