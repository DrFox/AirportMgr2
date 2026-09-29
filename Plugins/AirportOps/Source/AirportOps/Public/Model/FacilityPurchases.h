#pragma once

#include "CoreMinimal.h"
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
 * (written only through ApplyModulePurchase), UJobBoard owns Vehicles (through AddPurchasedVehicle and
 * RemoveVehicle), ULedger owns the money. Capacity is DERIVED on every ask - offers' VehicleSlots over
 * owned modules - never stored.
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

	/** The money. Null in a test that does not care: then everything is free, IBuildPurse's rule. */
	UPROPERTY() TObjectPtr<ULedger> Ledger = nullptr;

	/** Formats prices for the labels; null formats plain numbers. */
	UPROPERTY() TObjectPtr<UPricing> Pricing = nullptr;

	/** Dates the ledger entries; null dates them 0, ULedger::NowOrZero's rule. */
	UPROPERTY() TObjectPtr<USimClock> Clock = nullptr;

	/** Published to on success only. Owned by UOpsRuntime, like UAirlineRoster::Bus. */
	FOpsEventBus* Bus = nullptr;

	/**
	 * How many of Module the placed plot of Depot can hold - PlotYard's reservation ceiling. UNSET (a bare
	 * NewObject) answers 0, so no module can be bought: refusing is the honest default for a question
	 * this layer cannot answer.
	 */
	TFunction<int32(FEntityInstanceId Id, const FEntityInstance& Depot, EDepotModule Module)> ReservedSlotsOf;

	/**
	 * Append Module to the depot, rebuild its yard and checkpoint undo - URoadEditFacade::AddEntityModule
	 * in production. UNSET or false refuses NotAFacility, uncharged.
	 */
	TFunction<bool(FEntityInstanceId Id, EDepotModule Module)> ApplyModulePurchase;

	/** Vehicle bays Depot's modules grant. */
	int32 VehicleSlotsOf(const FEntityInstance& Depot) const;

	FFacilityQuote Quote(const URoadNetwork& Network, FEntityInstanceId Entity) const;
	FPurchaseResult BuyModule(const URoadNetwork& Network, FEntityInstanceId Entity, EDepotModule Module);
	FPurchaseResult BuyVehicle(const URoadNetwork& Network, FEntityInstanceId Entity, FName TypeCode);
	FPurchaseResult SellVehicle(int32 VehicleId);

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
