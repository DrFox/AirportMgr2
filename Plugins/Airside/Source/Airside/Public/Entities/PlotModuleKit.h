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
	/**
	 * Cap + Bay x N + Cap turned half a turn, assembled at runtime - BUILT 2026-09-22 for the shed.
	 *
	 * THE DESIGN DOC CHOSE BAKED and the shed arrived modular: Blender ships one bay and one
	 * end, spec'd so N bays butt with no visible seam. Baking 1-, 2- and 3-bay variants would
	 * be three meshes restating one bay, and a partly-bought run is drawn as TWO pieces
	 * (built + ghost) that a baked mesh can only express with a cap at the seam.
	 */
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

	/**
	 * Clear ground this module needs BEYOND its own footprint, uu. X reaches towards the
	 * gate - the apron a truck stands on - and Y is kept clear to either side.
	 *
	 * SEPARATE FROM Footprint, WHICH STAYS THE OBJECT. Folding the apron in would make a
	 * shed 4 x 10 m: the grey box would draw ten metres deep today, and when the real
	 * six-metre mesh arrives the footprint-versus-bounds test would have to be told to
	 * ignore the difference - which is the test giving up on the thing it exists for.
	 *
	 * ZERO IS THE DEFAULT AND MEANS NONE. Every module already gets PlotYard::ClearanceUu
	 * between it and its neighbours; an apron is for a module that needs a vehicle to stand
	 * in front of it, and most do not.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") FVector2D ApronUu = FVector2D::ZeroVector;

	/** Grey-box height, uu. Unused by a kit that has meshes. */
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
	 * Baked only. Index = bay count - 1, so exactly RunCap entries and none null. Each is
	 * centred on its footprint by its own bounds, so its origin is not a contract.
	 *
	 * EMPTY IS LEGAL and means grey box. That is what lets the runtime half of this work
	 * land and be tested before any mesh exists.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit")
	TArray<TSoftObjectPtr<UStaticMesh>> BakedMeshes;

	/**
	 * Parts only. The end of a run, authored for the NEAR end and turned half a turn for the
	 * far one - never mirrored, since a mirrored instance of a single-sheet two-sided wall
	 * drew black (2026-09-22). So it must be SYMMETRIC FRONT TO BACK, as
	 * FuelDepot1/shed/SPEC.md authors it.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") TSoftObjectPtr<UStaticMesh> PartCapMesh;

	/**
	 * Parts only. One bay, repeated along the run.
	 *
	 * ITS PITCH IS Footprint.Y, NOT A FIELD OF ITS OWN. There was a PartPitchUu beside it
	 * until 2026-09-22, when the parts path was built and it became a second statement of the
	 * one width the solver reserves per module - removed rather than kept in step.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") TSoftObjectPtr<UStaticMesh> PartBayMesh;

	/**
	 * Parts only. How far ONE cap reaches along the run beyond the bays, uu - roof overhang
	 * included, because the overhang is what would touch a neighbour.
	 *
	 * RESERVED, NOT JUST DRAWN: PlotYard::FKitSpec::RunWidthUu adds it at both ends, so a
	 * three-bay run claims its caps as ground. Left out, two sheds side by side would each
	 * push their gable into the ClearanceUu between them.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit|Parts") double PartCapUu = 0.0;

	/**
	 * Yaw from the MESH's own frame to the kit's, degrees: +X away from the road, the run
	 * along +Y. A MULTIPLE OF 90, so the mesh's bounds stay a box in the kit's frame.
	 *
	 * HERE RATHER THAN BAKED INTO THE IMPORT: the models repo states its own axes (the shed's
	 * openings face -Y and bays tile along +X), and restating them in an import option would
	 * be a transform nobody can see on the asset. Measured, not derived - see
	 * AirportMgr.Content.DepotKitMeshesMatchTheirFootprints.
	 */
	UPROPERTY(EditAnywhere, Category = "Kit") double MeshYawDeg = 0.0;
};
