#include "AirportMgrUserSettings.h"

#include "Engine/Engine.h"
#include "RoadBuildLog.h"

UAirportMgrUserSettings* UAirportMgrUserSettings::Get()
{
	UAirportMgrUserSettings* Settings = GEngine != nullptr ? Cast<UAirportMgrUserSettings>(GEngine->GetGameUserSettings()) : nullptr;
	if (Settings == nullptr)
	{
		// Once, not per call: the store asks on every read and write.
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogRoadBuild, Error, TEXT("GameUserSettings is not a UAirportMgrUserSettings - is GameUserSettingsClassName set in DefaultEngine.ini? Nothing will be remembered."));
		}
	}
	return Settings;
}
