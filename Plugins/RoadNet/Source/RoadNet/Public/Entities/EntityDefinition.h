#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
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
UCLASS()
class ROADNET_API UEntityDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere) TArray<FEntityAnchor> Anchors;

	/** Plan extent, for the overlay to draw the thing the anchors surround. */
	UPROPERTY(EditAnywhere) FEntityFootprint Footprint;

	/**
	 * The footprint as plan-view line segments in LOCAL space: even indices start a
	 * segment, odd indices end it.
	 *
	 * One implementation, called by both the overlay and the placement preview, so a stand
	 * being aimed and a stand already placed cannot be drawn to different shapes.
	 */
	static void BuildFootprintLines(const FEntityFootprint& Footprint, TArray<FVector2D>& OutSegments);

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
