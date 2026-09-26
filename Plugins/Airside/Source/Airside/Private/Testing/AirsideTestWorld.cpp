#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Entities/AircraftType.h"

TArray<UAircraftType*> EveryAircraftType()
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();
	// WAITED FOR, as EveryVehicleType() and AnimYardCatalogue wait - see this function's own
	// header comment for why an unfinished scan is worse than a slow one here.
	Registry.WaitForCompletion();

	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UAircraftType::StaticClass()->GetClassPathName(), Assets);

	TArray<UAircraftType*> Out;
	for (const FAssetData& Data : Assets)
	{
		if (UAircraftType* Type = Cast<UAircraftType>(Data.GetAsset()))
		{
			// NOT THE PAPER TYPES - see this function's header comment.
			if (!Type->Mesh.IsNull())
			{
				Out.Add(Type);
			}
		}
	}

	// A FLOOR, NOT AN EXACT COUNT - see the header comment for why 12 and why ensureAlways
	// rather than a silent return.
	ensureAlwaysMsgf(Out.Num() >= 12,
		TEXT("EveryAircraftType() found only %d type(s) with a mesh - the asset registry scan "
			"is likely incomplete or the checkout has no content"), Out.Num());
	return Out;
}

#endif // WITH_DEV_AUTOMATION_TESTS
