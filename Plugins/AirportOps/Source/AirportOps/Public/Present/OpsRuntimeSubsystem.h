#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OpsRuntimeSubsystem.generated.h"

class UOpsRuntime;

/**
 * Gives UOpsRuntime a lifetime in play. A GAME INSTANCE subsystem because it must outlive a
 * level load (save/load, later a main menu); a world subsystem would die with the map.
 * FTickableGameObject because game-instance subsystems do not tick on their own, and the
 * alternative - ticking from the player controller - would tie the sim clock to a pawn.
 *
 * A forwarder only. Nothing here has logic worth a test; the test drives UOpsRuntime.
 * Attaches lazily to the first ARoadNetworkActor it finds in the game instance's world,
 * because the actor is level-resident and does not exist when the subsystem initialises.
 */
UCLASS()
class AIRPORTOPS_API UOpsRuntimeSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return Runtime != nullptr; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UOpsRuntimeSubsystem, STATGROUP_Tickables); }
	virtual bool IsTickableInEditor() const override { return false; }

	UOpsRuntime* GetRuntime() const { return Runtime; }

	/** The runtime for a world's game instance, or null (editor worlds have no game instance). */
	static UOpsRuntime* Get(const UWorld* World);

private:
	UPROPERTY() TObjectPtr<UOpsRuntime> Runtime;

	void EnsureAttached();
};
