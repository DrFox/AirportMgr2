#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "UObject/WeakObjectPtr.h"
#include "OpsRuntimeSubsystem.generated.h"

class AActor;
class UOpsRuntime;

/**
 * Gives UOpsRuntime a lifetime in play. A GAME INSTANCE subsystem because it must outlive a
 * level load (save/load, later a main menu); a world subsystem would die with the map.
 * FTickableGameObject because game-instance subsystems do not tick on their own, and the
 * alternative - ticking from the player controller - would tie the sim clock to a pawn.
 *
 * MOSTLY a forwarder; the test drives UOpsRuntime itself for everything but EnsureAttached,
 * which OpsRuntimeSubsystemTest.cpp exists for (issue #194 found nothing measuring it).
 * Attaches to the first ARoadNetworkActor it finds in the game instance's world, because the
 * actor is level-resident and does not exist when the subsystem initialises - lazily, via an
 * OnActorSpawned subscription rather than a scan run from Tick every frame (issue #190): see
 * AttachToWorld.
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

	/** How many times EnsureAttached has run a TActorIterator scan this session - see
	 *  AttachToWorld's own comment on why that is bounded to once per WORLD rather than once
	 *  per TICK. For a test to prove an idle world costs nothing further after the first. */
	int32 GetActorScanCountForTest() const { return ActorScanCountForTest; }

private:
	UPROPERTY() TObjectPtr<UOpsRuntime> Runtime;

	/** The world AttachToWorld last subscribed OnActorSpawned to. A level load can hand the
	 *  same game instance a DIFFERENT UWorld object - the TActorIterator this replaced used
	 *  to just re-scan every tick and never had to care - so EnsureAttached compares against
	 *  this to notice and re-subscribe rather than keep listening to a world nothing spawns
	 *  into any more. WEAK: this subsystem must never be the reason a world outlives its own
	 *  teardown. */
	TWeakObjectPtr<UWorld> SubscribedWorld;

	/** The handle for the OnActorSpawned subscription on SubscribedWorld, removed whenever
	 *  that changes and on Deinitialize - a stale handler must not fire into a Runtime that
	 *  no longer exists. */
	FDelegateHandle SpawnHandle;

	/** See GetActorScanCountForTest. */
	int32 ActorScanCountForTest = 0;

	void EnsureAttached();

	/**
	 * Runs exactly once per world this subsystem ever attaches to (issue #190) - not once
	 * per tick, which is what a TActorIterator over the whole level used to cost for as long
	 * as the target stayed unfound: the entire span before a player has placed a road, or an
	 * entire main menu with no game world's actor at all.
	 *
	 * Arms OnActorSpawned, which is what actually notices a NEW ARoadNetworkActor from here
	 * on, then runs ONE scan to catch the level-resident case this class's own header
	 * describes: an actor already placed before this subsystem noticed the world, which
	 * predates a handler that only fires for spawns FROM HERE ON.
	 */
	void AttachToWorld(UWorld& World);

	/** OnActorSpawned callback: attach to a freshly spawned ARoadNetworkActor if nothing is
	 *  attached right now - the event that lets EnsureAttached stay a validity check instead
	 *  of a scan on every tick after AttachToWorld's one. */
	void OnActorSpawned(AActor* Actor);
};
