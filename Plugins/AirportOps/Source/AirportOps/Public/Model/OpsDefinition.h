#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadEntity.h"
#include "Model/VehicleCodes.h"
#include "Model/OpsDesignDefaults.h"
#include "OpsDefinition.generated.h"

/**
 * Base of every authored definition in AirportOps: scenarios, and in later milestones
 * vehicles, buildings, airlines, cargo classes, research nodes, contract templates.
 *
 * A PRIMARY data asset so the Asset Manager can enumerate them by type without loading
 * the world, which is how UOpsCatalog fills itself on a cold start. The primary asset
 * TYPE is the class name minus its prefix ("Scenario" for UScenario), so DefaultGame.ini
 * needs one PrimaryAssetTypesToScan entry per subclass. Only the subclasses that exist
 * are declared: spec §2.2 lists more, and they arrive with the milestone that reads them.
 */
UCLASS(Abstract, BlueprintType)
class AIRPORTOPS_API UOpsDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	virtual FPrimaryAssetId GetPrimaryAssetId() const override;
};

/**
 * What one KIND of fuel vehicle carries, how fast it pumps, and what it costs to own - keyed by
 * FVehicle::TypeCode.
 *
 * THE AUTHORED HALF OF A CATALOGUE ROW (#430): FServiceFleet::ResolveCatalogue joins it, at attach and after every load, to the chassis
 * Content builds for the same code, into the FServiceVehicleType everything else reads. A row whose code has no chassis
 * is dropped there, with a Warning - it used to become a zero-size vehicle that passed every fit gate.
 *
 * IN AirportOps, NOT ON FVehicle: Airside knows how a vehicle MOVES and must never learn what it
 * is FOR (UJobBoard's header). Spec 2026-09-28-fuel-litres section 1; prices from spec
 * 2026-09-29-facility-upgrades §2, where "a tanker later is a data row" (R4) is this struct.
 */
USTRUCT()
struct AIRPORTOPS_API FFuelVehicleSpec
{
	GENERATED_BODY()

	FFuelVehicleSpec() = default;
	FFuelVehicleSpec(double InCapacity, double InFlow) : CapacityLitres(InCapacity), FlowLitresPerMinute(InFlow) {}
	FFuelVehicleSpec(double InCapacity, double InFlow, double InPrice, double InUpkeep, FText InName)
		: CapacityLitres(InCapacity), FlowLitresPerMinute(InFlow), DisplayName(MoveTemp(InName)), Price(InPrice), UpkeepPerDay(InUpkeep) {}

	/** The vehicle's own tank - a load bigger than this takes more than one trip. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double CapacityLitres = 1000.0;

	/** How fast it pumps into an aircraft, litres per GAME minute. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double FlowLitresPerMinute = 75.0;

	/** What the depot card calls it - "Bowser". Empty falls back to the TypeCode, in FServiceFleet::NameOf alone. */
	UPROPERTY(EditAnywhere, Category = "Fleet") FText DisplayName;

	/** What buying one costs, charged to ELedgerCategory::Fleet. 0 is free, not "not for sale". */
	UPROPERTY(EditAnywhere, Category = "Fleet", meta = (ClampMin = "0.0")) double Price = 0.0;

	/** What owning one costs a day, whatever it does - part of the daily "Fleet upkeep" entry. */
	UPROPERTY(EditAnywhere, Category = "Fleet", meta = (ClampMin = "0.0")) double UpkeepPerDay = 0.0;

	/** The share of Price an idle one sells back for (R5). */
	UPROPERTY(EditAnywhere, Category = "Fleet", meta = (ClampMin = "0.0", ClampMax = "1.0")) double ResaleFraction = 0.5;

	/**
	 * What one is worth back: Price x ResaleFraction. ONE RULE for the two ways a vehicle leaves for
	 * money - UFacilityPurchases::RefundOf (a sale) and UJobBoard::SyncFleet (its depot removed) - so
	 * the card's "Sell" label and a bulldozer's credit cannot drift apart. Asked ONCE since #430, by
	 * FServiceFleet::ResolveCatalogue, into the row's ResaleValue that FServiceFleet::ResaleOf reads.
	 * ENFORCED BY: Check-Architecture rule 43 (fleet-one-door: .ResaleValue read in ServiceFleet.cpp alone)
	 */
	double ResaleValue() const { return Price * ResaleFraction; }
};

