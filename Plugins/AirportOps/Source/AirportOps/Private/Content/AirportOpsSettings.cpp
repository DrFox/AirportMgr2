#include "Content/AirportOpsSettings.h"
#include "AirportOpsLog.h"
#include "Model/OpsCatalog.h"
#include "Model/OpsDefinition.h"

const UScenario* UAirportOpsSettings::ResolveDefaultScenario(const UOpsCatalog& Catalog)
{
	const UAirportOpsSettings* Settings = GetDefault<UAirportOpsSettings>();
	if (Settings->DefaultScenario.IsValid())
	{
		// The catalog, not a load of our own: LoadFromAssetManager already put every
		// UScenario it found there, by name, and this is the one place that gets to pick
		// which name is the default rather than a second place that also loads one.
		if (const UScenario* Found = Catalog.Find<UScenario>(Settings->DefaultScenario.PrimaryAssetName))
		{
			return Found;
		}
		UE_LOG(LogAirportOps, Error,
			TEXT("DefaultScenario '%s' is configured but not in the catalog; falling back to built-in defaults"),
			*Settings->DefaultScenario.ToString());
		return GetDefault<UScenario>();
	}

	// Once, not per attach: a supported state deserves one line, not a nag.
	static bool bWarned = false;
	if (!bWarned)
	{
		bWarned = true;
		UE_LOG(LogAirportOps, Log, TEXT("No DefaultScenario configured; using UScenario's built-in defaults"));
	}

	// THE CDO, NOT NULL, and this used to be null - which quietly made the log line
	// above a lie. Every caller guards with `if (Scenario)`, so returning null skipped
	// the whole apply block and the "built-in defaults" were never applied to anything:
	// the clock kept USimClock's own zero rather than the scenario's start hour, and a
	// project with no scenario asset silently opened at midnight.
	//
	// The CDO carries exactly the values UScenario declares, so there is ONE source of
	// truth for a default instead of a declared one and an implied one that drift.
	return GetDefault<UScenario>();
}
