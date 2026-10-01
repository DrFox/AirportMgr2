#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GroundCoverSubsystem.generated.h"

class AAirsideGroundCoverActor;
class ARoadNetworkActor;
enum class EAirportRegistration : uint8;

/**
 * Puts one grass actor in every GAME and PIE world that has an airport, bound to it.
 *
 * A SUBSYSTEM RATHER THAN A FindOrCreate AT EACH CALL SITE (AAirsideBuildingsActor's way): the
 * buildings actor is saved with the level and must exist in the editor too, so the three
 * sites that make a road network each make one. Grass is runtime-only (spec 1), so the world
 * itself can supply it - no site to forget, and no grass actor in an editor world or a saved
 * level, ever.
 *
 * SPAWNS AT BEGIN PLAY, NOT ON ARRIVAL: the airport registers from its own
 * PostRegisterAllComponents, mid level-load, and spawning an actor there is spawning during
 * another actor's registration. An airport arriving after begin play (a level streamed in)
 * is bound on arrival. A test world never begins play, so no fixture grows grass unasked.
 */
UCLASS()
class AIRSIDE_API UGroundCoverSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** World's grass actor, or nullptr (no subsystem, not begun play, or no airport yet). */
	static AAirsideGroundCoverActor* FindActor(const UWorld* World);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

protected:
	/** Game and PIE only - see the class comment. */
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void OnAirportChanged(UWorld& World, ARoadNetworkActor& Airport, EAirportRegistration Change);

	/** Spawn the actor (resolving the content kit) if there is none, then bind it to Road. */
	void EnsureActorBoundTo(ARoadNetworkActor& Road);

	UPROPERTY(Transient) TObjectPtr<AAirsideGroundCoverActor> Actor;

	FDelegateHandle RegistryHandle;
};
