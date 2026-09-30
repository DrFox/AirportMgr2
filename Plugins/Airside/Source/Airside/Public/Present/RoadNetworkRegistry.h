#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/WeakObjectPtr.h"
#include "RoadNetworkRegistry.generated.h"

class ARoadNetworkActor;

/**
 * Which way a world's airport moved, for URoadNetworkRegistry::OnAirportChanged. A PHASE, SO ONE ENUM (CLAUDE.md), not a
 * nullable pointer standing for "left". Plain, not a UENUM: it travels on a native delegate only (EChangeKind's reason).
 */
enum class EAirportRegistration : uint8
{
	Arrived,
	Left
};

/**
 * WHICH ACTOR IS THIS WORLD'S AIRPORT - one answer, owned here (#446).
 *
 * Until #446 four lookups answered it with two different rules: ARoadNetworkActor::Find and ops'
 * catch-up scan took the FIRST TActorIterator hit, ops also took whichever network actor SPAWNED
 * first (an OnActorSpawned hook) and re-checked IsValid on its target every tick, and the buildings
 * actor refused to guess between two. With two network actors in a level the driver built into one,
 * ops ran the other, and the depots vanished - three answers, none of them wrong by its own rule.
 *
 * THE ACTOR REGISTERS ITSELF (ARoadNetworkActor::PostRegisterAllComponents) and gives the slot back
 * when it GOES (EndPlay, or UnregisterAllComponents while being destroyed - never on a reregister). Nobody searches: a lookup is a read of the slot, and a listener
 * hears OnAirportChanged instead of polling for the answer to move.
 *
 * NEVER A GUESS BETWEEN TWO - the buildings actor's rule, made the registry's. A SECOND actor asking
 * for the slot while one holds it is REFUSED LOUDLY: an Error naming both, and the first keeps it.
 * Not an ensure: a level saved with two is a content mistake the player's session survives, and the
 * Error is what says which actor to delete. Not a promotion either, when the first leaves: a refused
 * actor is not remembered, so which one the game runs never depends on teardown order.
 * ENFORCED BY: Airside.Present.Registry.SecondAirportIsRefusedLoudly
 *
 * A WORLD SUBSYSTEM, so the editor world, each PIE world and each test fixture's world has its own
 * slot - the editor mode (URoadBuildEdMode) asks the editor world's, which PIE never touches.
 * UWorldSubsystem's default DoesSupportWorldType covers Game, Editor and PIE, which is every world a
 * network actor is placed or spawned in; an EditorPreview world has none.
 */
UCLASS()
class AIRSIDE_API URoadNetworkRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** The airport registered in World, or nullptr (no world, no subsystem, or nothing registered yet). */
	static ARoadNetworkActor* Find(const UWorld* World);

	/**
	 * Claims World's slot for Actor: takes an empty one, or finds Actor already holds it (its components
	 * reregistered). Another live actor holding it REFUSES Actor, with an Error naming both.
	 * VOID, NOT A bool (#446 review): the one caller had nothing to do with a refusal the Error has
	 * already reported, and a returned bool nobody reads is the discarded-result shape CLAUDE.md bans.
	 * Who holds the slot is Find's answer, which is what a test of the refusal reads.
	 */
	void Register(ARoadNetworkActor& Actor);

	/** Gives the slot back if Actor holds it; anything else is a no-op (a refused actor, a second call). */
	void Unregister(ARoadNetworkActor& Actor);

	/** The registered actor, or nullptr. */
	ARoadNetworkActor* GetAirport() const { return Registered.Get(); }

	/**
	 * A world's airport arrived or left - the actor EITHER WAY, and which way as a phase. STATIC, one list
	 * for every world, because its main listener outlives worlds: the ops runtime's subsystem is a GAME
	 * INSTANCE one, alive before the PIE world's actors register and after they go, and a per-world
	 * delegate would need it to find each new world's registry before that world's airport registers - the
	 * race the spawn hook existed for. A listener filters by World. Fired AFTER the slot changes, so a
	 * listener's own Find agrees.
	 * THE LEAVER IS NAMED (#446 review): it was a null "left", so a listener could not tell the airport it
	 * holds leaving from any other; ops now detaches only when the leaver is its own target.
	 * ENFORCED BY: AirportOps.Present.OpsRuntimeSubsystemReattaches, AirportOps.Present.OpsRuntimeSubsystemSurvivesAReregister
	 */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAirportChanged, UWorld& /*World*/, ARoadNetworkActor& /*Airport*/, EAirportRegistration /*Change*/);
	static FOnAirportChanged& OnAirportChanged();

private:
	/**
	 * WEAK: the actor owns its registration, and an actor destroyed without its EndPlay (a GC'd
	 * editor-world actor, a world torn down under it) must read as gone, not dangle.
	 */
	TWeakObjectPtr<ARoadNetworkActor> Registered;
};
