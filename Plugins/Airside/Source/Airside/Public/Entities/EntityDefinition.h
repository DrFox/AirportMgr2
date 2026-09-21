#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Entities/AircraftType.h"
#include "Model/RoadEntity.h"
#include "Model/RouteSearch.h"
#include "Solve/IcaoCode.h"
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
 * How a plot's modules are arranged. Build/PlotLayoutStrategy.h holds the strategies.
 *
 * HERE RATHER THAN BESIDE THEM, because a definition owns this field: declaring it in Build/
 * would make Entities/ depend on Build/ to state its own property, and the dependency runs
 * the other way round.
 */
UENUM()
enum class EPlotLayout : uint8
{
	/** Sampled, jittered, unplanned. The default - see PlotLayoutFor. */
	Scatter,
	/** Sheds across the back, tanks left, pumps right. Scaffolding; see the class. */
	FuelYardBands
};

/**
 * One piece of a stand's authored geometry: a chain of quadratic segments.
 *
 * POINTS AND CONTROLS, NOT A POLYLINE, because that is what a guideline edge IS - two
 * endpoints and one control - so laying this into the graph is a copy rather than a
 * re-interpretation. The sampled polyline the oracle judges is DERIVED from these by the same
 * GuidelineGeom::Sample the graph uses, which is the guideline graph's own sample-once rule:
 * a second evaluator lets the line a vehicle drives differ from the one that was verified,
 * visibly and only on bends.
 *
 * EVERY CORNER IS CARRIED WHOLE. A leg is checked ALONE by FSpeedProfile, so a corner that fell
 * between two legs would be judged by nothing - which is the exact defect this piece exists to
 * remove, one level up. Legs therefore meet TANGENTIALLY and each owns its own bends.
 */
USTRUCT()
struct AIRSIDE_API FStandLeg
{
	GENERATED_BODY()

	/**
	 * Segment i runs Points[i] to Points[i+1], bending about Controls[i].
	 *
	 * VisibleAnywhere, LIKE EVERY FIELD OF THE LAYOUT, and the reason is a readback rather
	 * than a UI. A bare UPROPERTY() serialises perfectly well but cannot be reached by
	 * get_editor_property - FindPropertyByName does not see it - so the Python that authors
	 * DA_Stand_CodeC could not print what it had just saved. The layout is invisible in the
	 * editor (no mesh, no material, no marking builder), so that log IS the only way to tell
	 * a stand carrying four bays from one carrying none, and the two look identical in the
	 * content browser. VISIBLE rather than Edit, because all of it is derived: a hand-edited
	 * leg would be overwritten by the next build and undriveable in the meantime.
	 */
	UPROPERTY(VisibleAnywhere) TArray<FVector2D> Points;
	UPROPERTY(VisibleAnywhere) TArray<FVector2D> Controls;

	bool IsSet() const { return Points.Num() >= 2 && Controls.Num() == Points.Num() - 1; }

	/** The chain as one welded polyline - the array the oracle costs and a follower walks. */
	void Sample(TArray<FVector2D>& OutPoints) const;

	/** The same chain as an FRoutePlan, Result Found, for FSpeedProfile and FReverseRun. */
	FRoutePlan ToPlan() const;
};

/**
 * One service vehicle's parking bay, and the four legs of its visit.
 *
 * FOUR LEGS, and the shape is the user's of 2026-09-17 (samples/standPlan.png) rather than
 * anything derived: off the service road into a PARKING BAY, forward to the service point,
 * REVERSE clear of the aeroplane, then forward out. Three earlier designs had the vehicle back
 * INTO its working position; this one drives in forwards and reverses out, which is what the
 * ground actually costs - the expensive manoeuvre lands in open ground at the reverse limit of
 * 494.5 uu instead of beside the aircraft at the forward limit of 699.3.
 *
 * THE PARKING BAY IS NOT THE SERVICE POINT. The vehicle WAITS in the bay; it WORKS at the
 * anchor. Collapsing the two is what produced a layout where every service position had to
 * double as a turn-round, and it is why the staging rank had nowhere to go.
 *
 * ITS OWN ENTRY OFF THE ROAD. Ruled 2026-09-17: a vehicle never threads past a parked one to
 * reach its own bay. The slots are angled so the turn off the road is 45 degrees and costs
 * CornerRunFor(R, 135) = 313 uu of run rather than the 989 a square one would; a vehicle
 * arriving from the other direction has the tight corner and manoeuvres in - which is a SHUNT,
 * forward-reverse-forward, and never a crab.
 *
 * AIRFRAME-INDEPENDENT. A bay is paint on concrete, and paint does not move when a different
 * type parks; where a service connects to the AIRCRAFT lives on UAircraftType, because an A320
 * and a 737-800 park here with their doors metres apart.
 */
USTRUCT()
struct AIRSIDE_API FServiceBay
{
	GENERATED_BODY()

	/** Which service this bay serves. Matches an FEntityAnchor::Id on the same definition. */
	UPROPERTY(VisibleAnywhere) FName AnchorId;

	/**
	 * Where this bay's road entry meets the stand's edge, and pointing which way.
	 *
	 * FIXED, AND IT MUST MEET A ROAD. This is the last per-placement geometry the design had
	 * and the reason it is gone: with the entry floating, one curve was still solved fresh on
	 * every placement, which is one remaining chance to produce something undrivable.
	 * Placement now VALIDATES - a road either passes through here or the stand is refused by
	 * name - and computes no geometry at all.
	 */
	UPROPERTY(VisibleAnywhere) FVector2D EntryLocal = FVector2D::ZeroVector;
	UPROPERTY(VisibleAnywhere) double EntryHeading = 0.0;

