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
	 * How long a fuel truck stays at the hydrant, in the sim seconds a truck MOVES in - see
	 * UFuelService::DwellSeconds, and its header for why that is not game time.
	 *
	 * Copied into UFuelService by UOpsRuntime at attach, exactly as RealSecondsDaylight is
	 * copied into USimClock: a designer figure lives on the authored asset, and the object
	 * that consumes it is transient.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0"))
	double FuelDwellSeconds = 40.0;

	/**
	 * How many offers the inbox holds before new ones are dropped (spec 2026-09-28 ruling 6).
	 * Copied into UOfferGenerator at attach. The ATC tower may raise it later.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1"))
	int32 MaxPendingOffers = 8;

	/**
	 * GAME seconds the turnaround contract allows for landing and taxiing in. Copied into
	 * UOfferGenerator at attach. A flat figure until B's sequencer can estimate it per flight.
	 */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "0.0"))
	double TaxiAllowanceSeconds = 600.0;
};
