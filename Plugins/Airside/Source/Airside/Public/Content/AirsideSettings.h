#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Model/RoadEntity.h"
#include "AirsideSettings.generated.h"

class UAirsideContent;
class USkeletalMesh;
class UStaticMesh;

/** What UAirsideSettings::ResolveAgentView resolved - see its own comment. */
USTRUCT()
struct FResolvedAgentView
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<USkeletalMesh> Mesh = nullptr;
	UPROPERTY() TObjectPtr<UClass> AnimClass = nullptr;
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
	 * AN FAirframe FOR A THING WITH NO AIRFRAME, and that is scaffolding, named as such by
	 * the fuel-service spec (§0.1). FRouteFollower, FRoadAgent and the arbiter all take one
	 * FAirframe, so giving a truck anything else this slice would mean a second follower
	 * before there is a second KIND of movement to justify one. M3 replaces this with a
	 * vehicle-shaped performance bundle when the fleet arrives.
	 *
	 * ONLY Ground IS SET, and Climb and Approach are deliberately ZEROED so IsSet() answers
	 * false for both. Their struct defaults are a light twin's and are all non-zero, so a
	 * van left at them would report itself landable to anything that asked - and both
	 * ArrivalPlanner and FRoadAgent branch on exactly that call. Nothing asks today; that is
	 * the reason to make it false by construction rather than by nobody having got round to
	 * it. Engine is the one exception, for the reason given at the assignment.
	 *
	 * Wingspan 0 because 0 is UNLIMITED in the edge test (FRouteQuery::Wingspan) - the right
	 * answer rather than a lax one, since a road guideline carries no span limit either, so
	 * neither side of that comparison means anything for a van.
	 */
	static FAirframe ResolveDefaultVehicle();

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

	/** A service vehicle's body mesh - the content default, or null with none configured. */
	static UStaticMesh* ResolveVehicleMesh();
};
