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

	/** Real seconds one game day takes at x1. Copied into USimClock by UOpsRuntime at attach. */
	UPROPERTY(EditAnywhere, Category = "Scenario", meta = (ClampMin = "1.0"))
	double RealSecondsPerGameDay = 1200.0;
};
