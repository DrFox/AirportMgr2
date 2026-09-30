#include "Present/RoadNetworkRegistry.h"
#include "AirsideLog.h"
#include "Engine/World.h"
#include "Present/RoadNetworkActor.h"

ARoadNetworkActor* URoadNetworkRegistry::Find(const UWorld* World)
{
	const URoadNetworkRegistry* Registry = World != nullptr ? World->GetSubsystem<URoadNetworkRegistry>() : nullptr;
	return Registry != nullptr ? Registry->GetAirport() : nullptr;
}

void URoadNetworkRegistry::Register(ARoadNetworkActor& Actor)
{
	ARoadNetworkActor* Holder = Registered.Get();
	if (Holder == &Actor)
	{
		// ALREADY HOLDS IT: every reregister of its components (a Details edit, an editor undo, a construction-script
		// rerun) runs PostRegisterAllComponents again. The actor never gave the slot back - it leaves only when it goes
		// (ARoadNetworkActor::EndPlay, or its UnregisterAllComponents while being destroyed) - so this is the same
		// actor asking again, not a second one, and no change anyone should hear about.
		// ENFORCED BY: AirportOps.Present.OpsRuntimeSubsystemSurvivesAReregister
		return;
	}
	if (Holder != nullptr)
	{
		// NEVER A GUESS BETWEEN TWO - see the class comment. The first keeps the slot; this says which to delete.
		UE_LOG(LogAirside, Error,
			TEXT("Airport registry: %s refused in %s - %s is already this world's airport. One ARoadNetworkActor per ")
			TEXT("level: the build tools, ops and the buildings all use %s; delete the other."),
			*Actor.GetName(), *GetNameSafe(GetWorld()), *Holder->GetName(), *Holder->GetName());
		return;
	}
	Registered = &Actor;
	// The line to grep for "which airport is ops running": one per world per arrival.
	UE_LOG(LogAirside, Log, TEXT("Airport registry: %s is %s's airport"), *Actor.GetName(), *GetNameSafe(GetWorld()));
	if (UWorld* World = GetWorld())
	{
		OnAirportChanged().Broadcast(*World, Actor, EAirportRegistration::Arrived);
	}
}

void URoadNetworkRegistry::Unregister(ARoadNetworkActor& Actor)
{
	if (Registered.Get() != &Actor)
	{
		return;
	}
	Registered.Reset();
	UE_LOG(LogAirside, Log, TEXT("Airport registry: %s left %s"), *Actor.GetName(), *GetNameSafe(GetWorld()));
	if (UWorld* World = GetWorld())
	{
		OnAirportChanged().Broadcast(*World, Actor, EAirportRegistration::Left);
	}
}

URoadNetworkRegistry::FOnAirportChanged& URoadNetworkRegistry::OnAirportChanged()
{
	// A FUNCTION-LOCAL STATIC, not a static data member: constructed on first use, so no module's
	// static initialisation order can reach it before it exists.
	static FOnAirportChanged Delegate;
	return Delegate;
}
