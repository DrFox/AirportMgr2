#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "PlotModuleKit.generated.h"

class UStaticMesh;

UENUM()
enum class EKitAssembly : uint8
{
	/** Whole meshes, one per bay count. Blender bakes them. */
	Baked,
	/** Cap + Bay x N + Cap, assembled at runtime. DESIGNED, NOT BUILT. */
	Parts
};

/**
 * One kind of thing that stands in a plot: how big it is, how many of it a plot reserves,
 * and what it looks like.
 *
 * THE OTHER HALF OF A PLOT, and the sibling of UAircraftType. A plot says what ground it
 * has; a kit says what occupies a piece of it. Baking those figures into DepotKit.cpp's
 * switch was right while they were grey boxes and stops being right the moment a mesh
 * exists, because the switch and the mesh are then two statements of one dimension - and
 * they would disagree the first time a model was rescaled, which is the failure this
 * project names most often.
 *
 * HAND-AUTHORED, with provenance in the comment, exactly as UAircraftType is. A manifest
 * emitted by the Blender build script was considered and rejected: it is a mechanism this
 * project uses nowhere, and what it would buy is bought by the test that walks the registry
 * comparing a kit's footprint against its mesh's bounds.
 */
UCLASS(BlueprintType)
class AIRSIDE_API UPlotModuleKit : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Shown in the build menu and the inspector. */
	UPROPERTY(EditAnywhere, Category = "Kit") FText DisplayName;

	/**
	 * Plan extent of ONE module, uu. X is along the module's own +X, which faces AWAY from
	 * the road - see UEntityDefinition::BuildFuelDepot, whose comment records that this was
	 * once stated the wrong way round.
	 *
	 * ONE MODULE'S, NEVER A RUN'S. PlotYard::Reserve multiplies it by the run length itself,
	 * so that multiplication happens in exactly one place.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") FVector2D Footprint = FVector2D::ZeroVector;

	/** Grey-box height, uu. Retired once BakedMeshes is set. */
	UPROPERTY(EditAnywhere, Category = "Kit") double HeightUu = 0.0;

	/**
	 * Stood against the plot's BACK boundary rather than sampled into the yard.
	 *
	 * POSITION ONLY - it says where the thing stands and nothing about which way it points,
	 * deliberately. PlotYard::FFootprint carries the same field with the same warning, and
	 * it was called bFrontsTheGate once, which reads as either.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") bool bAgainstTheBackFence = false;

	/**
	 * How often this kit comes up in the reservation round-robin.
	 *
	 * IT SETS A CEILING, NOT THE PLAYER'S STRATEGY. They buy in whatever order they like up
	 * to the ceiling, so a generous weight costs nothing and a mean one silently forbids a
	 * build the player wanted - a storage-first player capped at two tanks with four shed
	 * slots standing empty.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") int32 ReserveWeight = 1;

	/**
	 * Modules of this kit grouped into one stand at one heading. 1 = never grouped.
	 *
	 * RESERVED AT FULL WIDTH UP FRONT, never grown as the player buys bays: a run that grew
	 * would need ground it was never promised, and every ghosted slot is exactly that
	 * promise.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") int32 RunCap = 1;

	UPROPERTY(EditAnywhere, Category = "Kit") EKitAssembly Assembly = EKitAssembly::Baked;

	/**
	 * Baked only. Index = bay count - 1, so exactly RunCap entries and none null.
	 *
	 * EMPTY IS LEGAL and means grey box. That is what lets the runtime half of this work
	 * land and be tested before any mesh exists.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit")
	TArray<TSoftObjectPtr<UStaticMesh>> BakedMeshes;

	/** Parts only. UNUSED while EKitAssembly::Parts is unimplemented. */
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") TSoftObjectPtr<UStaticMesh> PartCapMesh;
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") TSoftObjectPtr<UStaticMesh> PartBayMesh;
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") double PartPitchUu = 0.0;
};
