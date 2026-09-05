#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AirportOpsSettings.generated.h"

class UScenario;

/**
 * Project Settings > Game > AirportOps. The one place the default scenario is named; every
 * caller goes through ResolveDefaultScenario. Mirrors UAirsideSettings for the same reason
 * it exists: a path typed at a second call site is a second source of truth.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "AirportOps"))
class AIRPORTOPS_API UAirportOpsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(config, EditAnywhere, Category = "Content")
	TSoftObjectPtr<UScenario> DefaultScenario;

	/**
	 * The configured scenario, loaded on first use, or null when none is set. Null is a
	 * SUPPORTED state - the clock and balance fall back to their constructor defaults and
	 * the log says so once. A configured scenario that fails to load is an error and is
	 * logged as one.
	 */
	static const UScenario* ResolveDefaultScenario();
};
