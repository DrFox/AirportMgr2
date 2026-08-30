#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Entities/AircraftType.h"
#include "Model/RoadEntity.h"
#include "EntityDefinition.generated.h"

/**
 * Shared, immutable description of a kind of installation (Flyweight), matching
 * URoadProfile's role for cross-sections.
 *
 * A stand, a hangar, a de-icing pad and a cargo terminal differ only by their anchors and
 * their visuals. Adding a new kind is a new data asset, not new code - which is the whole
 * reason anchors are a general mechanism rather than fields on a stand.
 */
UCLASS(BlueprintType)
class ROADNET_API UEntityDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * Things dug into the concrete: a hydrant pit, fixed ground power, a PCA point, the
	 * painted boxes equipment stages in.
	 *
	 * GROUND-FIXED, and that is the whole distinction. Where a service is REQUIRED belongs
	 * to the aircraft - an A320 and a 737-800 share this stand and put their hold doors
	 * metres apart - so an anchor here is a place on the apron, never a place on an
	 * airframe. See UAircraftType.
	 *
	 * These resolve to guideline nodes when the stand is placed, because they are permanent.
	 * An aircraft's service points do not: they exist only while an aircraft occupies the
	 * stand, and will be resolved then.
	 */
	UPROPERTY(EditAnywhere) TArray<FEntityAnchor> Anchors;

	/**
	 * What the ground here can provide at all, whether from fixed plant or from equipment
	 * that drives up.
	 *
	 * A stand without Fuel cannot be refuelled however willing the aircraft is; a stand
	 * with Fuel but no hydrant fixture needs a bowser, which is a different vehicle making
	 * a different journey. Availability and position are separate questions and this is the
	 * first of them.
	 */
	UPROPERTY(EditAnywhere) TArray<EServiceRole> AvailableServices;

	/**
	 * The aircraft this stand is sized for.
	 *
	 * Used to draw how the stand would be used - the envelope, and where that type's
	 * services would fall - before any aircraft exists to occupy it. When aircraft arrive,
	 * occupancy replaces this with whatever is actually parked, and the same composition
	 * gives the real positions.
	 */
	UPROPERTY(EditAnywhere) TObjectPtr<UAircraftType> DesignAircraft;

	/** True when this stand can provide Role at all. */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	bool Provides(EServiceRole Role) const { return AvailableServices.Contains(Role); }

	/**
	 * A contact stand for tests and the debug gallery: the aircraft stop position, plus
	 * the service positions that surround it.
	 *
	 * Local space has the aircraft nose pointing along +X, so the stop position is at the
	 * origin and the servicing vehicles stand off to either side and behind.
	 */
	static UEntityDefinition* MakeStandTransient();

	/**
	 * Fill Definition with the Code C contact stand layout, replacing whatever it held.
	 *
	 * Shared by MakeStandTransient and the commandlet that authors the DA_Stand_CodeC data
	 * asset, so the tested layout and the shipped one are the same numbers rather than two
	 * transcriptions of them.
	 */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	static void BuildCodeCStand(UEntityDefinition* Definition);

	/**
	 * True when every anchor carries a non-empty id and no two share one.
	 *
	 * Lookup is by id, so an unnamed or duplicated one makes two anchors indistinguishable
	 * and a query silently returns whichever comes first - which sends the fuel truck to
	 * the belt loader's position and reports success. PlaceEntity checks this and complains
	 * loudly rather than refusing, so a half-authored asset is visible instead of fatal.
	 */
	UFUNCTION(BlueprintCallable, Category = "RoadNet")
	static bool HasUsableAnchorIds(const UEntityDefinition* Definition);
};
