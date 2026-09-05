#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "AirsideContent.generated.h"

class UEntityDefinition;
class UMaterialInterface;
class URoadProfile;
class UAnimInstance;
class USkeletalMesh;
class UAircraftType;

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

	// NO MaterialSet HERE, deliberately. A null one on the actor is not an unset field, it is
	// the single-material road - so offering a default silently converts every airport that
	// chose it. Assign one on the actor to get per-band materials.

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

	/** What the stand tool places. */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<UEntityDefinition> DefaultStand;

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
	 * see the rig built by AirportMgr2Models/piperMeridian/scripts/build_lowpoly.py.
	 */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<USkeletalMesh> AgentMesh;

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
