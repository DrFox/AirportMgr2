#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Engine.h"
#include "Content/AirsideSettings.h"
#include "Entities/AircraftType.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/RoadNetworkActor.h"

// FAirsideTestWorld's constructor and destructor: moved out of the header 2026-09-27 so
// SpawnActor<ARoadNetworkActor>() and SpawnActor<AAirsideBuildingsActor>() - templates that
// need both types complete at the point of instantiation - stop forcing Present/
// RoadNetworkActor.h (and its own fan-out) onto every TU that includes AirsideTestWorld.h
// through AirsideTestFixtures.h, whether or not that test ever touches the actor. See the
// header's own comment on the forward declarations this leaves behind.
FAirsideTestWorld::FAirsideTestWorld(bool bSpawnActor, EWorldType::Type WorldType)
{
	World = UWorld::CreateWorld(WorldType, false);
	if (World == nullptr) { return; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(WorldType);
	Context.SetCurrentWorld(World);
	if (bSpawnActor)
	{
		Actor = World->SpawnActor<ARoadNetworkActor>();
		Buildings = World->SpawnActor<AAirsideBuildingsActor>();
	}
}

FAirsideTestWorld::~FAirsideTestWorld()
{
	if (World == nullptr) { return; }
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
}

TArray<UAircraftType*> EveryAircraftType()
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
		TEXT("AssetRegistry")).Get();
	// WAITED FOR, as EveryVehicleType() and AnimYardCatalogue wait - see this function's own
	// header comment for why an unfinished scan is worse than a slow one here.
	Registry.WaitForCompletion();

	// THE ONE SCAN (#432) - UAirsideSettings::EveryAircraftType, meshed only: NOT THE PAPER TYPES, see this
	// function's header comment. It had its own copy of that rule, and the Land panel a third; this helper
	// adds the wait above and the floor below, and nothing else.
	TArray<UAircraftType*> Out = UAirsideSettings::EveryAircraftType(/*bMeshedOnly*/ true);

	// A FLOOR, NOT AN EXACT COUNT - see the header comment for why 12 and why ensureAlways
	// rather than a silent return.
	ensureAlwaysMsgf(Out.Num() >= 12,
		TEXT("EveryAircraftType() found only %d type(s) with a mesh - the asset registry scan "
			"is likely incomplete or the checkout has no content"), Out.Num());
	return Out;
}

#endif // WITH_DEV_AUTOMATION_TESTS
