#include "Content/AirportOpsSettings.h"
#include "AirportOpsLog.h"
#include "Model/OpsDefinition.h"

const UScenario* UAirportOpsSettings::ResolveDefaultScenario()
{
	const UAirportOpsSettings* Settings = GetDefault<UAirportOpsSettings>();
	if (Settings->DefaultScenario.IsNull())
	{
		// Once, not per attach: a supported state deserves one line, not a nag.
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogAirportOps, Log, TEXT("No DefaultScenario configured; clock and balance use built-in defaults"));
		}
		return nullptr;
	}
	const UScenario* Scenario = Settings->DefaultScenario.LoadSynchronous();
	if (Scenario == nullptr)
	{
		UE_LOG(LogAirportOps, Error, TEXT("DefaultScenario '%s' is configured but failed to load"),
			*Settings->DefaultScenario.ToString());
	}
	return Scenario;
}
