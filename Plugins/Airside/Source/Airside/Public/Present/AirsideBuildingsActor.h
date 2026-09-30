#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/RoadHandles.h"
#include "AirsideBuildingsActor.generated.h"

class ARoadNetworkActor;
class UDynamicMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UPlotPresenter;
class URoadNetwork;
// OPAQUE, NOT INCLUDED: Tool/RoadEditTarget.h pulls in a dozen Model/ headers for one enum two private
// handlers take. A scoped enum with its underlying type is complete from this line alone.
enum class EChangeKind : uint8;

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
 * LISTENS, NEVER POLLS: ARoadNetworkActor::OnNetworkChanged (its Topology and Facts kinds; it was
 * OnTopologyRebuilt until #446) is the only thing that makes it draw, plus one catch-up rebuild on
 * binding. WHICH road network it listens to is URoadNetworkRegistry's answer, heard as it changes.
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

	/**
	 * The ghost gate, both halves in one call (facility-upgrades spec R10): bVisible is edit mode OR a
	 * revealed depot; Only is that depot (unset = every yard). A change of Only rebuilds the plots once -
	 * instances are made in RebuildFrom - and a frame with no change costs two compares, so a driver may
	 * call this every frame. The reveal draws the one depot's ghosts and follows the selection.
	 * ENFORCED BY: Airside.Present.PlotPresenter.RevealDrawsOneDepotsGhosts,
	 * AirportMgr.Actions.RevealedDepotFollowsTheSelection
	 * CALL SITES AS OF 2026-09-30: ARoadBuildController::PlayerTick is the sole caller; the editor tool
	 * still calls UPlotPresenter::SetGhostsVisible alone, so its scope stays every yard.
	 */
	void ShowPlotGhosts(bool bVisible, FEntityInstanceId Only);

	/**
	 * For tests: the fence's components. Same ...ForTest precedent as
	 * UPlotPresenter::GetInstanceTransformForTest - widening them would open them to everything.
	 */
	UHierarchicalInstancedStaticMeshComponent* GetFencePostsForTest() const { return FencePosts; }
	UHierarchicalInstancedStaticMeshComponent* GetFenceHeavyPostsForTest() const { return FenceHeavyPosts; }
	UDynamicMeshComponent* GetFenceFabricForTest() const { return FenceFabric; }

	virtual void PostInitProperties() override;
	virtual void PostRegisterAllComponents() override;
	virtual void UnregisterAllComponents(bool bForReregister = false) override;

	/**
	 * The road network to draw for. EMPTY MEANS "THE ONE IN THE WORLD", which is every level
	 * this game has; set it only in a level that holds more than one.
	 *
	 * "THE ONE IN THE WORLD" IS URoadNetworkRegistry's (#446), not a search of this actor's own:
	 * it was a scan here that drew nothing on zero or several candidates - never a guess between
	 * two - while ops and the driver each took the first they found. The registry holds that rule
	 * for all of them now: a second network actor is refused there with an Error, so there is
	 * never a second candidate to guess between.
	 */
	UPROPERTY(EditInstanceOnly, Category = "Airside")
	TObjectPtr<ARoadNetworkActor> RoadNetwork;

private:
	/** OnNetworkChanged's handler (#446): Topology and Facts redraw the plots, the other kinds move none. */
	void OnNetworkChanged(EChangeKind Kind, const URoadNetwork& Network);

	/** URoadNetworkRegistry::OnAirportChanged's handler: bind to this world's airport as it arrives or
	 *  leaves, while RoadNetwork is unset. */
	void OnAirportChanged(UWorld& World, ARoadNetworkActor* Airport);

	/** The registry binding - see PostRegisterAllComponents. Removed on a real unregister. */
	FDelegateHandle RegistryHandle;

	/** The redraw itself - OnNetworkChanged's, and BindTo's catch-up. */
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
	 * The chainlink line posts - HIERARCHICAL, unlike the module boxes, because a perimeter is
	 * hundreds of identical posts and the hierarchy is what culls the ones off screen.
	 */
	UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FencePosts;

	/** Corner and gate posts - the heavier mesh. */
	UPROPERTY() TObjectPtr<UHierarchicalInstancedStaticMeshComponent> FenceHeavyPosts;

	/**
	 * Every plot's fabric, one strip. Distance-field lighting OFF: Lumen's distance fields
	 * ignore opacity masks, so a fence left on would be a solid wall in the field and drop a
	 * black box over the plot (asset README). A dynamic mesh builds no distance field today;
	 * the flag is set so a later switch to a static mesh cannot quietly bring the box back.
	 */
	UPROPERTY() TObjectPtr<UDynamicMeshComponent> FenceFabric;

	/**
	 * WEAK, because the road network can be destroyed first - a level unload tears actors
	 * down in no promised order - and a strong pointer here would be a dangling one then.
	 */
	TWeakObjectPtr<ARoadNetworkActor> Bound;

	/** The OnNetworkChanged binding on Bound; invalid when unbound. */
	FDelegateHandle BoundHandle;
};
