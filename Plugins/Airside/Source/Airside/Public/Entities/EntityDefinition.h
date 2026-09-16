#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Entities/AircraftType.h"
#include "Model/RoadEntity.h"
#include "EntityDefinition.generated.h"

class URoadNetwork;

/**
 * Which authored installation a placement gesture drops.
 *
 * A KIND, NOT A UEntityDefinition*, wherever a tool is involved: a tool has no business
 * naming an asset, and resolving which definition a kind MEANS is the facade's job
 * (ARoadNetworkActor::ResolveEntityDefinition) - in one place, so the PREVIEW and the
 * PLACEMENT cannot resolve different objects. That is exactly what
 * IRoadEditTarget::GetStandDefinition's own comment has always warned about, and it stops
 * being hypothetical the moment there are two kinds.
 *
 * A UENUM in a UHT-parsed header rather than a plain enum on the tool seam, for the reason
 * ERoadKind records at its own declaration: Tool/RoadEditTarget.h has no .generated.h, so
 * UHT cannot resolve a type declared there when it appears in ARoadNetworkActor's members,
 * and a forward declaration does not satisfy it either.
 */
UENUM()
enum class EPlaceableEntity : uint8
{
	Stand,
	FuelDepot
};

/**
 * What a lane waypoint IS, so a builder never has to infer it from position.
 *
 * A UENUM rather than a plain enum for the reason EPlaceableEntity records above: UHT cannot
 * resolve a plain enum named by a USTRUCT's UPROPERTY, and a forward declaration does not
 * satisfy it either.
 *
 * BlueprintType, like EServiceRole beside it, because Tools/Python/build_stand_asset.py reads
 * this back off the authored asset - and a listing of positions with no kind beside them would
 * read as correct on a lane whose anchors had come unstuck from it.
 */
UENUM(BlueprintType)
enum class EStandWaypointKind : uint8
{
	/** Shape only - a corner, or the flat a dip needs so the pit is driven through. */
	Plain,
	/** An anchor sits here. AnchorId names it, and the lane reuses that anchor's node. */
	Anchor,
	/** Where a road may join. The heading is the lane's own direction here. */
	Entry,
};

