#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AirsideBuildingsActor.generated.h"

class ARoadNetworkActor;
class UInstancedStaticMeshComponent;
class UPlotPresenter;
class URoadNetwork;

/**
 * Everything that stands ON the airport rather than being part of its surface: plotted
 * installations' modules today, their fences next.
 *
 * SPLIT OUT OF ARoadNetworkActor on 2026-09-22. That actor is the composition root for the
 * road network and had grown to own the depot presentation too, because every feature
 * entered through the one door. Only PRESENTATION moved: entities are still
 * URoadNetwork::Entities, read by 19 files and snapshotted by the one undo Memento, and
 * moving the model is a separate and much larger decision.
 *
 * ONE ACTOR FOR ALL BUILDINGS, not one per plot. A per-plot actor needs a spawn/destroy index
 * kept in step with the model through undo - the second index UPlotPresenter::RebuildFrom's
 * own comment refused, with tens of plots to rebuild. Revisit when per-building selection
 * needs a click target of its own.
 *
 * LISTENS, NEVER POLLS: ARoadNetworkActor::OnTopologyRebuilt is the only thing that makes it
 * draw, plus one catch-up rebuild on binding.
 */
UCLASS()
class AIRSIDE_API AAirsideBuildingsActor : public AActor
{
	GENERATED_BODY()

public:
	AAirsideBuildingsActor();

	/** The first buildings actor in a world, or nullptr. */
	static AAirsideBuildingsActor* Find(const UWorld* World);

	/**
	 * The world's buildings actor bound to Road, spawned if there is none.
	 *
	 * EVERY SITE THAT CREATES A ROAD NETWORK CALLS THIS BESIDE IT - the editor mode, the editor
	 * tool and the PIE controller - because a level with no buildings actor draws no depots,
	 * and nothing on screen would say why. Not transient, for ARoadNetworkActor::FindOrCreate's
	 * reason: this is the one saved with the level.
	 */
	static AAirsideBuildingsActor* FindOrCreate(UWorld* World, ARoadNetworkActor* Road);

	/**
	 * Listen to Road, dropping any previous binding, and rebuild once to catch up.
	 *
	 * THE CATCH-UP IS NOT OPTIONAL. Actors register in no promised order, so the road
	 * network's own first rebuild (its PostRegisterAllComponents) may already have fired
	 * before this bound - and without it a loaded level's depots stay invisible until the
	 * player next edits a road. Null unbinds and clears.
	 */
	void BindTo(ARoadNetworkActor* Road);

	/** The road network currently bound, or nullptr. */
	ARoadNetworkActor* GetRoadNetwork() const { return Bound.Get(); }

	/** The plot boxes - see UPlotPresenter. */
	UPlotPresenter* GetPlotPresenter() const { return Plots; }

	virtual void PostInitProperties() override;
	virtual void PostRegisterAllComponents() override;
	virtual void UnregisterAllComponents(bool bForReregister = false) override;

	/**
	 * The road network to draw for. EMPTY MEANS "THE ONE IN THE WORLD", which is every level
	 * this game has; set it only in a level that holds more than one.
	 *
	 * A SEARCH IS ACCEPTABLE HERE where ASunDriver refuses one, because this one cannot pick
	 * wrong silently: zero or several candidates is a Warning naming the count, and nothing is
	 * drawn - never a guess between two.
	 */
	UPROPERTY(EditInstanceOnly, Category = "Airside")
	TObjectPtr<ARoadNetworkActor> RoadNetwork;

private:
	/** OnTopologyRebuilt's handler. */
	void Rebuild(const URoadNetwork& Network);

	/** Remove the delegate binding, if any. Safe to call when unbound. */
	void Unbind();

	/** Same CreateDefaultSubobject and Transient reasoning as ARoadNetworkActor::Presenter. */
	UPROPERTY(Transient) TObjectPtr<UPlotPresenter> Plots;

	/**
	 * The one component every built module box is an instance in. A UPROPERTY and NOT
	 * Transient, unlike the presenter that fills it: it is a scene component this actor owns.
	 * Moved from ARoadNetworkActor::PlotBoxes.
	 */
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ModuleBoxes;

	/**
	 * The same again for reserved bays nobody has bought, wearing the ghost material.
	 * A SECOND COMPONENT because an instance carries a transform and not a material, so a
	 * ghosted slot cannot differ from a built one inside ModuleBoxes. Moved from
	 * ARoadNetworkActor::PlotGhostBoxes.
	 */
	UPROPERTY() TObjectPtr<UInstancedStaticMeshComponent> ModuleGhosts;

	/**
	 * WEAK, because the road network can be destroyed first - a level unload tears actors
	 * down in no promised order - and a strong pointer here would be a dangling one then.
	 */
	TWeakObjectPtr<ARoadNetworkActor> Bound;

	/** The OnTopologyRebuilt binding on Bound; invalid when unbound. */
	FDelegateHandle BoundHandle;
};
