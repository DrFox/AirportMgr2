#include "Model/OpsCatalog.h"
#include "AirportOpsLog.h"
#include "Engine/AssetManager.h"
#include "UObject/UObjectIterator.h"

void UOpsCatalog::Add(UOpsDefinition* Definition)
{
	if (Definition == nullptr || Definitions.Contains(Definition))
	{
		return;
	}
	Definitions.Add(Definition);
}

int32 UOpsCatalog::LoadFromAssetManager()
{
	if (!UAssetManager::IsInitialized())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("OpsCatalog: Asset Manager not initialised; nothing loaded"));
		return 0;
	}
	UAssetManager& Manager = UAssetManager::Get();
	int32 Added = 0;

	// Every concrete subclass of UOpsDefinition is a type to scan. Derived from the class
	// hierarchy rather than a hand-kept list so a new definition class cannot be forgotten
	// here - though it CAN still be forgotten in DefaultGame.ini, which the per-type log
	// line below is there to show.
	TArray<UClass*> Types;
	GetDerivedClasses(UOpsDefinition::StaticClass(), Types, /*bRecursive*/ true);
	for (UClass* Type : Types)
	{
		if (Type->HasAnyClassFlags(CLASS_Abstract)) { continue; }
		const FPrimaryAssetType AssetType(Type->GetFName());
		TArray<FPrimaryAssetId> Ids;
		Manager.GetPrimaryAssetIdList(AssetType, Ids);
		int32 ForType = 0;
		for (const FPrimaryAssetId& Id : Ids)
		{
			const FSoftObjectPath Path = Manager.GetPrimaryAssetPath(Id);
			if (UOpsDefinition* Loaded = Cast<UOpsDefinition>(Path.TryLoad()))
			{
				Add(Loaded);
				++ForType;
				++Added;
			}
		}
		UE_LOG(LogAirportOps, Log, TEXT("OpsCatalog: %d %s(s)"), ForType, *Type->GetName());
	}
	return Added;
}
