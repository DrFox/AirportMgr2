#pragma once

#include "CoreMinimal.h"
#include "Content/DepotModuleLook.h"
#include "Content/FenceKit.h"
#include "Engine/DeveloperSettings.h"
#include "Model/RoadEntity.h"
#include "Model/Vehicle.h"
#include "Profiles/RoadDesignVehicles.h"
#include "AirsideSettings.generated.h"

class UAirsideContent;
class USkeletalMesh;
class UStaticMesh;
class UEntityDefinition;
class UAircraftType;
enum class EPlaceableEntity : uint8;
enum class EIcaoCode : uint8;

/** What UAirsideSettings::ResolveAgentView resolved - see its own comment. */
USTRUCT()
struct FResolvedAgentView
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<USkeletalMesh> Mesh = nullptr;
	UPROPERTY() TObjectPtr<UClass> AnimClass = nullptr;
};

/**
 * What UAirsideSettings::ResolveRigView (and any sibling Resolve*View for another articulated
 * vehicle - see ResolveRigView's own comment) resolved: the powered unit plus one entry per
 * FVehicle::Tow link, IN TOW ORDER.
 *
 * ONE ENTRY PER LINK, NOT PER BODY. A link with no body of its own - FTowLink::BodyFront and
 * BodyRear both zero, a bar (see FTowLink) - resolves to an EMPTY FResolvedAgentView
 * (Mesh == nullptr) rather than being left out of the array, so Links[i] always answers
 * Tow[i] and a caller walking the chain never has to know in advance which links carry a
 * body. That is the whole reason this is its own struct rather than a single
 * FResolvedAgentView: the rig's one-link semi-trailer chain and a future two-link drawbar
 * chain (a bar, then a body) both fit through the SAME shape with no change to it - only the
 * content fields and the resolver function differ per vehicle.
 */
USTRUCT()
struct FResolvedTowView
{
	GENERATED_BODY()

	/** The powered unit - the rig's tractor. */
	UPROPERTY() FResolvedAgentView Cab;

	/** One entry per FVehicle::Tow link, in order - see this struct's own comment. */
	UPROPERTY() TArray<FResolvedAgentView> Links;
};

/**
 * Where the plugin is told which content set to use. The ONE remaining path, and it is data.
 *
 * A UDeveloperSettings puts it in Config/DefaultAirside.ini as one readable line and gives it
 * an asset picker under Project Settings > Plugins > Airside. That matters for two reasons
 * this project cares about: it is plain text, so a change to it shows up in a diff the way
 * every other decision here does; and it is not C++, so the eight literals it replaces can
 * never again be a reference the editor cannot see.
 *
 * It does not solve the problem so much as reduce it from eight to one - a folder move still
 * leaves this line stale. The difference is that everything BEHIND it is a real asset
 * reference the editor maintains, so the one line only changes when the content set itself
 * moves, and a wrong value here fails loudly at first use rather than silently at CDO time.
 */
