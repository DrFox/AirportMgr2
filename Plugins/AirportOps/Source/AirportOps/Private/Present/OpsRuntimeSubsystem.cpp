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
	for (TActorIterator<ARoadNetworkActor> It(World); It; ++It)
	{
		Runtime->Attach(*It);
		break;
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
