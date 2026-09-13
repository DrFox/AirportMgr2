#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AirportOpsSettings.generated.h"

class UOpsCatalog;
class UScenario;

/**
 * Project Settings > Game > AirportOps. The one place which scenario is DEFAULT is named -
 * NOT where one is loaded from, which is UOpsCatalog (#104: the two used to disagree about
 * which was "the one place", because DefaultScenario was its own TSoftObjectPtr and loaded
 * the asset a second way, independently of the catalog the rest of the game reads definitions
 * through). Every caller goes through ResolveDefaultScenario. Mirrors UAirsideSettings for
 * the same reason it exists: a path typed at a second call site is a second source of truth.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "AirportOps"))
class AIRPORTOPS_API UAirportOpsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** A NAME, not an asset reference - resolved through the catalog, not loaded here. */
	UPROPERTY(config, EditAnywhere, Category = "Content", meta = (AllowedTypes = "Scenario"))
	FPrimaryAssetId DefaultScenario;

	/**
	 * The configured scenario, found in Catalog, or UScenario's CDO when none is configured
	 * or the name is not in it. NEVER NULL, deliberately: a null made every caller's
	 * `if (Scenario)` skip the apply block, so the "built-in defaults" the log promised were
	 * applied to nothing and a project without a scenario asset silently opened its clock at
	 * midnight.
	 *
	 * Takes the catalog rather than loading DefaultScenario itself: UOpsCatalog::
	 * LoadFromAssetManager is the one place a UOpsDefinition gets loaded from disk (#104) -
	 * this only picks which of the ones already there is the default.
	 */
	static const UScenario* ResolveDefaultScenario(const UOpsCatalog& Catalog);
};
