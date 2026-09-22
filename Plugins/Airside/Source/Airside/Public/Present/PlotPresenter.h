#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Content/DepotModuleLook.h"
#include "Content/FenceKit.h"
#include "Solve/PlotYard.h"
#include "PlotPresenter.generated.h"

class UDynamicMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;
class URoadNetwork;
class USceneComponent;
class UStaticMesh;

/**
 * Where the fence is drawn. Bundled because it travels together - see CLAUDE.md "one struct
 * per thing" - and any member may be null, which draws that part of the fence nowhere.
 */
struct FFenceTargets
{
	UHierarchicalInstancedStaticMeshComponent* Posts = nullptr;
	UHierarchicalInstancedStaticMeshComponent* HeavyPosts = nullptr;
	UDynamicMeshComponent* Fabric = nullptr;
};

/**
 * The boxes standing in a plotted installation's bays, and the fence round its outline.
 *
 * ITS OWN SUBOBJECT rather than another method on ARoadNetworkActor, which reached 2313
 * lines because every feature entered through the one door, and not on URoadSurfacePresenter
 * either: that owns the road SURFACE - mesh, aprons, ghost - and a depot's sheds are objects
 * standing on the surface rather than part of it. The pad is the surface's, and it is built
 * there; see URoadSurfacePresenter.
 *
 * ONE COMPONENT PER AUTHORED MESH, since 2026-09-22, plus the grey-box cube for a module
 * whose kit has none. An instance carries a transform and not a mesh, so the shed's cap, the
 * shed's bay and the tank each need their own; they are created on first use under the mesh
 * parent the owner hands in, and pooled by mesh. The fence has its own components (posts
 * are two authored meshes and the fabric is a strip, not a box).
 */
UCLASS()
class AIRSIDE_API UPlotPresenter : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * The components to fill. The actor owns them, as it owns every other draw target.
	 *
	 * TWO OF THEM, because an instance carries a transform and not a material: a ghosted slot
	 * cannot differ from a built one inside a single component. InGhosts may be null, and
	 * then a plot simply draws nothing for the room it has left. InFence names the fence's
	 * three components; see FFenceTargets.
	 *
	 * InMeshParent is where per-mesh components are attached when a kit has meshes. NULL
	 * DRAWS EVERY MODULE AS A GREY BOX, meshes or not: a component needs an owning actor, and
	 * a presenter with none has nowhere to put one.
	 */
	void Initialise(UInstancedStaticMeshComponent* InBoxes,
		UInstancedStaticMeshComponent* InGhosts = nullptr,
		const FFenceTargets& InFence = FFenceTargets(),
		USceneComponent* InMeshParent = nullptr);

	/**
	 * Clear and re-add an instance per module standing where the yard solver put it, plus a
	 * fence round every plot's outline.
	 *
	 * THE KITS ARE HANDED IN, not looked up. This used to reach its owning ARoadNetworkActor
	 * for ResolveDepotKits (issue #181), which tied the presenter to whichever actor happened
	 * to be its outer - and the buildings split moved that outer. The caller resolves through
	 * the ONE resolver, ARoadNetworkActor::ResolveDepotKits, so issue #181's second-resolution
	 * drift stays impossible: this class has no way to resolve them itself. (Before #181 it
	 * called DepotKitSpecs(UAirsideSettings::GetContent()) directly - a second resolution of
	 * the table FPlotPlaceTool's ghost reads through IRoadEditTarget, and the two drifted.)
	 *
	 * Kit is the fence's meshes and material, resolved by the caller for the same one-resolver
	 * reason as the specs; a null member falls back to grey box.
	 *
	 * Looks are the modules' meshes, indexed like Specs (UAirsideSettings::ResolveDepotLooks).
	 * EMPTY, OR A LOOK WITH NO MESHES, IS THE GREY BOX - the path every test with no content
	 * set exercises, and a pump still takes today.
	 */
	void RebuildFrom(const URoadNetwork& Network, TArrayView<const PlotYard::FKitSpec> Specs,
		const FFenceKit& Kit = FFenceKit(), TArrayView<const FDepotModuleLook> Looks = {});

	/**
	 * Empty every component and zero every count, as if rebuilt from an airport with no plots.
	 *
	 * FOR AN OWNER WITH NOTHING TO DRAW FROM - a buildings actor whose road network is gone.
	 * RebuildFrom starts with exactly this, so the two cannot disagree about what "empty" is.
	 */
	void Clear();

	/** For tests: how many boxes are standing. */
	int32 GetInstanceCount() const;

	/**
	 * How many more modules would still fit across every plot - the room the player has to
	 * grow into. For tests, and for the census line.
	 *
	 * ANSWERED BY THE SOLVER THAT PLACES THINGS, not by arithmetic on a bay grid: the number
	 * the player reads is produced by the code that would actually put the module down, so
	 * it cannot drift from what they get.
	 */
	int32 GetRoomForMore() const { return RoomForMore; }

	/**
	 * Module BAYS built across every plot - what the player owns and can see.
	 *
	 * BAYS, NOT INSTANCES, and the two parted company when runs arrived: a three-bay shed run
	 * is three modules drawn as one box. This answers "how much depot is there", which is the
	 * question every caller was really asking even while the two numbers agreed.
	 */
	int32 GetModuleCount() const { return ModuleBoxes; }

	/**
	 * Modules that had nowhere to stand. Reported, never hidden.
	 *
	 * ZERO NOW, AND KEPT ANYWAY. Reserve returns only what it placed, so a drop is a bug
	 * rather than a refusal - and an invariant with no accessor is an invariant nobody can
	 * assert. See Airside.Present.PlotPresenterGhostsUnboughtSlots.
	 */
	int32 GetDroppedCount() const { return Dropped; }

	/**
	 * Reserved bays nobody has bought yet, drawn ghosted.
	 *
	 * NOT A GRID OF SLOT MARKERS. Those were removed on 2026-09-16 because a uniform grid
	 * claimed a structure the scattered yard did not have. A ghost here is a SOLVED STAND -
	 * its own footprint, its own sampled heading, from the code path that will place the
	 * module when it is bought. It does not claim the yard has a structure; it shows the yard.
	 */
	int32 GetGhostCount() const { return Ghosts; }

	/**
	 * For tests: one instance's transform, false if there is no such instance.
	 *
	 * The component itself is private on ARoadNetworkActor, and widening it so a test can
	 * read one transform would open it to everything else too. Same ...ForTest precedent as
	 * GetHudForTest and SessionForTest.
	 */
	bool GetInstanceTransformForTest(int32 Index, FTransform& OutTransform) const;

	/**
	 * For tests: how many plots got a gate. One per plot, or the census names the one that did
	 * not - see FenceLayout::FLayout::bHasGate.
	 *
	 * A fence with no gate is a depot no truck can leave, and it would look completely correct
	 * from every angle - which is why the gate is counted rather than eyeballed. It counted
	 * SKIPPED BAYS until 2026-09-22, when the gate stopped being "the bay nearest the pose" and
	 * became an opening of exactly PlotYard::GateCorridorUu.
	 */
	int32 GetGateGapCount() const { return Gates; }

	/**
	 * For tests: instances of one authored mesh drawn in the last rebuild, built or ghosted.
	 * Zero for a mesh no component was ever made for.
	 */
	int32 GetMeshInstanceCountForTest(const UStaticMesh* Mesh, bool bGhost) const;

	/** For tests: the pooled component drawing Mesh, or null if none was made. */
	const UInstancedStaticMeshComponent* GetMeshComponentForTest(const UStaticMesh* Mesh, bool bGhost) const;

	/** For tests: one of those instances' world transform, false if there is no such one. */
	bool GetMeshInstanceTransformForTest(const UStaticMesh* Mesh, bool bGhost, int32 Index,
		FTransform& OutTransform) const;

	/** For tests: fence posts of every kind drawn in the last rebuild. */
	int32 GetFencePostCount() const { return FencePosts; }

	/** For tests: fabric bays drawn in the last rebuild. */
	int32 GetFenceSpanCount() const { return FenceSpans; }