/**
 * One depot module the player can buy, and what it grants (spec 2026-09-29-facility-upgrades §2).
 *
 * ON UScenario, NOT ON UPlotModuleKit as the spec first said (§6 deviation 1): difficulty is the
 * scenario's numbers, a kit is a .uasset needing a headless edit per rebalance, and AirportOps'
 * Model/ may not include Airside's Content/. Rules code never names "shed": a module grants
 * VehicleSlots, and a second kind of bay later is a second row.
 */
USTRUCT()
struct AIRPORTOPS_API FModuleOffer
{
	GENERATED_BODY()

	FModuleOffer() = default;
	FModuleOffer(double InPrice, double InUpkeep, int32 InSlots, FText InName, FText InPlural)
		: DisplayName(MoveTemp(InName)), PluralName(MoveTemp(InPlural)), Price(InPrice), UpkeepPerDay(InUpkeep), VehicleSlots(InSlots) {}

	/** "Shed" - the buy button's word. */
	UPROPERTY(EditAnywhere, Category = "Facility") FText DisplayName;

	/** "Sheds" - the card's count line. Data, not an appended "s". */
	UPROPERTY(EditAnywhere, Category = "Facility") FText PluralName;

	/** Charged to ELedgerCategory::Placement - a module is building work. */
	UPROPERTY(EditAnywhere, Category = "Facility", meta = (ClampMin = "0.0")) double Price = 0.0;

	/** Per owned module per day, on every live depot - the daily "Facility upkeep" entry. */
	UPROPERTY(EditAnywhere, Category = "Facility", meta = (ClampMin = "0.0")) double UpkeepPerDay = 0.0;

	/** Vehicle bays each one grants (R2: a shed is one bay, any type). */
	UPROPERTY(EditAnywhere, Category = "Facility", meta = (ClampMin = "0")) int32 VehicleSlots = 0;
};