UCLASS(config = Airside, defaultconfig, meta = (DisplayName = "Airside"))
class AIRSIDE_API UAirsideSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UAirsideSettings();

	/**
	 * What a square metre of apron costs to lay, and what a day of owning one costs.
	 *
	 * HERE AND NOT ON AN ASSET, which is the one exception to the rule URoadProfile and
	 * UEntityDefinition follow - and it is forced, not a shortcut. FApronSurface is an outline
	 * and a material slot name; there is deliberately NO per-apron asset, because bands and
	 * lanes are meaningless for a polygon (see RoadApron.h). This class is already this
	 * project's single door for a content default with no better home, so the rate goes here.
	 *
	 * The consequence, named so the null is not read later as a bug: an apron's FBuildQuote
	 * carries no Source asset, so an M4 research discount cannot single aprons out the way it
	 * can single out a taxiway profile.
	 *
	 * 15 per square metre sits between taxiway (13) and runway (25), which is the ORDERING to
	 * preserve when these are tuned; the magnitude is an unplayed first pass.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0"))
	double ApronCostPerSquareMetre = 15.0;

	/** A thousandth of the build cost per day, the same ratio the profiles are authored at. */
	UPROPERTY(config, EditAnywhere, Category = "Cost", meta = (ClampMin = "0.0"))
	double ApronUpkeepPerSquareMetrePerDay = 0.015;

	/** Defaults for materials, profiles and the stand. See UAirsideContent. */
	UPROPERTY(config, EditAnywhere, Category = "Content")
	TSoftObjectPtr<UAirsideContent> Content;

	/**
	 * The content set, loaded on first use, or null if none is configured.
	 *
	 * NOT callable from a constructor, and that is the point rather than a limitation. The
	 * paths this replaces were read at CDO construction, which is the one moment the asset
	 * registry is not reliably up, and ConstructorHelpers exists precisely to work around
	 * that. Every caller now asks at the moment it needs the asset - a rebuild, a spawn -
	 * by which time loading is ordinary.
	 *
	 * Logs once, loudly, when a content set is configured and cannot be loaded. A missing
	 * default is a supported state; a configured one that is not there is a mistake, and
	 * silence about it is what cost this project an hour of looking at empty roads.
	 */
	static const UAirsideContent* GetContent();

	/**
	 * How many times GetContent has actually run, for issue #190's cache test: every
	 * ARoadNetworkActor::Resolve*Material call reaches this first, so a rebuild that changed
	 * nothing MakeSurfaceSettings' cache reads should see this stay flat rather than climbing
	 * by one per resolver per rebuild. Free-standing for the same reason
	 * NodeClaimsCallCountForTest is: GetContent is static and every caller goes through it.
	 */
	static int32 GetContentCallCountForTest;

	/** Zeroes the counter above - see GetContentCallCountForTest. */
	static void ResetGetContentCallCountForTest() { GetContentCallCountForTest = 0; }

	/**
	 * The airframe a route wears when there is no design aircraft to ask - THE ONE place
	 * UAircraftType::PiperMeridian*() may still be called from production code.
	 *
	 * Issue #30: those four functions used to be called directly at seven sites (RouteTool's
	 * three *For helpers, RoadBuildController::LandAircraftNearViewFocus's four), each one a place the
	 * fallback could drift from the others. Content->DefaultAircraft is preferred when the
	 * content set names one - Airframe() is what makes UAircraftType::Airframe stop being
	 * dead code - and the Piper is the fallback for a project with no content set configured
	 * at all, which every automation test still is.
	 */
	static FAirframe ResolveDefaultAirframe();

	/**
	 * The performance a SERVICE VEHICLE moves with - the one place a truck's figures live.
	 *
	 * AN FVehicle SINCE 2026-09-23. It was an FAirframe - "scaffolding, named as such by the
	 * fuel-service spec (§0.1)": FRouteFollower, FRoadAgent and the arbiter all took one
	 * FAirframe, and M3 was to replace it "with a vehicle-shaped performance bundle when the
	 * fleet arrives". The articulated rig arrived first, and a trailer on FAirframe would have
	 * put a kingpin on every aeroplane, so the bundle came now: FChassis (what rolls) plus a
	 * type code. See Model/Vehicle.h.
	 *
	 * NOTHING IS ZEROED ANY MORE. Climb and Approach used to be zeroed here so IsSet() answered
	 * false - their defaults are a light twin's, and a van left at them would have reported
	 * itself landable to ArrivalPlanner and FRoadAgent, which both branch on that call. An
	 * FVehicle has no such fields, and FRoadAgent::AsAircraft() is null for one.
	 *
	 * Wingspan is no longer a field either: FRoadAgent::Wingspan() answers 0 for a vehicle,
	 * which FRouteQuery::Wingspan reads as UNLIMITED - the right answer rather than a lax one,
	 * since a road guideline carries no span limit either.
	 */
	static FVehicle ResolveDefaultVehicle();

	/**
	 * The articulated fuel rig (truckCab1 + tankTrailer1), for the big-aircraft stands - the
	 * Wide road tier's design vehicle (spec 2026-09-23 §6). HARD-CODED for ResolveDefaultVehicle's
	 * reason: there is no vehicle type asset yet. Not dispatched by anything until the rig is
	 * imported (articulated step 2); route search and the Wide corners are sized against it now.
	 */
	static FVehicle ResolveRigVehicle();

	/**
	 * The biggest thing that may drive on a service road, which is what the road's corners
	 * are sized for.
	 *
	 * A SEPARATE FUNCTION FROM ResolveDefaultVehicle even though it returns the same airframe
	 * today, because the two answer different questions and will diverge the moment a second
	 * vehicle class exists: "what does a truck without a type look like" against "what must
	 * every service road be able to turn". Merged, a new larger dispenser would silently
	 * widen nothing, or a small van would silently narrow every junction.
	 *
	 * THE RULE IS THE ONE AIRCRAFT GEOMETRY ALREADY USES. IcaoCode::RadiusForLetter sweeps a
	 * painted taxi line for the largest aircraft a stand admits, never for the one taxiing
	 * now. This is that rule, for vehicles. It was INVERTED on 2026-09-14 - a truck's steering
	 * lock was widened from 45 to 50 degrees so it would fit an authored 500 uu corner, which
	 * is shrinking the vehicle to fit the road - and this function exists to make that
	 * inversion impossible to repeat.
	 *
	 * A CHASSIS, NOT AN AIRFRAME, since 2026-09-23: road corners are sized from the lock and
	 * the wheelbase, and nothing that sizes concrete has any use for a climb rate.
	 */
	static FChassis ResolveLargestServiceVehicle();

	/**
	 * THE ONE PLACE a service-road width tier names its design vehicle (user ruling 2026-09-25,
	 * see FRoadDesignVehicles): the Wide tier - ServiceRoadProfiles[WideServiceTier] - is
	 * designed for the articulated rig (ResolveRigVehicle), and every other tier is left to the
	 * default, the largest rigid service vehicle. Only the profiles that DIFFER from that default
	 * are named. Reads the content set, so a caller in a rebuild caches it (ARoadNetworkActor's
	 * resolved-content cache) rather than calling it per rebuild.
	 * ENFORCED BY: Airside.Build.DesignVehicle.WideDeadEndAdmitsRig and its siblings
	 */
	static TMap<TObjectKey<URoadProfile>, FVehicle> ResolveTierDesignVehicles();

	/**
	 * ResolveLargestServiceVehicle as the default, with ResolveTierDesignVehicles' exceptions:
	 * what a caller that asks once (a profile's own ResolvedFilletRadius, a test) is handed.
	 */
	static FRoadDesignVehicles ResolveRoadDesignVehicles();

	/**
	 * The Wide service-road tier's index in UAirsideContent::ServiceRoadProfiles, which is
	 * authored narrow first: Narrow 0, Standard 1, Wide 2. Named, not typed at the call site.
	 */
	static constexpr int32 WideServiceTier = 2;

	/**
	 * How many times ResolveLargestServiceVehicle has actually run, for issue #190's test
	 * that a rebuild resolves it ONCE and hands the answer down, rather than re-building the
	 * FAirframe by value per arm (RoadNetworkSolver's BuildNodeInput), per ordered arm pair
	 * (FRoadGuidelineBuilder::Build) or twice per link (FAnchorLink::Join). A free-standing
	 * counter, not a member - this function is static and every caller reaches it the same
	 * way NodeClaimsCallCountForTest's own comment explains for FRoadNetworkSolver.
	 */
	static int32 ResolveLargestServiceVehicleCallCountForTest;

	/** Zeroes the counter above - see ResolveLargestServiceVehicleCallCountForTest. */
	static void ResetResolveLargestServiceVehicleCallCountForTest() { ResolveLargestServiceVehicleCallCountForTest = 0; }

	/**
	 * What an aircraft's view should wear: THE AGENT'S OWN AIRFRAME FIRST, falling back to
	 * the content set's - and if the type has a mesh but no anim Blueprint of its own, the
	 * content default anim Blueprint drives it rather than leaving it unanimated.
	 *
	 * Moved out of UAirsideTraffic::SpawnView (#104): three branches of authored/content/
	 * mixed fallback belong beside every other Resolve*, not inline in Present/, which is
	 * how this project keeps "what does content default to" answered in exactly one place.
	 * This used to read Content->AgentMesh unconditionally - ONE mesh for every aircraft in
	 * the game - so a Twin Otter was offered, dispatched and landed as a Meridian: the name,
	 * the figures and the refusal reasons were all the right type's, and only the aeroplane
	 * on the runway was not.
	 */
	static FResolvedAgentView ResolveAgentView(const FAirframe& Airframe);

	/**
	 * What a placement gesture of this KIND drops, by the content set's Placeables map - or
	 * null with no content set configured, or none authored for this kind.
	 *
	 * THE ONE PLACE UAirsideContent::Placeables IS READ (issue #192 item 1), matching every
	 * other Resolve* here: ARoadNetworkActor::ResolveEntityDefinition used to answer this with
	 * a ternary over exactly two members, which is a list a third kind could join without
	 * anything here noticing - the "check where a list is CONSUMED" failure this codebase has
	 * shipped three times. A per-actor override still wins first (StandDefinition /
	 * FuelDepotDefinition on the actor); this is only the content-set fallback, called from
	 * ARoadNetworkActor::ResolveStandDefinition / ::ResolveFuelDepotDefinition.
	 */
	static UEntityDefinition* ResolvePlaceable(EPlaceableEntity Kind);

	/**
	 * The largest shipped UAircraftType whose ICAO code letter is Letter, or null - the
	 * drawn-stand commit path's one place to ask "what would actually park on a Code X
	 * stand", matching every other Resolve* here (CLAUDE.md's "Content/ resolves every
	 * content default in exactly one function").
	 *
	 * RETURNS NULL FOR EVERY LETTER TODAY, and that is a finding, not a stub left half done.
	 * UAirsideContent carries exactly one aircraft reference (DefaultAircraft, a single
	 * TSoftObjectPtr) - no per-letter table and no list of shipped UAircraftType assets to
	 * search (grepped: there is no AircraftTypes array anywhere in AirsideContent.h). Adding
	 * an AssetRegistry scan to answer one letter would be exactly the kind of mechanism this
	 * project reaches for only when nothing simpler exists, and a scan built for this one
	 * caller would still need a per-letter Code match on every asset found, which is a second
	 * table by another name. Code C does not call this at all - it keeps its authored
	 * DA_Stand_CodeC / DA_Aircraft_A320 pairing through ResolveStandDefinition - so this is
	 * the seam a future per-letter aircraft asset would resolve through, not a duplicate of
	 * Code C's own answer.
	 */
	static UAircraftType* ResolveLargestAircraftOfLetter(EIcaoCode Letter);

	/** A service vehicle's body mesh - the content default, or null with none configured. */
	static UStaticMesh* ResolveVehicleMesh();

	/** The chainlink fence's meshes and material - the content defaults, each null if unset. */
	static FFenceKit ResolveFenceKit();

	/**
	 * Every depot module's meshes, indexed by EDepotModule and walked to its sentinel like
	 * DepotKitSpecs. A module with no kit, or a kit with no meshes, resolves to a look whose
	 * HasMeshes() is false - which the presenter draws as the grey box.
	 */
	static TArray<FDepotModuleLook> ResolveDepotLooks();

	/**
	 * A service vehicle's RIGGED body and the graph that drives it, or an empty view when the
	 * content set names none - in which case the caller falls back to ResolveVehicleMesh.
	 *
	 * THE SIBLING OF ResolveAgentView, and deliberately not a branch inside it: an aircraft
	 * resolves its mesh from its own FAirframe FIRST and only then from the content set,
	 * because a Twin Otter offered as a Meridian was a real defect. A vehicle has no such
	 * per-type asset yet - ResolveDefaultVehicle hands every truck the same scaffolding
	 * FAirframe - so there is nothing to prefer and adding the branch now would be inventing
	 * a fallback order for a choice nobody makes.
	 *
	 * The anim class is taken WITHOUT a fallback to AgentAnimClass: an aircraft's graph
	 * drives bones named prop and nosewheel_steer, and pointed at a truck it would find none
	 * of them and silently animate nothing. Better an unanimated truck that is obviously
	 * unwired than one that looks wired and is not.
	 */
	static FResolvedAgentView ResolveVehicleView();

	/**
	 * The articulated rig's view: truckCab1's mesh and ABP, plus tankTrailer1's for
	 * ResolveRigVehicle's one Tow link - or an empty view for any link/asset the content set
	 * does not name, matching ResolveVehicleView's null-safe shape.
	 *
	 * A SEPARATE FUNCTION FROM ResolveVehicleView, for the reason ResolveRigVehicle is
	 * separate from ResolveDefaultVehicle: the content diverges completely (two meshes, not
	 * one) rather than being a second reading of the same field, so branching inside
	 * ResolveVehicleView on HasTrailer() would make one function resolve two unrelated sets
	 * of soft pointers - the "Content/ resolves every content default in exactly one
	 * function" rule read the other way round.
	 *
	 * TAKES NO FVehicle, mirroring ResolveRigVehicle's own no-arg shape: the content it reads
	 * (RigCabMesh/RigCabAnimClass/RigTrailerMesh/RigTrailerAnimClass on UAirsideContent) is
	 * fixed to THIS rig, the way ResolveRigVehicle's figures are. FResolvedTowView::Links is
	 * sized to ResolveRigVehicle's own Tow.Num() (one today), so a future second rig link
	 * would need a second content field here and a second entry there, not a change to the
	 * struct itself - see FResolvedTowView's own comment.
	 *
	 * A sibling ResolveUtilityTowView() for utility1 + fuelTrailer1's two-link chain (a bar,
	 * then a body) would follow the same shape: its own content fields, its own function,
	 * the same FResolvedTowView return type.
	 */
	static FResolvedTowView ResolveRigView();

	/**
	 * utility1 towing fuelTrailer1 (spec 2026-09-24 revision, section 4) - the SIBLING
	 * ResolveRigView's own comment promised: utility1 is the powered unit, and Tow holds the
	 * drawbar's TWO links - the towbar (a bar: BodyFront and BodyRear both zero) then the body.
	 * MIRRORS ResolveRigVehicle's shape (a chassis from ResolveDefaultVehicle, overridden with
	 * this vehicle's own measured axles and body, plus a Tow chain), not called from it: the
	 * two vehicles share no figures, so there is nothing for one function to hand the other.
	 */
	static FVehicle ResolveUtilityTowVehicle();

	/**
	 * utility1's mesh and ABP, plus fuelTrailer1's for ResolveUtilityTowVehicle's two Tow
	 * links - or an empty view for any mesh/ABP the content set does not name.
	 *
	 * A SEPARATE FUNCTION FROM ResolveRigView, for the reason ResolveRigView already gives for
	 * being separate from ResolveVehicleView: the content diverges completely (utility1's and
	 * fuelTrailer1's own soft pointers, not a second reading of RigCabMesh/RigTrailerMesh).
	 *
	 * Links.Num() == 2, matching ResolveUtilityTowVehicle's own two-link Tow: Links[0] (the
	 * towbar) resolves to an EMPTY FResolvedAgentView (Mesh == nullptr) always, because the
	 * towbar carries no mesh of its own - fuelTrailer1 is ONE skinned asset covering both
	 * links, and Links[1] is where it (and its ABP) actually resolves. See FResolvedTowView's
	 * own comment: "a link with no body of its own... resolves to an EMPTY FResolvedAgentView
	 * rather than being left out of the array" - written for exactly this vehicle.
	 */
	static FResolvedTowView ResolveUtilityTowView();

	/**
	 * THE LOOK FOR THIS VEHICLE: its cab plus one entry per Tow link - ResolveRigView for the
	 * rig, ResolveUtilityTowView for utility1 + fuelTrailer1, and for anything else
	 * ResolveVehicleView as the Cab with no Links (rigid, as every truck was before).
	 *
	 * THE ONE PLACE THE CHOICE IS MADE, by TypeCode - the code each Resolve*Vehicle above stamps,
	 * from one constant per vehicle in this file - so the dresser (UAirsideTraffic::SpawnView)
	 * names no vehicle and no asset. Keyed on the TypeCode rather than on the Tow's shape
	 * because two vehicles may share a shape and not a look.
	 * ENFORCED BY: Airside.Present.RigActor.TrailerOnItsLink (each vehicle's cab wears its own mesh).
	 */
	static FResolvedTowView ResolveVehicleViewFor(const FVehicle& Vehicle);
};
