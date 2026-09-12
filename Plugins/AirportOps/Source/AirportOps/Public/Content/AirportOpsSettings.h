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
	 * The configured scenario, or UScenario's CDO when none is configured or one fails to
	 * load. NEVER NULL, deliberately: a null made every caller's `if (Scenario)` skip the
	 * apply block, so the "built-in defaults" the log promised were applied to nothing and a
	 * project without a scenario asset silently opened its clock at midnight.
	 */
	static const UScenario* ResolveDefaultScenario();
};