	/** Where the vehicle waits, and pointing which way. Angled; see the struct comment. */
	UPROPERTY(VisibleAnywhere) FVector2D ParkLocal = FVector2D::ZeroVector;
	UPROPERTY(VisibleAnywhere) double ParkHeading = 0.0;

	/** Where this bay's traffic leaves the stand. Shared with its side's other bays. */
	UPROPERTY(VisibleAnywhere) FVector2D ExitLocal = FVector2D::ZeroVector;
	UPROPERTY(VisibleAnywhere) double ExitHeading = 0.0;

	/** Road entry to the parking bay, forwards. Checked against the FORWARD limit. */
	UPROPERTY(VisibleAnywhere) FStandLeg ArriveLeg;

	/** Parking bay to the service point, forwards. Checked against the FORWARD limit. */
	UPROPERTY(VisibleAnywhere) FStandLeg ServeLeg;

	/** Clear of the aeroplane, backwards. Checked by FReverseRun::Start, which arms it. */
	UPROPERTY(VisibleAnywhere) FStandLeg ReverseLeg;

	/** Clear pose to the stand's exit, forwards. Checked against the FORWARD limit. */
	UPROPERTY(VisibleAnywhere) FStandLeg DepartLeg;
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
	 * What to call one of these where the PLAYER reads it - "the stand", "the fuel depot".
	 *
	 * ADDED FOR THE GUIDE LABELS, which are the first player-facing text that has to name a
	 * placed entity: FEntityInstance carries a pose, a definition and anchors, and the only
	 * naming anywhere in the codebase before this was a log line printing the asset's name.
	 *
	 * FALLS BACK TO THE ASSET NAME when unset - see EntityNaming::Describe - so every existing
	 * .uasset keeps working without being re-authored. An empty FText here is a content task,
	 * not a bug, and must not read as one on screen.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Presentation")
	FText DisplayName;

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
	 * Every bay this layout paints - one per anchor a VEHICLE drives to, with the four legs
	 * of its visit already solved and proven.
	 *
	 * SOLVED ONCE, FOR EVERY VEHICLE, and that is the property the whole piece exists to get.
	 * Four attempts failed because each derived geometry per stand and therefore had to
	 * re-prove it drivable per stand; placement is now a TRANSFORM, and a transform preserves
	 * curvature, so a verified template cannot become undrivable by being put somewhere.
	 *
	 * COMPUTED by BuildStandTemplate, never authored beside the anchors. Deriving it at rebuild
	 * time instead was rejected for the reason the old ring's header gave: that is a runtime
	 * algorithm's opinion with no override, and a second evaluator of the same geometry.
	 *
	 * INVISIBLE, and a decision rather than an omission: no marking builder, no material, no
	 * mesh. It exists as guideline nodes and edges and shows in the G overlay because
	 * everything in the graph does. The one thing here that SHOULD eventually be painted is
	 * the wing keep-out, which real aprons mark as a no-entry box - see
	 * IcaoCode::WingKeepOutContains, and note that nothing draws it yet.
	 */
	UPROPERTY(VisibleAnywhere) TArray<FServiceBay> ServiceBays;

	/**
	 * How much ground this layout actually needs: X is WIDTH (across, the local Y axis) and
	 * Y is DEPTH (along, the local X axis), uu.
	 *
	 * WIDTH-THEN-DEPTH RATHER THAN AN (X, Y) EXTENT, so that it reads straight into
	 * IcaoCode::LetterForStandSize and the template can name its own letter. An extent in
	 * local axes would have to be swapped at that call, and a swap nobody notices is a stand
	 * that reports the wrong code.
	 *
	 * DERIVED FROM THE POSES AND LEGS, never typed. A typed figure would be a second opinion
	 * about how big the layout is, and the two would drift the first time a bay moved - which
	 * is the whole reason StandWidthForLetter derives width rather than storing it.
	 */
	UPROPERTY(VisibleAnywhere) FVector2D RequiredExtent = FVector2D::ZeroVector;

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
	 * How this plot's modules are arranged. See Build/PlotLayoutStrategy.h.
	 *
	 * SCATTER IS THE DEFAULT so an un-migrated definition keeps the behaviour it had. A
	 * default of FuelYardBands would silently re-shape every plot ever saved, which is the
	 * same reason PoseRole above defaults to Aircraft.
	 */
	UPROPERTY(EditAnywhere) EPlotLayout Layout = EPlotLayout::Scatter;

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
	 * Lay the layout template - entry, staging rank, a bay per service anchor, and the legs
	 * between them - for a stand of this ICAO code Letter, sized for Largest.
	 *
	 * SEPARATE FROM BuildCodeCStandFor, and not merely extracted from it: the anchors above
	 * are Code C's plant, and this is the RULE that turns any letter's anchors into a
	 * drivable layout. It reads its box from IcaoCode and nothing else, which is what lets
	 * one verification cover a whole width band - see UEntityDefinition::RequiredExtent.
	 *
	 * TAKES THE ANCHORS AS IT FINDS THEM. It adds no fixture and moves none: a bay is paint
	 * laid beside plant that is already there.
	 *
	 * TAKES THE ENUM, not a string: every caller of this function holds a compile-time letter
	 * ("C" today, from BuildCodeCStandFor) rather than one read off a data asset, so there is
	 * no authored content here for IcaoCode::Parse to fail on - see Solve/IcaoCode.h.
	 */
	static void BuildStandTemplate(
		UEntityDefinition& Definition, EIcaoCode Letter, const FAirframe& Largest);

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
