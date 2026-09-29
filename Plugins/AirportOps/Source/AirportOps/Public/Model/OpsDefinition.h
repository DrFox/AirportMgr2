#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
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
 * What one KIND of fuel vehicle carries and how fast it pumps - keyed by FVehicle::TypeCode.
 *
 * IN AirportOps, NOT ON FVehicle: Airside knows how a vehicle MOVES and must never learn what it
 * is FOR (UJobBoard's header). Spec 2026-09-28-fuel-litres section 1.
 */
USTRUCT()
struct AIRPORTOPS_API FFuelVehicleSpec
{
	GENERATED_BODY()

	FFuelVehicleSpec() = default;
	FFuelVehicleSpec(double InCapacity, double InFlow) : CapacityLitres(InCapacity), FlowLitresPerMinute(InFlow) {}

	/** The vehicle's own tank - a load bigger than this takes more than one trip. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double CapacityLitres = 1000.0;

	/** How fast it pumps into an aircraft, litres per GAME minute. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double FlowLitresPerMinute = 75.0;
};

/**
 * What moves an airline's satisfaction, and how much it moves its offers. Spec 2026-09-29-ops-event-bus
 * §3. Copied into UAirlineRoster at attach.
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

	/** Each day, this fraction of the way back to Start. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0", ClampMax = "1.0")) double DailyDriftFraction = 0.2;

	/** An airline's demand is scaled by Lerp(Min, Max, satisfaction). A floor airline never below 1.0. */
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double MinRateMultiplier = 0.5;
	UPROPERTY(EditAnywhere, Category = "Airlines", meta = (ClampMin = "0.0")) double MaxRateMultiplier = 1.5;
};

/** A new-game setup. Difficulty is these numbers and nothing else (spec §5.2). */
UCLASS(BlueprintType)
class AIRPORTOPS_API UScenario : public UOpsDefinition
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0"))
	double StartingBalance = 500000.0;

	/**
	 * Real seconds the daylight hours (DawnHour..DuskHour) take at x1, and the night hours.
	 * Copied into USimClock by UOpsRuntime at attach.
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
	 * Each fuel vehicle's tank and flow rate, by FVehicle::TypeCode. Copied into UJobBoard at
	 * attach. First guesses from the user (2026-09-28): the utility tow's 1,000 L trailer at
	 * 75 L/min, the bowser 10,000 L at 200 L/min; the articulated tanker (30,000 L, 500 L/min)
	 * joins with the depot fleet.
	 *
	 * REPLACED FuelDwellSeconds, a flat 40 movement-seconds for every aircraft whatever it held.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario")
	TMap<FName, FFuelVehicleSpec> FuelVehicles = {
		{ FName(TEXT("UTILITY")), FFuelVehicleSpec(1000.0, 75.0) },
		{ FName(TEXT("FUEL")), FFuelVehicleSpec(10000.0, 200.0) } };

	/** How fast a depot refills a returning vehicle, litres per GAME minute per pump module. */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1.0"))
	double DepotRefillLitresPerMinutePerPump = 500.0;

	/**
	 * How many offers the inbox holds before new ones are dropped (spec 2026-09-28 ruling 6).
	 * Copied into UOfferGenerator at attach. The ATC tower may raise it later.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1"))
	int32 MaxPendingOffers = 8;

	/** What moves an airline's satisfaction - see FAirlineSatisfactionTuning. */
	UPROPERTY(EditAnywhere, Category = "Airlines")
	FAirlineSatisfactionTuning AirlineSatisfaction;
};