/**
 * What moves an airline's satisfaction, and how much it moves its offers. Spec 2026-09-29-ops-event-bus
 * §3. Into UAirlineRoster, applied at attach and after every load by UOpsRuntime::ApplyScenarioFigures (#449).
 *
 * FIRST GUESSES, UNJUDGED (2026-09-29): nobody has played with them yet. Tune once seen in play.
 */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FAirlineSatisfactionTuning
{
	GENERATED_BODY()

	/** Where every airline starts, and where the daily drift pulls it back to. 0..1. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0", ClampMax = "1.0")) double Start = 0.5;

	/** A flight airborne by its contract's deadline. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double OnTimeBonus = 0.03;

	/** A late flight costs this per ten game minutes late, up to LatePenaltyCap. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double LatePenaltyPerTenMinutes = 0.02;
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double LatePenaltyCap = 0.10;

	/** An offer the player could have taken and let lapse. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double IgnoredPenalty = 0.02;

	/** An offer that lapsed because no stand was free for it the whole time - the airport's fault, not the player's inattention. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double NeverAcceptablePenalty = 0.01;

	/**
	 * A turnaround that left short of fuel costs this times the fraction NOT delivered - part-fuelled in
	 * proportion, unfuelled in full (spec 2026-09-29-ops-batch3 §0). A fuelled one costs nothing and earns
	 * nothing here: the on-time bonus already rewards it. UNJUDGED (2026-09-29): a first guess, set three
	 * times the ignored-offer penalty because the aircraft was on the ground and the airport let it down.
	 */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double ShortfallPenalty = 0.06;

	/**
	 * Each flight the player's closure of the airport cancelled (spec 2026-09-29-ops-batch3 §0). ONLY a closure:
	 * a flight cancelled because the last runway went, or by the player despawning a stuck aeroplane, costs
	 * nothing - the user accepted the runway loophole. UNJUDGED (2026-09-29): a first guess, a little under the
	 * shortfall because the airline was told before it flew.
	 */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double ClosureCancelPenalty = 0.05;

	/** Each day, this fraction of the way back to Start. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0", ClampMax = "1.0")) double DailyDriftFraction = 0.2;

	/** An airline's demand is scaled by Lerp(Min, Max, satisfaction). A floor airline never below 1.0. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double MinRateMultiplier = 0.5;
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double MaxRateMultiplier = 1.5;
};

/** One rung of the fuel contract (spec 2026-10-02-progression-and-fuel-supply §7): litres delivered each game day at a
 *  price per litre. A tier needs the tanks to hold one day's delivery - see UFuelSupply::JudgeContract. */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FFuelContractTier
{
	GENERATED_BODY()

	FFuelContractTier() = default;
	FFuelContractTier(double InLitres, double InPrice) : LitresPerDay(InLitres), PricePerLitre(InPrice) {}

	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double LitresPerDay = 0.0;
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double PricePerLitre = 0.0;
};

/**
 * What fuel costs the airport and how it arrives (spec 2026-10-02 §7). FIRST GUESSES from the pacing model
 * (Tools/pacing_model.py, 2026-10-02): contract 0.9, spot 1.2 against a 2.0 sell price. Larger tiers will sit on the
 * cargo milestone track when milestones exist; until then every tier is open to anyone with the tanks for it.
 */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FFuelSupplyFigures
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double LitresPerTank = OpsDesignDefaults::LitresPerTank;
	/** A new game opens with the starter tank full, so the first flights can be fuelled before any order arrives. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double StartingStockLitres = OpsDesignDefaults::LitresPerTank;
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double SpotPricePerLitre = 1.2;
	/** GAME seconds from order to delivery. A timer until there is a landside road for a tanker to drive. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double SpotDelaySeconds = 7200.0;
	UPROPERTY(EditAnywhere, Category = "Fuel") TArray<FFuelContractTier> ContractTiers = {
		FFuelContractTier(5000.0, 0.9), FFuelContractTier(10000.0, 0.9),
		FFuelContractTier(20000.0, 0.9), FFuelContractTier(40000.0, 0.9) };
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1")) int32 ContractTermDays = 7;
	/** Cancelling owes this share of the days left at the tier's daily cost. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0", ClampMax = "1.0")) double CancelFraction = 0.5;
};

/** A new-game setup. Difficulty is these numbers and nothing else (spec §5.2). */
UCLASS(BlueprintType)
class AIRPORTOPS_API UScenario : public UOpsDefinition
{
	GENERATED_BODY()

public:
	/** The opening build plus about 50,000 (2026-10-02, was 500,000): the pacing model's opening - a
	 *  26 x 600 m grass runway, taxiway, two stands, the fuel depot with its bowser and shed - costs
	 *  ~207,000 at the x0.4 build prices, and the margin left over is what makes paving at ~2 real hours
	 *  a goal rather than a day-0 purchase. Spec 2026-10-02-progression-and-fuel-supply §9. */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0"))
	double StartingBalance = 260000.0;

	/**
	 * Real seconds the daylight hours (DawnHour..DuskHour) take at x1, and the night hours.
	 * Into USimClock, applied at attach and after every load by UOpsRuntime::ApplyScenarioFigures (#449).
	 *
	 * TWO FIGURES, NOT ONE (spec 2026-09-28): the night is a lull - the flying club is quiet
	 * and the scheduled carrier trickles - and a lull the player has to sit through at a busy
	 * morning's pace is dead time. 40 minutes of daylight to 8 of night is the first guess.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1.0"))
	double RealSecondsDaylight = 2400.0;
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1.0"))
	double RealSecondsNight = 480.0;

	/** Hours of day, 0-24, that bound daylight. Copied into USimClock with the lengths above. */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0", ClampMax = "24.0"))
	double DawnHour = 6.0;
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0", ClampMax = "24.0"))
	double DuskHour = 20.0;

	/**
	 * Hour of day a NEW GAME starts at, 0-24. Copied into USimClock by UOpsRuntime.
	 *
	 * 09:00 rather than midnight, because the first thing a new player sees should be the
	 * airfield in daylight with the day ahead of it. Starting at 00:00 opened on the sun
	 * parked at its dusk floor and nothing due for six game hours.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0", ClampMax = "24.0"))
	double StartHour = 9.0;

	/**
	 * Each fuel vehicle's tank and flow rate, by FVehicle::TypeCode. Joined - at attach and after every load, by
	 * UOpsRuntime::ApplyScenarioFigures (#449) - to the chassis
	 * Content builds for each code, into the job board's catalogue (FServiceFleet::ResolveCatalogue);
	 * a row whose code has no chassis is dropped there, with a Warning. KEYED BY AirsideVehicleCodes,
	 * never a literal: the code is the join, and it is typed once (Model/VehicleCodes.h). First guesses from the user (2026-09-28): the utility tow's 1,000 L trailer at
	 * 75 L/min, the bowser 10,000 L at 200 L/min; the articulated tanker (30,000 L, 500 L/min)
	 * joins with the depot fleet.
	 *
	 * REPLACED FuelDwellSeconds, a flat 40 movement-seconds for every aircraft whatever it held.
	 *
	 * PRICES (spec 2026-09-29-facility-upgrades §2): first guesses against a 500k opening balance and a ~15k full bowser load; unjudged in play.
	 * x0.4 ON 2026-10-02 with every other build price (spec 2026-10-02-progression-and-fuel-supply §9), upkeep with them.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario")
	TMap<FName, FFuelVehicleSpec> FuelVehicles = {
		{ FName(AirsideVehicleCodes::UtilityTow), FFuelVehicleSpec(1000.0, 75.0, 10000.0, 60.0, NSLOCTEXT("Scenario", "UtilityTow", "Utility tow")) },
		{ FName(AirsideVehicleCodes::Fuel), FFuelVehicleSpec(10000.0, 200.0, 36000.0, 200.0, NSLOCTEXT("Scenario", "Bowser", "Bowser")) } };

	/**
	 * The kinds of vehicle a STARTER depot begins with (spec 2026-09-28-service-vehicle-lifecycle §3.4): a depot placed
	 * with Trucks = N gets N of each, once. The player's depot has Trucks 0 and buys its fleet instead.
	 *
	 * AN EXPLICIT LIST (#430). It was "every distinct TypeCode in the stand-letter design-vehicle table", so changing
	 * which vehicle a stand LETTER was designed for silently changed which kinds a starter depot was seeded with - two
	 * questions answered by one table. The two codes here are what that table held on 2026-09-30 (A/B the tow, C-F the
	 * bowser), in its order. A code with no catalogue row is dropped where the catalogue is joined, with a Warning.
	 * ENFORCED BY: AirportOps.Fleet.CatalogueDropsARowWithNoChassis (the starter list is filtered to the catalogue)
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario")
	TArray<FName> StarterFleet = { FName(AirsideVehicleCodes::UtilityTow), FName(AirsideVehicleCodes::Fuel) };

	/** How fast a depot refills a returning vehicle, litres per GAME minute per pump module. */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1.0"))
	double DepotRefillLitresPerMinutePerPump = OpsDesignDefaults::RefillLitresPerMinutePerPump;

	/**
	 * The depot modules the player can buy, and what each grants. Into UFacilityPurchases, applied at attach
	 * and after every load by UOpsRuntime::ApplyScenarioFigures (#449). THE SHED ONLY this slice (spec 2026-09-29-facility-upgrades §1: pumps and tanks are out of
	 * scope) - THE SHED AND THE TANK (2026-10-02, spec 2026-10-02-progression-and-fuel-supply §7); pumps are still not for sale.
	 * A module with no row here is not for sale, and its buy is refused UnknownType.
	 * ENFORCED BY: AirportOps.Present.Facility.AttachCopiesTheOffers (the copy),
	 * AirportOps.Model.Facility.RefusalsChargeAndPublishNothing ("UnknownType module (no offer)")
	 * The shed x0.4 on 2026-10-02 (was 40,000 and 200/day), with every other build price.
	 */
	UPROPERTY(EditAnywhere, Category = "Facilities")
	TMap<EDepotModule, FModuleOffer> ModuleOffers = {
		{ EDepotModule::Shed, FModuleOffer(16000.0, 80.0, 1, NSLOCTEXT("Scenario", "Shed", "Shed"), NSLOCTEXT("Scenario", "Sheds", "Sheds")) },
		{ EDepotModule::Tank, FModuleOffer(20000.0, 100.0, 0, NSLOCTEXT("Scenario", "Tank", "Fuel tank"), NSLOCTEXT("Scenario", "Tanks", "Fuel tanks")) } };

	/** Fuel's price, delivery and storage figures (spec 2026-10-02 §7); consumed by UFuelSupply. */
	UPROPERTY(EditAnywhere, Category = "Fuel")
	FFuelSupplyFigures FuelSupply;

	/**
	 * How many offers the inbox holds before new ones are dropped (spec 2026-09-28 ruling 6).
	 * Into UOfferGenerator, applied at attach and after every load by UOpsRuntime::ApplyScenarioFigures (#449). The ATC tower may raise it later.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1"))
	int32 MaxPendingOffers = OpsDesignDefaults::MaxPendingOffers;

	/** What moves an airline's satisfaction - see FAirlineSatisfactionTuning. */
	UPROPERTY(EditAnywhere, Category = "Airlines")
	FAirlineSatisfactionTuning AirlineSatisfaction;
};
