#include "Tool/ToolPreferences.h"

#include "Misc/ConfigCacheIni.h"

TOptional<FString> FMemoryToolPreferences::Read(const FString& Key) const
{
	const FString* Found = Values.Find(Key);
	return Found != nullptr ? TOptional<FString>(*Found) : TOptional<FString>();
}

void FMemoryToolPreferences::Write(const FString& Key, const FString& Value)
{
	Values.Add(Key, Value);
	++WriteCount;
}

const TCHAR* FConfigToolPreferences::Section = TEXT("Airside.ToolPreferences");

// GGameUserSettingsIni, ALWAYS. Not a filename parameter: GConfig only writes to a file it has
// already loaded, so a test handing in a throwaway name would have seen every Write vanish and
// every Read come back unset - a store that "works" by never storing.
TOptional<FString> FConfigToolPreferences::Read(const FString& Key) const
{
	FString Value;
	if (GConfig != nullptr && GConfig->GetString(Section, *Key, Value, GGameUserSettingsIni))
	{
		return Value;
	}
	return {};
}

void FConfigToolPreferences::Write(const FString& Key, const FString& Value)
{
	if (GConfig == nullptr)
	{
		return;
	}
	GConfig->SetString(Section, *Key, *Value, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void FConfigToolPreferences::Remove(const FString& Key)
{
	if (GConfig == nullptr)
	{
		return;
	}
	GConfig->RemoveKey(Section, *Key, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}
