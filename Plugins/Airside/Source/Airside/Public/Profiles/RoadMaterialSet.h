#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RoadMaterialSet.generated.h"

class UMaterialInterface;

/** One named surface. The name is what a profile band asks for; the material is what it gets. */
USTRUCT(BlueprintType)
struct AIRSIDE_API FRoadMaterialSlot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FName Name;

	UPROPERTY(EditAnywhere) TObjectPtr<UMaterialInterface> Material = nullptr;
};

/**
 * The name -> material binding for a road network. Flyweight, like URoadProfile: shared,
 * immutable at run time, and referenced rather than copied.
 *
 * A DataAsset rather than a TMap on ARoadNetworkActor because the level is deliberately
 * never saved, so a table edited on a level-resident instance would be gone every session.
 * An asset is on disk.
 *
 * SLOTS IS AN ARRAY, NOT A MAP, AND THE ORDER IS LOAD-BEARING. The index of a slot is the
 * material id written into the mesh and the index handed to
 * UDynamicMeshComponent::ConfigureMaterialSet, which has no slot names and matches purely by
 * position. TMap iteration order is neither stable across rebuilds nor meaningful, so a map
 * would re-skin every existing surface the first time the set was edited.
 */
UCLASS(BlueprintType)
class AIRSIDE_API URoadMaterialSet : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere) TArray<FRoadMaterialSlot> Slots;

	/** The material id for a slot name, or INDEX_NONE when this set does not declare it. */
	int32 IndexOf(FName Slot) const;

	/**
	 * The material array to hand ConfigureMaterialSet: always exactly Slots.Num() entries,
	 * in slot order, with a null slot filled by the engine's default surface material.
	 *
	 * Never short, and never sparse. FDynamicMeshSceneProxy drops any triangle whose id is
	 * >= NumMaterials into no render buffer at all - the triangle simply vanishes, with
	 * nothing logged - so a material array shorter than the ids in the mesh punches
	 * invisible holes in the road.
	 */
	void ResolveMaterials(TArray<UMaterialInterface*>& Out) const;

	/** Slots with no materials, for tests and headless construction. */
	static URoadMaterialSet* MakeTransient(const TArray<FName>& Names);
};
