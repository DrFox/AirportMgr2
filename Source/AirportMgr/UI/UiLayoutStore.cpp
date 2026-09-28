#include "UI/UiLayoutStore.h"

#include "AirportMgrUserSettings.h"

TOptional<FUiWindowPlacement> FMemoryUiLayoutStore::Read(FName Id) const
{
	const FUiWindowPlacement* Found = Values.Find(Id);
	return Found != nullptr ? TOptional<FUiWindowPlacement>(*Found) : TOptional<FUiWindowPlacement>();
}

void FMemoryUiLayoutStore::Write(FName Id, const FUiWindowPlacement& Placement)
{
	Values.Add(Id, Placement);
	++WriteCount;
}

void FMemoryUiLayoutStore::Clear()
{
	Values.Reset();
}

TOptional<FUiWindowPlacement> FUserSettingsLayoutStore::Read(FName Id) const
{
	const UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get();
	const FUiWindowPlacement* Found = Settings != nullptr ? Settings->WindowLayout.Find(Id) : nullptr;
	return Found != nullptr ? TOptional<FUiWindowPlacement>(*Found) : TOptional<FUiWindowPlacement>();
}

void FUserSettingsLayoutStore::Write(FName Id, const FUiWindowPlacement& Placement)
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->WindowLayout.Add(Id, Placement);
		Settings->SaveSettings();
	}
}

void FUserSettingsLayoutStore::Clear()
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->WindowLayout.Reset();
		Settings->SaveSettings();
	}
}

void FUserSettingsLayoutStore::Remove(FName Id)
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->WindowLayout.Remove(Id);
		Settings->SaveSettings();
	}
}
