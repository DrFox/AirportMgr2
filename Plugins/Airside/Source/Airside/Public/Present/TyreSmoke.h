#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "TyreSmoke.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;

/**
 * One pooled puff: a component, its instance, and how far through its life it is.
 *
 * A USTRUCT WITH UPROPERTY POINTERS (issue #192 item 2), not a plain struct beside two
 * index-parallel UPROPERTY arrays that exist only to root what the struct itself could not.
 * DynamicMeshSink.h's own comment names that shape "the defect this codebase has already
 * paid for once" - two lists that must agree, kept in step by nothing but every writer
 * remembering to touch both. A reflected TArray<FTyreSmokePuff> is the garbage collector's
 * own list, so there is exactly one to keep in step, and reflection is what makes it
 * root-able at all: an unreflected struct's TObjectPtr members are invisible to the
 * collector, which is why the two extra arrays existed in the first place.
 *
 * NAMED FTyreSmokePuff RATHER THAN THE ISSUE'S FPuff, and AT FILE SCOPE rather than nested
 * in UTyreSmoke: UHT does not reflect a USTRUCT declared inside a UCLASS body - the same
 * constraint EntityDefinition.h's own comment records for ERoadKind and EPlaceableEntity,
 * one level up from an enum - and a bare four-letter name at file scope in a UNITY build is
 * exactly the collision this module's own log-category rule warns about, one level up from
 * a macro.
 */
USTRUCT()
struct FTyreSmokePuff
{
	GENERATED_BODY()

	UPROPERTY(Transient) TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> Instance;

	/** Seconds since birth. At or past PuffSeconds the puff is dead and hidden. */
	double Age = 0.0;
	double Radius = 0.0;
	FVector Born = FVector::ZeroVector;
	bool bLive = false;
};

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

	/** The pool's own size, for Airside.Present.TyreSmokePoolStaysRooted. */
	int32 PoolCountForTest() const { return Puffs.Num(); }

	/**
	 * True when every pooled puff still owns its mesh component, for the same test - the GC
	 * rooting issue #192 item 2 exists to prove. FTyreSmokePuff::Mesh is a UPROPERTY now, so a
	 * collection must never null it out from under the pool the way an unreflected pointer
	 * would.
	 */
	bool EveryPuffHasAMeshForTest() const;

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
	/** Index of the puff to use next: the first dead one, else the OLDEST live one. */
	int32 ClaimSlot();

	UPROPERTY(Transient) TObjectPtr<AActor> Owner;
	UPROPERTY(Transient) TObjectPtr<UMaterialInterface> Material;

	/**
	 * The pool, and now the ONLY list of it (issue #192 item 2) - see FTyreSmokePuff's own
	 * comment for why a reflected TArray of a reflected struct is what makes the two
	 * component arrays this replaced unnecessary rather than merely redundant.
	 */
	UPROPERTY(Transient) TArray<FTyreSmokePuff> Puffs;
};
