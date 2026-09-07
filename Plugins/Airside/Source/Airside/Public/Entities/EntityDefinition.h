#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Entities/AircraftType.h"
#include "Model/RoadEntity.h"
#include "EntityDefinition.generated.h"

class URoadNetwork;

/**
 * Shared, immutable description of a kind of installation (Flyweight), matching
 * URoadProfile's role for cross-sections.
 *
 * A stand, a hangar, a de-icing pad and a cargo terminal differ only by their anchors and
 * their visuals. Adding a new kind is a new data asset, not new code - which is the whole
 * reason anchors are a general mechanism rather than fields on a stand.
 */
UCLASS(BlueprintType)
class AIRSIDE_API UEntityDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * Things dug into the concrete: a hydrant pit, fixed ground power, a PCA point, the
	 * painted boxes equipment stages in.
	 *
	 * GROUND-FIXED, and that is the whole distinction. Where a service is REQUIRED belongs
	 * to the aircraft - an A320 and a 737-800 share this stand and put their hold doors
	 * metres apart - so an anchor here is a place on the apron, never a place on an
	 * airframe. See UAircraftType.
	 *
	 * These resolve to guideline nodes when the stand is placed, because they are permanent.
	 * An aircraft's service points do not: they exist only while an aircraft occupies the
	 * stand, and will be resolved then.
	 */
	UPROPERTY(EditAnywhere) TArray<FEntityAnchor> Anchors;

	/**
	 * What the ground here can provide at all, whether from fixed plant or from equipment
	 * that drives up.
	 *
	 * A stand without Fuel cannot be refuelled however willing the aircraft is; a stand
	 * with Fuel but no hydrant fixture needs a bowser, which is a different vehicle making
	 * a different journey. Availability and position are separate questions and this is the
	 * first of them.
	 */
	UPROPERTY(EditAnywhere) TArray<EServiceRole> AvailableServices;

	/**
	 * What this installation's OWN pose is for - a stand's aircraft stop mark, a depot's
	 * truck bay.
	 *
	 * NOT AN ANCHOR, and deliberately (see FEntityInstance::PoseNode): an anchor is a
	 * FIXTURE dug into or painted onto the concrete, and there is nothing at a stop mark but
	 * paint. But the pose still has to be REACHED, and by something - so this says which
	 * class of guideline its lead-in may join, through TraversalForRole.
	 *
	 * Aircraft by default, which is what every definition authored before this field existed
	 * meant, and is why the Code C stand needs no edit.
	 */
	UPROPERTY(EditAnywhere) EServiceRole PoseRole = EServiceRole::Aircraft;

	/**
	 * Plan-view HALF-extents of the installation itself, uu, in its own local space.
	 *
	 * FOR THE PLACEMENT PREVIEW, and nothing else this slice. Zero draws nothing but the
	 * pose mark, which is what a STAND wants: a stand's extent is its design aircraft's, and
	 * a second rectangle round it would be a second opinion about how big the thing is.
	 *
	 * A BOX AND NOT FEntityFootprint. That struct is aircraft-shaped - nose, wingspan,
	 * tailplane - and a fuel depot has none of those; filling it in for a building would be
	 * authored numbers nothing could read correctly.
	 */
	UPROPERTY(EditAnywhere) FVector2D FootprintExtent = FVector2D::ZeroVector;

	/**
	 * How many vehicles this installation can have out at once.
	 *
	 * Read only from a definition whose PoseRole is a service role; 0 on a stand, where it
	 * means nothing. SCAFFOLDING, named as such by the fuel-service spec (§0.1): M3's
	 * UJobBoard bids by ETA over a real fleet, and this becomes the fleet's SIZE rather than
	 * a number a service counts its own dispatches against.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0")) int32 Trucks = 0;

	/**
	 * The aircraft this stand is sized for.
	 *
	 * Used to draw how the stand would be used - the envelope, and where that type's
	 * services would fall - before any aircraft exists to occupy it. When aircraft arrive,
	 * occupancy replaces this with whatever is actually parked, and the same composition
	 * gives the real positions.
	 */
	UPROPERTY(EditAnywhere) TObjectPtr<UAircraftType> DesignAircraft;

	/** True when this stand can provide Role at all. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	bool Provides(EServiceRole Role) const { return AvailableServices.Contains(Role); }

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
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildCodeCStand(UEntityDefinition* Definition);

	/**
	 * Fill Definition with the fuel depot layout: a box on a service road, and one truck.
	 *
	 * SCAFFOLDING, and named as such by the fuel-service spec (§0.1). M4 replaces this with a
	 * UBuildingInstance carrying a road anchor node, add-on modules, a fleet and an
	 * inventory. What SURVIVES that replacement is the road connection - a pose whose lead-in
	 * joins a GroundVehicle guideline - which is why that part is a general mechanism here
	 * (PoseRole) and the truck count is a bare number.
	 *
	 * NO ANCHORS, deliberately. The spec's first draft gave the depot a Fuel anchor as well
	 * as a pose; two lead-ins from one small building into one road is a duplicate painted
	 * line, and the pose is the node a truck is actually dispatched from and back to.
	 *
	 * Shared by MakeFuelDepotTransient and the commandlet that authors DA_FuelDepot, so the
	 * tested layout and the shipped one are the same numbers rather than two transcriptions.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildFuelDepot(UEntityDefinition* Definition);

	/** BuildFuelDepot plus a NewObject, for tests and the debug gallery. */
	static UEntityDefinition* MakeFuelDepotTransient();

	/**
	 * True when every anchor carries a non-empty id and no two share one.
	 *
	 * Lookup is by id, so an unnamed or duplicated one makes two anchors indistinguishable
	 * and a query silently returns whichever comes first - which sends the fuel truck to
	 * the belt loader's position and reports success. URoadEditFacade::PlaceStand checks
	 * this and complains loudly rather than refusing, so a half-authored asset is visible
	 * instead of fatal - checked there rather than in URoadNetwork::PlaceEntity because
	 * Model/ cannot call a UEntityDefinition method (see RoadEntity.h's header comment).
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static bool HasUsableAnchorIds(const UEntityDefinition* Definition);

	/**
	 * Bring every live entity's FResolvedAnchor::LocalHeading and ::Role up to date with
	 * its definition's current FEntityAnchor values. Returns how many anchors changed.
	 *
	 * Lives here, not on URoadNetwork, because Entities depends on Model and never the
	 * reverse (see RoadEntity.h) - this is the one place both are known at once. It exists
	 * for two reasons that are really one: FResolvedAnchor's LocalHeading and Role are
	 * placement-time SNAPSHOTS (see that struct's comment), and a snapshot needs a moment
	 * it gets refreshed at. An instance placed and saved before these two fields existed on
	 * the struct loads with the UPROPERTY defaults (LocalHeading 0.0, Role Aircraft) and
	 * nothing else ever corrects it; a definition anyone edits after stands are placed from
	 * it needs the same correction to reach them. Called from
	 * ARoadNetworkActor::PostRegisterAllComponents, before RebuildMesh - so both cases are
	 * caught at the next load or explicit rebuild, which is as well-defined a moment as a
	 * snapshot design gets.
	 */
	static int32 RefreshResolvedAnchors(URoadNetwork& Network);
};
