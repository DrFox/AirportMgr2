#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
// INCLUDED RATHER THAN FORWARD DECLARED: EDepotModule keys DepotKits below, and EPlaceableEntity
// keys Placeables below it, and UHT needs an enum's definition to reflect a TMap key. Content/
// carries no include-direction rule - Check-Architecture constrains Model/, Solve/, Tool/ and
// Build/ only.
#include "Model/RoadEntity.h"
#include "Entities/EntityDefinition.h"
#include "AirsideContent.generated.h"

class UMaterialInterface;
class URoadProfile;
class UAnimInstance;
class USkeletalMesh;
class UStaticMesh;
class UAircraftType;
class UPlotModuleKit;

/**
 * The content this plugin reaches for when nothing has been assigned by hand.
 *
 * WHY IT EXISTS. These references used to be string literals in constructors, resolved by
 * ConstructorHelpers::FObjectFinder at CDO time. That worked and it was the reason a freshly
 * placed ARoadNetworkActor rendered as asphalt with no setup - but a path in C++ is a
 * reference the editor cannot see, so moving a content folder left eight of them pointing at
 * nothing. The editor fixed up every other reference in the project and could not fix those,
 * because it did not know they were there. An hour of "the roads have gone" followed.
 *
 * Here they are ASSET REFERENCES. Move a folder and the editor repoints them like anything
 * else. That is the whole point, and it is worth more than the indirection costs.
 *
 * Soft pointers throughout: this is a set of DEFAULTS, and a project that assigns its own
 * materials on the actor should not pay to load ours. Each is loaded at the moment it is
 * first wanted - see UAirsideSettings.
 *
 * EVERY FIELD IS OPTIONAL. Null means "no default for this", which is the state the plugin
 * already had to handle: no material set is the supported single-material road, no stand
 * definition is a tool that places nothing, and no profile is a network with no width to
 * draw. A missing entry must degrade the way it always did, never assert.
 */
