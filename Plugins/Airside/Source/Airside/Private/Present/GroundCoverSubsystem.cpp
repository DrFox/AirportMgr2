#include "Present/GroundCoverSubsystem.h"

#include "Content/AirsideSettings.h"
#include "Engine/World.h"
#include "Present/AirsideGroundCoverActor.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadNetworkRegistry.h"

AAirsideGroundCoverActor* UGroundCoverSubsystem::FindActor(const UWorld* World)
{
	const UGroundCoverSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UGroundCoverSubsystem>() : nullptr;
	return Subsystem != nullptr ? Subsystem->Actor.Get() : nullptr;
}

bool UGroundCoverSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UGroundCoverSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	RegistryHandle = URoadNetworkRegistry::OnAirportChanged().AddUObject(this, &UGroundCoverSubsystem::OnAirportChanged);
}

void UGroundCoverSubsystem::Deinitialize()
{
	URoadNetworkRegistry::OnAirportChanged().Remove(RegistryHandle);
	RegistryHandle.Reset();
	Super::Deinitialize();
}

void UGroundCoverSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (ARoadNetworkActor* Road = URoadNetworkRegistry::Find(&InWorld))
	{
		EnsureActorBoundTo(*Road);
	}
}

void UGroundCoverSubsystem::OnAirportChanged(UWorld& World, ARoadNetworkActor& Airport, EAirportRegistration Change)
{
	// ONE LIST FOR EVERY WORLD (the registry's own comment): filter to ours.
	if (&World != GetWorld() || !World.HasBegunPlay())
	{
		return;
	}
	switch (Change)
	{
	case EAirportRegistration::Arrived:
		EnsureActorBoundTo(Airport);
		return;
	case EAirportRegistration::Left:
		if (Actor != nullptr && Actor->GetRoadNetwork() == &Airport)
		{
			Actor->BindTo(nullptr);
		}
		return;
	}
}

void UGroundCoverSubsystem::EnsureActorBoundTo(ARoadNetworkActor& Road)
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	if (Actor == nullptr)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		Actor = World->SpawnActor<AAirsideGroundCoverActor>(Params);
		if (Actor == nullptr)
		{
			return;
		}
		Actor->SetKit(UAirsideSettings::ResolveGroundCover());
	}
	Actor->BindTo(&Road);
}
