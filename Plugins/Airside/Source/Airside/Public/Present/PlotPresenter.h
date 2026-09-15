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
	/** The component to fill. The actor owns it, as it owns every other draw target. */
	void Initialise(UInstancedStaticMeshComponent* InBoxes);

	/** Clear and re-add an instance per module, plus a fence round every plot's outline. */
	void RebuildFrom(const URoadNetwork& Network);

	/** For tests: how many boxes are standing. */
	int32 GetInstanceCount() const;

	/**
	 * For tests: how many fence bays were skipped to leave a gate.
	 *
	 * A fence with no gate is a depot no truck can leave, and it would look completely
	 * correct from every angle - which is why the gap is counted rather than eyeballed.
	 */
	int32 GetGateGapCount() const { return GateGaps; }

private:
	UPROPERTY(Transient) TObjectPtr<UInstancedStaticMeshComponent> Boxes;

	/** Counted during the last RebuildFrom. See GetGateGapCount. */
	int32 GateGaps = 0;
};