UCLASS(BlueprintType)
class AIRSIDE_API UAirsideContent : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The road and taxiway surface. Null falls back to the engine default plus a colour. */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TSoftObjectPtr<UMaterialInterface> SurfaceMaterial;

	/** Apron concrete. A different surface from the taxiway, deliberately. */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TSoftObjectPtr<UMaterialInterface> ApronMaterial;

	/** The translucent preview. Never scenery - see the ghost component. */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TSoftObjectPtr<UMaterialInterface> GhostMaterial;

	/**
	 * The tyre rubber in a runway's touchdown zone - Tools/Python/build_rubber_material.py.
	 *
	 * TRANSLUCENT, and that is what makes it one material rather than one per runway
	 * surface. A rubber patch darkens whatever pavement it lies on, so it never has to know
	 * whether that is tarmac or concrete; an opaque patch would have to REPRODUCE the
	 * surface underneath it, and one component covering runways of two different surfaces
	 * cannot reproduce both.
	 *
	 * Null leaves the rubber undrawn. That is a supported state, not a defect: rubber is
	 * decoration, and an airport with no rubber material is an airport whose runways look
	 * newly laid.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TSoftObjectPtr<UMaterialInterface> RubberMaterial;

	/**
	 * The puff of tyre smoke at touchdown - Tools/Python/build_puff_material.py.
	 *
	 * Null leaves the puffs undrawn, and that is a supported state rather than a defect: an
	 * airport with no smoke material is one where aircraft land quietly.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TSoftObjectPtr<UMaterialInterface> TyreSmokeMaterial;

	/**
	 * What a runway's pavement looks like, by its surface fact - see FRunwayFacts. Indexed by
	 * RunwayMaterialSlot(Surface), which is the ONE place Reinforced aliases to Concrete's
	 * slot - replacing the switch that used to live in ResolveRunwayMaterial AND the
	 * array-of-struct RoadSurfacePresenter.cpp built from three named properties here.
	 *
	 * THREE SLOTS: reinforced is concrete with a stronger rating, and the difference shows
	 * in the details panel and in what may land there, not on the ground (spec 2026-09-07
	 * ยง8). A TArray, not a TStaticArray: UPROPERTY reflection has no TStaticArray support -
	 * see URoadSurfacePresenter::LayerComponents' own comment for the same constraint. Each
	 * slot is an instance of the road surface with its centreline width at zero, because the
	 * only line on a runway is the white one FRunwayMarkingBuilder paints - authored by
	 * Tools/Python/build_runway_materials.py. A null or missing slot falls back to
	 * SurfaceMaterial, yellow line and all, which is what runways looked like before.
	 *
	 * NO ASSET EDIT NEEDED (PR #137 review reversed the earlier "needs resave" note): PostLoad
	 * migrates the three deprecated properties below into this array itself, so an asset
	 * authored before RunwayMaterials existed keeps its runway materials with no manual step.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TArray<TSoftObjectPtr<UMaterialInterface>> RunwayMaterials;

	// NO MaterialSet HERE, deliberately. A null one on the actor is not an unset field, it is
	// the single-material road - so offering a default silently converts every airport that
	// chose it. Assign one on the actor to get per-band materials.

	/**
	 * DEPRECATED (issue #105 item 4, PR #137 review). An asset saved before RunwayMaterials
	 * existed still has its bytes under these three tagged-property names - UE's tagged
	 * serialisation matches by name, and the "_DEPRECATED" suffix is what tells it to keep
	 * matching them here rather than dropping the bytes on the floor, while also hiding the
	 * fields from the details panel and Blueprint so nothing new can be authored into them.
	 * PostLoad migrates whichever of these are set into RunwayMaterials and never reads them
	 * again afterward - see PostLoad's own comment for the slot mapping.
	 */
	UPROPERTY() TSoftObjectPtr<UMaterialInterface> RunwayGrassMaterial_DEPRECATED;
	UPROPERTY() TSoftObjectPtr<UMaterialInterface> RunwayTarmacMaterial_DEPRECATED;
	UPROPERTY() TSoftObjectPtr<UMaterialInterface> RunwayConcreteMaterial_DEPRECATED;

	//~ Begin UObject Interface
	virtual void PostLoad() override;
	//~ End UObject Interface

	/**
	 * The runway cross-sections, one per standard width, widest last.
	 *
	 * AUTHORED ASSETS, and that is not a preference. A runway segment stores a pointer to its
	 * profile, and URoadNetwork::DefaultProfile repairs any segment whose pointer came back
	 * null from a save - with the TAXIWAY profile. A transient runway profile would therefore
	 * not merely vanish, it would come back as a taxiway: right width lost, continuity lost,
	 * and a junction paved across the middle of the runway. Nothing would report it.
	 *
	 * Widths are the ICAO set - 18, 23, 30, 45 and 60 m - because a runway conforms to one of
	 * them or it is not a runway. The tool picks from this list rather than taking a number,
	 * which is what makes "conforms to certain widths" true by construction instead of by
	 * validation.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TArray<TSoftObjectPtr<URoadProfile>> RunwayProfiles;

	/**
	 * The standard TAXIWAY widths a player cycles through, narrowest first.
	 *
	 * AUTHORED ASSETS, for the reason RunwayProfiles gives above: a segment stores a
	 * pointer to its profile, and a transient one would come back from a save as something
	 * else entirely.
	 *
	 * Widths are the ICAO code letters - 10.5, 15, 18, 23 and 25 m for B through F -
	 * because a taxiway conforms to one of them or it is not a taxiway. The tool picks from
	 * this list rather than taking a number, which is what makes that true by construction
	 * instead of by validation.
	 *
	 * SEPARATE FROM the taxiway a level defaults to. That is ARoadNetworkActor's own
	 * Profile, tuned per instance and deliberately not read from here - see
	 * ARoadNetworkActor::ResolveProfile, whose comment records what happened when the
	 * content set was consulted for it. Empty is legal and means the width cycle has
	 * nothing to offer, which the tool says out loud.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TArray<TSoftObjectPtr<URoadProfile>> TaxiwayProfiles;

	/**
	 * The SERVICE ROAD cross-section a ground vehicle drives on.
	 *
	 * AN AUTHORED ASSET, for exactly the reason RunwayProfiles gives: a segment stores a
	 * POINTER to its profile, and URoadNetwork::DefaultProfile repairs any segment whose
	 * pointer came back null from a save - with the TAXIWAY profile. A transient service-road
	 * profile would therefore not merely vanish on reload, it would come back as a taxiway:
	 * the road widened to 23 m and, far worse, admitting AIRCRAFT onto a lane laid for vans,
	 * with nothing anywhere to report it.
	 *
	 * Null is still legal, and means the road tool refuses to lay one and says which asset is
	 * missing - the same treatment URoadEditFacade::PlaceRunway gives a missing runway profile,
	 * and for the same reason: a silent fallback here is the wrong behaviour at every junction.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<URoadProfile> ServiceRoadProfile;

	/**
	 * DEPRECATED (issue #192 item 1). What the stand tool placed before Placeables existed.
	 *
	 * meta = (DeprecatedProperty) RATHER THAN AN "_DEPRECATED" NAME SUFFIX, unlike the three
	 * runway materials above: those were RENAMED, so the suffix is what keeps UE's
	 * tagged-property serialisation matching an old asset's bytes against a name that no
	 * longer exists in the details panel. This field keeps its ORIGINAL name - nothing else
	 * is claiming it - so the deprecation meta alone is enough to hide it from the panel and
	 * Blueprint while PostLoad still finds it under the tag an old asset saved. See PostLoad
	 * for the migration into Placeables[EPlaceableEntity::Stand].
	 */
	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Use Placeables[EPlaceableEntity::Stand]"))
	TSoftObjectPtr<UEntityDefinition> DefaultStand;

	/**
	 * DEPRECATED (issue #192 item 1). What the fuel depot tool placed before Placeables
	 * existed. See DefaultStand's own comment for why this keeps its name rather than
	 * gaining an "_DEPRECATED" suffix, and PostLoad for the migration into
	 * Placeables[EPlaceableEntity::FuelDepot].
	 */
	UPROPERTY(meta = (DeprecatedProperty, DeprecationMessage = "Use Placeables[EPlaceableEntity::FuelDepot]"))
	TSoftObjectPtr<UEntityDefinition> DefaultFuelDepot;

	/**
	 * What each placeable KIND resolves to by default. See UAirsideSettings::ResolvePlaceable,
	 * the one place this is read.
	 *
	 * A MAP KEYED BY EPlaceableEntity, replacing DefaultStand / DefaultFuelDepot above -
	 * EntityDefinition.h's own comment claims "a new kind is a new data asset, not new code",
	 * and a hand-named slot per kind plus a ternary resolver
	 * (ARoadNetworkActor::ResolveEntityDefinition) never lived up to that, the way DepotKits
	 * just below already does for EDepotModule. One new kind is now one new map entry and
	 * nothing else - the same "lists that must agree are one list" rule DepotKits follows.
	 *
	 * NO ASSET EDIT NEEDED, matching RunwayMaterials' own note above: PostLoad migrates the
	 * two deprecated properties into this map the first time an asset saved before it
	 * existed loads, so DA_AirsideContent keeps naming a stand and a fuel depot with no
	 * manual re-author step - which a renamed UPROPERTY cannot do on its own (a .uasset
	 * cannot be re-authored headlessly; see the project's own memory notes on that).
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TMap<EPlaceableEntity, TSoftObjectPtr<UEntityDefinition>> Placeables;

	/**
	 * The airframe a route wears when its start has no design aircraft to ask - most of the
	 * graph. See UAirsideSettings::ResolveDefaultAirframe, the one place this is read.
	 *
	 * BESIDE AgentMesh AND NOWHERE ELSE, deliberately: the two must describe the same
	 * airframe, or the thing on screen moves like an aeroplane it does not look like. Issue
	 * #30 was seven call sites hardcoding a Piper's numbers while the MESH already came from
	 * here - swap the mesh and the aircraft still taxied like a Piper, because only one of
	 * the two facts about it was data.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<UAircraftType> DefaultAircraft;

	/**
	 * The airframe a dispatched agent wears. Null leaves the placeholder cube.
	 *
	 * SKELETAL, because the propeller and the wheels turn. A static mesh cannot animate, and
	 * the parts that move are vertex groups inside one mesh rather than separate objects -
	 * see the rig built by AirportMgr2Models/plane7/scripts/build_rig.py, which since
	 * 2026-09-21 also retracts the gear and works four bay doors.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<USkeletalMesh> AgentMesh;

	/**
	 * What stands in a plot, by module.
	 *
	 * A MAP RATHER THAN A FIELD PER MODULE, because the consumer walks EDepotModule and a
	 * field per value could only be kept in step by someone remembering to add one - the
	 * failure AircraftLookTest exists for, where a hand-written pair passed while the A320
	 * and the 737 both still wore the default mesh.
	 *
	 * AN UNMAPPED MODULE FALLS BACK to DepotKit.cpp's grey-box table rather than failing.
	 * That is what lets the solver and presenter work land and be tested before a single
	 * mesh exists; a zero footprint instead would stack every module on the same spot.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TMap<EDepotModule, TObjectPtr<UPlotModuleKit>> DepotKits;

	/**
	 * What a GROUND VEHICLE agent looks like when there is no rigged one. Null leaves the
	 * placeholder box.
	 *
	 * STATIC, and now the FALLBACK rather than the only option - see VehicleSkeletalMesh
	 * below, which is preferred when it is set. This stays because a vehicle whose wheels do
	 * not need to turn has no business carrying a skeleton. The box it falls back to is sized
	 * from FTrafficRules::VehicleFootprint, so what is on screen is the length the arbiter
	 * actually keeps clear - see ARoadAgentActor::SetVehicleBody.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<UStaticMesh> VehicleMesh;

	/**
	 * The chainlink fence's ุ60 mm line post. Base-centred, 245 cm tall, 1 unit = 1 cm once
	 * imported - see AirportMgr2Models/accessories/chainlink/README.md. Null falls back to a
	 * scaled engine cube. Authored by Tools/Python/build_fence_content.py.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Fence")
	TSoftObjectPtr<UStaticMesh> FenceLinePost;

	/** The ุ90 mm corner and gate post - heavier instead of braced (README "Not here"). */
	UPROPERTY(EditAnywhere, Category = "Airside|Fence")
	TSoftObjectPtr<UStaticMesh> FenceHeavyPost;

	/**
	 * The fabric: Masked, two-sided, chainlink.png with its alpha-coverage mips. Null falls back
	 * to the sink's default surface material - visible and wrong, which is the failure to prefer.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Fence")
	TSoftObjectPtr<UMaterialInterface> FenceFabricMaterial;

	/**
	 * A RIGGED ground vehicle. Preferred over VehicleMesh; null falls back to it.
	 *
	 * "A truck's wheels turn and nothing else does, and there is no rig yet" is what stood
	 * beside VehicleMesh, and it was true until 2026-09-14. fueltruck1 now exports four wheel
	 * bones, two STEER bones carrying the front pair, and a beacon - the same steer->roll
	 * chain plane2's nose gear uses, so the roll axis turns with the steering rather than
	 * fighting it.
	 *
	 * BESIDE VehicleMesh rather than replacing it, which is the call AgentMesh already made:
	 * one property that might be either kind would have to be a TSoftObjectPtr<UObject> with
	 * a cast at the point of use, and the editor would offer every asset in the project in
	 * its picker.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<USkeletalMesh> VehicleSkeletalMesh;

	/**
	 * What drives the rigged vehicle's wheels and steering. Null leaves it in its reference
	 * pose - a truck that slides along with its wheels held still.
	 *
	 * A SOFT CLASS for the reason AgentAnimClass is one, and it must likewise be built on
	 * UAirsideAgentAnim, which is what computes WheelAngleDegrees and SteerAngleDegrees. A
	 * SEPARATE property from AgentAnimClass because the two graphs drive different bones: an
	 * aircraft's has no steer_FR to find.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftClassPtr<UAnimInstance> VehicleAnimClass;

	/**
	 * What drives the airframe's moving parts. Null leaves it posed in its reference pose.
	 *
	 * A SOFT CLASS, and here rather than in C++, for the reason the rest of this asset exists:
	 * an Animation Blueprint is content, and a path to it written into a constructor would be
	 * a reference the editor cannot fix up when the asset moves. It also has to be soft
	 * because the class lives in a Blueprint that C++ cannot name at compile time.
	 *
	 * Must be built on UAirsideAgentAnim, which is what computes the angles it applies.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftClassPtr<UAnimInstance> AgentAnimClass;
};
