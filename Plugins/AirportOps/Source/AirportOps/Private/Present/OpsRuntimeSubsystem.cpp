#include "Present/OpsRuntimeSubsystem.h"
#include "AirportOpsLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Present/RoadNetworkRegistry.h"

void UOpsRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Runtime = NewObject<UOpsRuntime>(this, TEXT("OpsRuntime"));
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntimeSubsystem initialised"));

	// HEARD, NOT SEARCHED FOR (#446) - see the class comment.
	AirportHandle = URoadNetworkRegistry::OnAirportChanged().AddUObject(this, &UOpsRuntimeSubsystem::OnAirportChanged);

	// THE ONE CATCH-UP READ, for an airport that registered before this subsystem existed. A level-resident
	// actor placed before this subsystem noticed the world predates a listener that only hears arrivals FROM
	// HERE ON - the case the catch-up scan was for. In PIE the game instance's subsystems initialise before the
	// world's actors register (UGameInstance::InitializeForPlayInEditor), so this normally finds nothing; a game
	// instance made over a live world would otherwise wait for an arrival that already happened. A read of the
	// registry's slot, not a scan: the world already said which actor it is.
	UWorld* World = GetGameInstance() != nullptr ? GetGameInstance()->GetWorld() : nullptr;
	if (ARoadNetworkActor* Airport = URoadNetworkRegistry::Find(World))
	{
		Runtime->Attach(Airport);
	}
}

void UOpsRuntimeSubsystem::Deinitialize()
{
	// UNSUBSCRIBE BEFORE Runtime GOES: the registry's list closes over `this`, and it is static - a world
	// outliving this subsystem (a game instance shutting down mid-level) must not call back into it.
	URoadNetworkRegistry::OnAirportChanged().Remove(AirportHandle);
	AirportHandle.Reset();
	// THE ONE DETACH, here too (#445): the registry's Left announcement is what detaches while a PIE stops, and that handle is gone two
	// lines up - so a game instance that shuts down with its airport still attached would let Runtime go with the facade still holding
	// a raw IBuildPurse* to its ledger (RoadEditFacade::Purse) and the actor's delegates still bound to it. Detach is idempotent
	// ("safe with nothing attached"), so the path that ran first - Left's - makes this one a no-op; it is the same call, not a second way.
	// ENFORCED BY: AirportOps.Present.OpsRuntimeSubsystemDetachesOnDeinitialize
	if (Runtime != nullptr)
	{
		Runtime->Detach();
	}
	Runtime = nullptr;
	Super::Deinitialize();
}

// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a third way for an airport to move must say what ops does.
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void UOpsRuntimeSubsystem::OnAirportChanged(UWorld& World, ARoadNetworkActor& Airport, EAirportRegistration Change)
{
	// OURS ONLY: the editor world's airport, and another PIE instance's, announce on the same list.
	if (Runtime == nullptr || World.GetGameInstance() != GetGameInstance())
	{
		return;
	}
	switch (Change)
	{
	case EAirportRegistration::Arrived:
		// THE AIRPORT BEING PLAYED IS NOT A NEW ONE. Attach is a new game - the clock restarts, the ledger reopens,
		// airlines and alerts reset - so a re-announcement of the target (a registry that ever read a reregister as
		// leave-then-arrive) must cost nothing. Any OTHER airport attaches; Attach detaches the old one first, and a
		// level change hands the same game instance a new world.
		if (Runtime->GetTarget() == &Airport)
		{
			return;
		}
		Runtime->Attach(&Airport);
		return;
	case EAirportRegistration::Left:
		// THE AIRPORT LEFT (a PIE stop, a level unload, the actor destroyed): a real Detach, while its traffic and
		// facade still exist to unbind from - see ARoadNetworkActor::EndPlay. Until #446 nothing unbound: a per-tick
		// IsValid noticed the target had gone and re-attached to whatever came next. ONLY OUR TARGET'S departure:
		// a refused second actor never held the slot, and another airport leaving is not ours leaving.
		if (Runtime->GetTarget() != &Airport)
		{
			return;
		}
		UE_LOG(LogAirportOps, Log, TEXT("OpsRuntime detached: %s left %s"), *Airport.GetName(), *World.GetName());
		Runtime->Detach();
		return;
	}
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void UOpsRuntimeSubsystem::Tick(float DeltaTime)
{
	Runtime->Tick(DeltaTime);
}

UOpsRuntime* UOpsRuntimeSubsystem::Get(const UWorld* World)
{
	if (World == nullptr || World->GetGameInstance() == nullptr)
	{
		return nullptr;
	}
	UOpsRuntimeSubsystem* Sub = World->GetGameInstance()->GetSubsystem<UOpsRuntimeSubsystem>();
	return Sub != nullptr ? Sub->GetRuntime() : nullptr;
}
