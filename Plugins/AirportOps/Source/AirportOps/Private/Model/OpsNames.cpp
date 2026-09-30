#include "Model/OpsNames.h"

#include "AirportOpsLog.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

FString OpsNames::StandLabel(const URoadNetwork* Network, FEntityInstanceId Stand)
{
	const FEntityInstance* Entity = Network != nullptr ? Network->GetEntity(Stand) : nullptr;
	if (Entity != nullptr && Entity->StandNumber > 0)
	{
		return FString::FromInt(Entity->StandNumber);
	}
	if (Entity != nullptr)
	{
		// ALIVE AND UNNUMBERED: the invariant Airside.Model.StandNumbers pins has broken, and "stand 0" beside a card
		// that says something else is the symptom - so the index goes out, and the log says why, once.
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogAirportOps, Warning,
				TEXT("OpsNames: entity %d is an unnumbered stand - EnsureStandNumbers did not run? Naming it by index."), Stand.Index);
		}
	}
	return FString::FromInt(Stand.Index);
}
