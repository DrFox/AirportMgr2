#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PlotPresenter.generated.h"

class UInstancedStaticMeshComponent;
class URoadNetwork;

/**
 * The boxes standing in a plotted installation's bays, and the fence round its outline.
 *
 * ITS OWN SUBOBJECT rather than another method on ARoadNetworkActor, which reached 2313
 * lines because every feature entered through the one door, and not on URoadSurfacePresenter
 * either: that owns the road SURFACE - mesh, aprons, ghost - and a depot's sheds are objects
 * standing on the surface rather than part of it. The pad is the surface's, and it is built
 * there; see URoadSurfacePresenter.
 *
 * ONE COMPONENT FOR EVERY MODULE TYPE AND THE FENCE TOO. An instance carries its own
 * transform including scale, so one engine cube dresses a shed, a tank, a pump and a fence
 * panel by scaling differently. That is a GREY BOX and says so: the split into one component
 * per authored mesh IS the art swap, when there are meshes to swap in, and nothing else
 * about this class moves when it happens.
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
	 * then a plot simply draws nothing for the room it has left.
	 */
	void Initialise(UInstancedStaticMeshComponent* InBoxes,
		UInstancedStaticMeshComponent* InGhosts = nullptr);

	/**
	 * Clear and re-add an instance per module standing where the yard solver put it, plus a
	 * fence round every plot's outline.
	 */
	void RebuildFrom(const URoadNetwork& Network);

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
	 * For tests: how many fence bays were skipped to leave a gate.
	 *
	 * A fence with no gate is a depot no truck can leave, and it would look completely
	 * correct from every angle - which is why the gap is counted rather than eyeballed.
	 */
	int32 GetGateGapCount() const { return GateGaps; }

private:
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Boxes;

	/** Reserved-but-unbought slots. Wears the ghost material; see Initialise. */
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> GhostBoxes;

	/**
	 * Every transform added during the last rebuild, in the order it was added.
	 *
	 * BECAUSE THE INSTANCE INDEX IS ABOUT TO STOP MEANING ANYTHING. One component held every
	 * box, so "instance 3" was a fact about the PLOT; a second component for the ghosts - and
	 * later one per authored mesh - makes it a fact about which component happened to take
	 * it. The tests ask about the plot, so the order they rely on lives here rather than in a
	 * component they do not own.
	 */
	TArray<FTransform> Placed;

	/** Counted during the last RebuildFrom. See GetGateGapCount. */
	int32 GateGaps = 0;

	/** Counted during the last RebuildFrom. See GetRoomForMore. */
	int32 RoomForMore = 0;

	/** Counted during the last RebuildFrom. See GetModuleCount. */
	int32 ModuleBoxes = 0;

	/** Counted during the last RebuildFrom. See GetDroppedCount. */
	int32 Dropped = 0;

	/** Counted during the last RebuildFrom. See GetGhostCount. */
	int32 Ghosts = 0;
};