private:
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Boxes;

	/** Reserved-but-unbought slots. Wears the ghost material; see Initialise. */
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> GhostBoxes;

	/** Where per-mesh components are attached; null draws grey boxes only. See Initialise. */
	UPROPERTY(Transient) TObjectPtr<USceneComponent> MeshParent;

	/**
	 * One component per authored mesh, built modules and ghosts apart - an instance carries a
	 * transform and not a material, the reason GhostBoxes exists.
	 *
	 * UPROPERTY SO THE COLLECTOR SEES THEM, Transient so they are never saved into the level:
	 * a component saved there would come back on load holding last session's instances, the
	 * orphan a removed default subobject left behind once (memory: "removed default subobject
	 * still renders"). They are re-made on the first rebuild after a load.
	 */
	UPROPERTY(Transient) TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>> MeshPool;
	UPROPERTY(Transient) TMap<TObjectPtr<UStaticMesh>, TObjectPtr<UInstancedStaticMeshComponent>> GhostMeshPool;

	/** The pooled component drawing Mesh, made on first use; null with no MeshParent. */
	UInstancedStaticMeshComponent* PoolFor(UStaticMesh* Mesh, bool bGhost);

	/**
	 * Draws one stand's modules from their meshes - see RebuildFrom's loop, which computed the
	 * built and ghosted spans this lays pieces along. Returns false when there is nothing to
	 * draw them with, and the caller draws boxes instead.
	 */
	bool DrawMeshes(const FDepotModuleLook& Look, const PlotYard::FKitSpec& Spec,
		const FVector2D& RunCentre, double Heading, int32 Lit, int32 Dark);

	/**
	 * Every MODULE transform added during the last rebuild, in the order it was added - the
	 * fence is not in it since 2026-09-22, when it moved to its own components.
	 *
	 * BECAUSE THE INSTANCE INDEX IS ABOUT TO STOP MEANING ANYTHING. One component held every
	 * box, so "instance 3" was a fact about the PLOT; a second component for the ghosts - and
	 * later one per authored mesh - makes it a fact about which component happened to take
	 * it. The tests ask about the plot, so the order they rely on lives here rather than in a
	 * component they do not own.
	 */
	TArray<FTransform> Placed;

	/** Counted during the last RebuildFrom. See GetGateGapCount. */
	int32 Gates = 0;

	/** Counted during the last RebuildFrom. See GetFencePostCount. */
	int32 FencePosts = 0;

	/** Counted during the last RebuildFrom. See GetFenceSpanCount. */
	int32 FenceSpans = 0;

	/** See FFenceTargets. UPROPERTY for the reason Boxes is one. */
	UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FencePostsInto;
	UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FenceHeavyPostsInto;
	UPROPERTY(Transient) TObjectPtr<UDynamicMeshComponent> FenceFabricInto;

	/** Counted during the last RebuildFrom. See GetRoomForMore. */
	int32 RoomForMore = 0;

	/** Counted during the last RebuildFrom. See GetModuleCount. */
	int32 ModuleBoxes = 0;

	/** Counted during the last RebuildFrom. See GetDroppedCount. */
	int32 Dropped = 0;

	/** Counted during the last RebuildFrom. See GetGhostCount. */
	int32 Ghosts = 0;
};