/**
 * One point of a stand's service lane, in the entity's own local space.
 *
 * NO HEADING FIELD, deliberately. An entry's heading is its lane's direction at that point and
 * an anchor's is already FEntityAnchor::LocalHeading; a copy here would be a value that must
 * agree with another value in the same asset, which is the drift FResolvedAnchor exists to
 * remove.
 *
 * BlueprintType for the same reason FEntityAnchor is: the Python asset author reads the lane
 * back to log it, and that is a read through the reflection system.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FStandWaypoint
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FVector2D Local = FVector2D::ZeroVector;

	/** Set if and only if Kind is Anchor. */
	UPROPERTY(EditAnywhere) FName AnchorId;

	UPROPERTY(EditAnywhere) EStandWaypointKind Kind = EStandWaypointKind::Plain;
};

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
	 * What placing one costs, and what a day of owning it costs.
	 *
	 * See URoadProfile::CostPerMetre for why the figure lives on the asset rather than in a
	 * table beside it, and why a number only AirportOps reads sits in this plugin.
	 */
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double PlacementCost = 0.0;
	UPROPERTY(EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0")) double UpkeepPerDay = 0.0;


	/**
	 * A closed, INVISIBLE vehicle lane threading the anchors, in the entity's own local space.
	 * Empty means none.
	 *
	 * CLOSED IMPLICITLY: the last point joins the first, and the array does NOT repeat it.
	 * Storing the repeat would be a value that must agree with another value in the same
	 * array, which is exactly the drift FResolvedAnchor exists to remove.
	 *
	 * IT RUNS ALONG THE AEROPLANE, NOT AROUND IT, and that is the whole of the 2026-09-16
	 * redesign. The ring this replaces ran outboard of the wingtips and every anchor hung off
	 * it on a spur - which needs a 90 degree turn, and at the 699 uu a real 8.5 m dispenser's
	 * steering lock allows, that is 989.1 uu of run on EACH of its two arms. The two arms meet
	 * on the same straight, so the straight has to carry 1978 uu, and there were 990 between the
	 * ring and the box row. Short by half. Widening the ring made it worse. So the lane comes
	 * inside the wingtip and runs along the row the boxes are already painted on: every service
	 * anchor sits ON it and nothing turns into one.
	 *
	 * ONE CYCLE, because a dead end is a reverse and reverse is a later stage. The hydrant, 400
	 * uu inboard of the row, is a flat-bottomed DIP in the lane rather than a stub - a pure V
	 * there would be a corner no vehicle can take, and under the rolling-steer law an agent
	 * stopped at one cannot turn at all, so it would be stuck rather than slow.
	 *
	 * NOTHING CROSSES THE AEROPLANE STILL, which was the old ring's reason for existing and is
	 * not weakened by bringing the lane inboard: both runs stay on one side of the fuselage
	 * centreline, and the two crossings that join them sit clear ahead of the nose and clear
	 * aft of the tail. Airside.Entities.StandLaneClearsTheAircraft measures it.
	 *
	 * COMPUTED by the builder that lays the anchors, never authored beside them, and sized from
	 * the LARGEST SERVICE VEHICLE ADMITTED rather than the one driving - see BuildCodeCStandFor.
	 * Deriving it at rebuild time was rejected for the reason the ring's own header gave: that
	 * is a runtime algorithm's opinion with no override, and a second evaluator of the same
	 * geometry.
	 *
	 * INVISIBLE, and a decision rather than an omission: no marking builder, no material, no
	 * mesh. It exists only as guideline nodes and edges, and shows in the G overlay because
	 * everything in the graph does. It is a routing lane, not paint.
	 */
	UPROPERTY(EditAnywhere) TArray<FStandWaypoint> ServiceLane;

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
	 * This stand has pavement on BOTH sides: an aeroplane enters nose-first and leaves
	 * nose-first, so it needs no push off it at all.
	 *
	 * IT CHANGES THE GRAPH, NOT THE TRAFFIC MODEL. All it does is make FAnchorLink::Gather
	 * cast a SECOND lead-in ray, forward along +Heading, so the pose node has a way OUT as
	 * well as a way in. Whether a given aeroplane then needs a push is measured off the route
	 * it is actually given - see UGroundTraffic::DepartAgent - which is why nothing in Model/
	 * reads this flag, and why one question also answers a taxiway a player happened to draw
	 * past an ordinary stand.
	 *
	 * ONE LEAD-IN IS STILL THE RULE for every stand that does not set this. A second line into
	 * one nose-stop is normally a defect, and the anchor loop in Gather refuses it by name.
	 */
	UPROPERTY(EditAnywhere) bool bTaxiThrough = false;

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
	 *
	 * TAKES THE DESIGN AIRCRAFT, and sets it. Named Aircraft rather than DesignAircraft
	 * because UHT refuses a UFUNCTION parameter that shadows a UPROPERTY of the same class. Both callers used to set DesignAircraft
	 * afterwards, which was harmless only for as long as nothing in the layout depended on
	 * it - and ServiceLane does: a stand's geometry is laid out ALONG the aircraft it is
	 * sized for, so the builder has to know which one that is. A null aircraft is allowed: the
	 * lane is still laid, and its two crossings are placed off the anchors alone rather than
	 * being pushed clear of a nose and a tail that are not there - which is what a definition
	 * with no envelope to clear actually wants.
	 *
	 * A FORWARDER since 2026-09-16, and it KEEPS THIS NAME AND THIS UFUNCTION because
	 * Tools/Python/build_stand_asset.py calls build_code_c_stand() on it. A UFUNCTION that
	 * moves is a Python script and a Blueprint that stop compiling.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildCodeCStand(UEntityDefinition* Definition, UAircraftType* Aircraft);

	/**
	 * BuildCodeCStand, with the vehicle the ground is sized for made explicit.
	 *
	 * EXISTS FOR THE TEST that proves the derivation is a derivation. Given a longer vehicle
	 * the equipment boxes must move outward; asked of the shipping vehicle alone, that
	 * assertion would pass just as well against a hand-typed figure, which is exactly what
	 * this change removes. BuildCodeCStand forwards with ResolveLargestServiceVehicle().
	 */
	static void BuildCodeCStandFor(
		UEntityDefinition* Definition, UAircraftType* Aircraft, const FAirframe& Largest);

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
