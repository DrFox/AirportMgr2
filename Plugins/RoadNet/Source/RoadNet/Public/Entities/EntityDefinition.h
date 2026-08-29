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

	/**
	 * A contact stand for tests and the debug gallery: the aircraft stop position, plus
	 * the service positions that surround it.
	 *
	 * Local space has the aircraft nose pointing along +X, so the stop position is at the
	 * origin and the servicing vehicles stand off to either side and behind.
	 */
	static UEntityDefinition* MakeStandTransient();
};
