#include "Present/OpsRuntimeSubsystem.h"
#include "AirportOpsLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"

void UOpsRuntimeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Runtime = NewObject<UOpsRuntime>(this, TEXT("OpsRuntime"));
	UE_LOG(LogAirportOps, Log, TEXT("OpsRuntimeSubsystem initialised"));
}

void UOpsRuntimeSubsystem::Deinitialize()
{
	// UNSUBSCRIBE BEFORE Runtime GOES: OnActorSpawned closes over `this`, and a world outliving
	// this subsystem (a game instance shutting down mid-level) must not call back into it.
	if (SubscribedWorld.IsValid())
	{
		SubscribedWorld->RemoveOnActorSpawnedHandler(SpawnHandle);
	}
	SubscribedWorld.Reset();
	Runtime = nullptr;
	Super::Deinitialize();
}

void UOpsRuntimeSubsystem::EnsureAttached()
{
	// Re-attach when the target is gone as well as when it was never found: a PIE stop
	// destroys the level's actor while this game-instance subsystem lives on.
	if (Runtime->GetTarget() != nullptr && IsValid(Runtime->GetTarget()))
	{
		return;
	}
	UWorld* World = GetGameInstance() != nullptr ? GetGameInstance()->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return;
	}

	// ONLY ON A WORLD CHANGE (issue #190). This used to run TActorIterator's own scan of the
	// whole level from here, every tick, for as long as the target stayed unfound - the cost
	// this ticket measured. AttachToWorld's OnActorSpawned subscription is what notices a new
	// ARoadNetworkActor from here on; the comparison below only re-arms it when GetWorld()
	// itself is a DIFFERENT object than the one already subscribed, which a stale handle on
	// the old world would never fire for again.
	if (SubscribedWorld != World)
	{
		AttachToWorld(*World);
	}
}

void UOpsRuntimeSubsystem::AttachToWorld(UWorld& World)
{
	if (SubscribedWorld.IsValid())
	{
		SubscribedWorld->RemoveOnActorSpawnedHandler(SpawnHandle);
	}
	SubscribedWorld = &World;
	SpawnHandle = World.AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(this, &UOpsRuntimeSubsystem::OnActorSpawned));

	// THE CATCH-UP SCAN, exactly once for this world. A level-resident actor placed before
	// this subsystem noticed the world (this class's own header) predates the handler just
	// armed above, which only fires for spawns FROM HERE ON - so one scan finds whichever one
	// is already there. GetActorScanCountForTest is what a test holds this "once" to.
	++ActorScanCountForTest;
	for (TActorIterator<ARoadNetworkActor> It(&World); It; ++It)
	{
		Runtime->Attach(*It);
		break;
	}
}

void UOpsRuntimeSubsystem::OnActorSpawned(AActor* Actor)
{
	// NOTHING TO DO if something is already attached and alive - a second
	// ARoadNetworkActor spawning (there should never be one) must not steal the target from
	// a live one, the same "first found, then stop" rule AttachToWorld's own scan follows.
	if (Runtime->GetTarget() != nullptr && IsValid(Runtime->GetTarget()))
	{
		return;
	}
	if (ARoadNetworkActor* RoadNetworkActor = Cast<ARoadNetworkActor>(Actor))
	{
		Runtime->Attach(RoadNetworkActor);
	}
}

void UOpsRuntimeSubsystem::Tick(float DeltaTime)
{
	EnsureAttached();
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
