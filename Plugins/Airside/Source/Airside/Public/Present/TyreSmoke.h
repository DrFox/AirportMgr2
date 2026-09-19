#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "TyreSmoke.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * The puffs of tyre smoke under the main wheels at touchdown, and the project's FIRST
 * effect - so this class is the precedent for any that follow.
 *
 * A FIXED POOL, NOT SPAWN-AND-DESTROY. Every puff is a UStaticMeshComponent created once and
 * reused round-robin, because a landing spawns two at a stroke and the alternative is two
 * component registrations and two destructions per arrival forever. The pool being fixed is
 * also what makes the worst case knowable: PoolSize puffs is all there will ever be, however
 * many aircraft land at once, and the oldest is stolen rather than the newest refused -
 * a refused puff is invisible, a stolen one is a puff that ended early.
 *
 * A SPHERE, not a camera-facing sprite, and that is the art direction rather than
 * convenience. The direction is a "living airport diorama"; a model-maker's puff of smoke is
 * a blob of cotton wool, not a wisp, and a chunky sphere eaten away by noise reads as MADE
 * rather than simulated. It also sidesteps the real problem billboards have here: the build
 * camera now sits nearly horizontal at full zoom, and edge-on is where sprites look worst.
 *
 * NOT ATTACHED TO THE AIRCRAFT. A puff is left BEHIND - at Vref the aircraft covers 36-44 m
 * in the first second, so by the time a puff has finished billowing its aircraft is most of
 * a wingspan past it. Attaching would drag the smoke down the runway like a scarf, which is
 * the single most obvious way to make this look wrong.
 *
 * WHY NOT NIAGARA is worth recording, because the answer would be different for the next
 * effect. Niagara is the engine's tool and is right the moment there is a second one; it was
 * refused here only because a Niagara system is hand-authored and cannot be rebuilt from
 * Tools/Python, which would have made it the only asset in the project that cannot. For two
 * puffs on one event this is less machinery, not more. It does not scale - see
 * build_puff_material.py, which says the same thing from the other side.
 */
UCLASS()
class AIRSIDE_API UTyreSmoke : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Give this object the actor to parent its puff components to. Until then Puff() does
	 * nothing, which is a supported state - an actor made before this class existed has no
	 * pool and simply never smokes.
	 */
	void Initialise(AActor* InOwner, UMaterialInterface* InMaterial);

	/**
	 * Lay one puff at a world position, sized for an aircraft of this wingspan.
	 *
	 * Wingspan rather than weight, because wingspan is measured on every airframe and weight
	 * is not - see FAirframe. It is a stand-in for mass and says so: a bigger aeroplane
	 * arrives heavier and smokes more, and span is the number that tracks that without
	 * inventing a figure nobody has measured.
	 */
	void Puff(const FVector& Where, double Wingspan);

	/** Age every live puff, retiring those that have finished. Called from the actor's tick. */
	void Advance(double DeltaSeconds);

	/** Live puffs right now, for Airside.Present.TyreSmokePuffs. */
	int32 LivePuffCountForTest() const;

	/** How long a puff lasts, seconds. Short: this is punctuation, not weather. */
	UPROPERTY(EditAnywhere, Category = "Airside|Smoke", meta = (ClampMin = "0.05"))
	double PuffSeconds = 1.3;

	/** Radius at birth and at death, uu, before the wingspan scaling below. */
	UPROPERTY(EditAnywhere, Category = "Airside|Smoke", meta = (ClampMin = "1.0"))
	double BirthRadius = 40.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Smoke", meta = (ClampMin = "1.0"))
	double DeathRadius = 260.0;

	/**
	 * Wingspan that BirthRadius/DeathRadius are quoted for, uu. A wider aircraft scales up
	 * from here and a narrower one down.
	 *
	 * 2842 is the Q400's span, the type this was tuned against - named rather than left as a
	 * bare number so that "sized for a Q400" is readable at the point it matters.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Smoke", meta = (ClampMin = "1.0"))
	double ReferenceWingspan = 2842.0;

	/** How far a puff rises over its life, uu. Smoke goes up; not far, and not fast. */
	UPROPERTY(EditAnywhere, Category = "Airside|Smoke")
	double RiseHeight = 70.0;

	/**
	 * How many puffs may be alive at once. Two per landing and a landing every few seconds,
	 * so eight covers four overlapping arrivals - far past anything a runway allows.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Smoke", meta = (ClampMin = "1"))
	int32 PoolSize = 8;

private:
	/** One pooled puff: a component, its instance, and how far through its life it is. */
	struct FPuff
	{
		TObjectPtr<UStaticMeshComponent> Mesh;
		TObjectPtr<UMaterialInstanceDynamic> Instance;
		/** Seconds since birth. At or past PuffSeconds the puff is dead and hidden. */
		double Age = 0.0;
		double Radius = 0.0;
		FVector Born = FVector::ZeroVector;
		bool bLive = false;
	};

	/** Index of the puff to use next: the first dead one, else the OLDEST live one. */
	int32 ClaimSlot();

	UPROPERTY(Transient) TObjectPtr<AActor> Owner;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Material;

	/**
	 * The pool. Not a UPROPERTY-visible TArray of the struct, because FPuff is a plain struct
	 * with TObjectPtrs in it: the two component arrays below are what the garbage collector
	 * traces, and Puffs indexes them.
	 */
	TArray<FPuff> Puffs;

	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> PuffMeshes;
	UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> PuffInstances;
};
