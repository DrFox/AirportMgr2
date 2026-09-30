#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OpsRuntimeSubsystem.generated.h"

class ARoadNetworkActor;
class UOpsRuntime;
class UWorld;
enum class EAirportRegistration : uint8;

/**
 * Gives UOpsRuntime a lifetime in play. A GAME INSTANCE subsystem because it must outlive a
 * level load (save/load, later a main menu); a world subsystem would die with the map.
 * FTickableGameObject because game-instance subsystems do not tick on their own, and the
 * alternative - ticking from the player controller - would tie the sim clock to a pawn.
 *
 * MOSTLY a forwarder; the test drives UOpsRuntime itself for everything but the attach, which
 * OpsRuntimeSubsystemTest.cpp exists for (issue #194 found nothing measuring it).
 *
 * ATTACHES TO THE WORLD'S AIRPORT AS URoadNetworkRegistry ANNOUNCES IT (#446), and DETACHES when it
 * leaves - a real Detach, where there was none: the actor is level-resident and does not exist when
 * this subsystem initialises, and until #446 this found it three ways - a TActorIterator scan once
 * per world (issue #190, which had been once per TICK for as long as the target stayed unfound), an
 * OnActorSpawned hook for the actor spawned after that scan, and an IsValid check on the target every
 * tick to notice a PIE stop. The registry is the one answer to "which airport", so there is nothing
 * to search for and nothing to re-check.
 * ENFORCED BY: AirportOps.Present.OpsRuntimeSubsystemReattaches; Check-Architecture rule 52 (one-airport-lookup)
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

	/** The URoadNetworkRegistry::OnAirportChanged binding - removed on Deinitialize, BEFORE Runtime goes:
	 *  the registry's list is static and outlives this subsystem. */
	FDelegateHandle AirportHandle;

	/**
	 * Airport arrived in or left World. Only a world THIS game instance owns - the list is shared by every
	 * world, the editor's included. An arrival attaches - UNLESS Airport is already the target, since an
	 * Attach is a new game (clock, ledger, airlines, alerts) and must never run for the airport being
	 * played. A departure detaches ONLY WHEN Airport is the target (#446 review: defence in depth
	 * behind ARoadNetworkActor's own rule that a reregister is no departure).
	 * ENFORCED BY: AirportOps.Present.OpsRuntimeSubsystemSurvivesAReregister
	 */
	void OnAirportChanged(UWorld& World, ARoadNetworkActor& Airport, EAirportRegistration Change);
};
