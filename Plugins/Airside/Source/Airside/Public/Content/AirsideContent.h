#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "AirsideContent.generated.h"

class UEntityDefinition;
class UMaterialInterface;
class URoadMaterialSet;
class URoadProfile;
class UStaticMesh;

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

	/** Per-band materials. Null is the supported single-material state. */
	UPROPERTY(EditAnywhere, Category = "Airside|Materials")
	TSoftObjectPtr<URoadMaterialSet> MaterialSet;

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

	/** The airframe a dispatched agent wears. Null leaves the placeholder cube. */
	UPROPERTY(EditAnywhere, Category = "Airside|Defaults")
	TSoftObjectPtr<UStaticMesh> AgentMesh;
};
