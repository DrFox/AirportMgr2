#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadEntity.h"
#include "AircraftType.generated.h"

/**
 * One aircraft type: how big it is, and where on it each service is REQUIRED.
 *
 * The other half of a stand. A stand says what services the ground can provide; an
 * aircraft says where they have to be delivered - and those are different questions with
 * different answers. A Code C stand takes an A320 and a 737-800, and their forward hold
 * doors are metres apart, so a belt loader parked at "the stand's baggage position" is at
 * the wrong place for one of them. Baking one type's geometry into the stand is the bug
 * this asset exists to prevent.
 *
 * LOCAL SPACE has its origin at the NOSE GEAR, +X forward, +Y starboard. The nose gear is
 * what stops on the mark painted on the stand, so an aircraft parked on a stand shares that
 * stand's pose exactly - which is why composing the two needs no offset of its own.
 *
 * PROVENANCE, stated plainly: these come from published dimensions and standard ramp
 * practice, not from a manufacturer's ground-handling manual. Door stations are the figures
 * to check first if something looks wrong on a real layout.
 */
UCLASS(BlueprintType)
class AIRSIDE_API UAircraftType : public UDataAsset
{
	GENERATED_BODY()

public:
	/** ICAO aerodrome reference code letter - C for an A320, E for a 777. */
	UPROPERTY(EditAnywhere) FName Code;

	/** Shown in the overlay and the tool. */
	UPROPERTY(EditAnywhere) FText DisplayName;

	/** Plan extent, for drawing the thing the service points surround. */
	UPROPERTY(EditAnywhere) FEntityFootprint Footprint;

	/**
	 * Main wheel radius, uu. What a rolling wheel's spin rate is divided by.
	 *
	 * MEASURED off the model, not estimated: the Meridian's mains are 0.420 m across in
	 * piper_aligned.blend, and their axle sits at Z = 0.21 with the origin on the ground,
	 * which is the same number arrived at twice. An earlier guess of 0.34 m would have turned
	 * the wheels at two thirds of the right rate - slow enough to look like a skid.
	 */
	UPROPERTY(EditAnywhere) double MainWheelRadius = 21.0;

	/** Propeller diameter, uu. Measured at 1.814 m on the model; published is 2.03. */
	UPROPERTY(EditAnywhere) double PropellerDiameter = 181.4;

	/**
	 * Where each service connects to THIS aircraft: hold doors, refuel panel, galley
	 * doors, ground power receptacle, nose gear for the tug.
	 *
	 * Same struct as a stand's fixtures and a deliberately different meaning. A fixture is
	 * a thing dug into the concrete; a service point is a place on an airframe. Both are
	 * an id, a local pose and a role, so both use FEntityAnchor - and the id is the only
	 * way either should be addressed.
	 */
	UPROPERTY(EditAnywhere) TArray<FEntityAnchor> ServicePoints;

	/**
	 * How this airframe moves on the ground: power settings, creep speed, turn rate.
	 *
	 * On the TYPE and not on the taxiway, which is the opposite of where the lead-in sweep
	 * lives and deliberately so - see FGroundPerformance for why the two go different ways.
	 */
	UPROPERTY(EditAnywhere) FGroundPerformance Ground;

	/**
	 * What it does once the wheels are off: climb rate, attitude, and when it is gone.
	 *
	 * Beside Ground rather than inside it, because FGroundPerformance is named for what it
	 * describes and a climb rate in it would be a lie in the field list. See FClimbPerformance.
	 */
	UPROPERTY(EditAnywhere) FClimbPerformance Climb;

	/** How this type flies an approach. Its arrival declines if this is not authored. */
	UPROPERTY(EditAnywhere) FApproachPerformance Approach;

	/** How this type's propeller spools up and down. */
	UPROPERTY(EditAnywhere) FEnginePerformance Engine;

	/**
	 * The four performance structs plus Wingspan, bundled - see FAirframe for why.
	 *
	 * Assembled on demand rather than stored, because the four already live here as the
	 * authored source of truth; a cached FAirframe would be a second place they could drift
	 * out of step with what a designer edited.
	 */
	FAirframe Airframe() const
	{
		FAirframe Out;
		Out.Ground = Ground;
		Out.Climb = Climb;
		Out.Approach = Approach;
		Out.Engine = Engine;
		Out.Wingspan = Footprint.Wingspan;
		return Out;
	}

	/** The footprint as plan-view line segments in LOCAL space; pairs of points. */
	static void BuildFootprintLines(const FEntityFootprint& Footprint, TArray<FVector2D>& OutSegments);

	/**
	 * Every service point carries a non-empty id and no two share one.
	 *
	 * Exposed to script because the authoring commandlet runs the same check the model
	 * does - an id that names two points sends a belt loader to the refuel panel and
	 * reports success, and catching that at authoring time is cheaper than in a sim.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static bool HasUsableServiceIds(const UAircraftType* Type);

	/** An A320-200 with sharklets - the Code C workhorse. */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildA320(UAircraftType* Type);

	/**
	 * A 737-800, the OTHER Code C workhorse.
	 *
	 * Authored specifically because it shares stands with the A320 and puts its doors
	 * elsewhere: longer fuselage, holds further aft, refuel panel on the starboard wing at
	 * a different station. If the two ever produce identical service positions, the split
	 * between stand and aircraft has stopped doing its job.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void Build737(UAircraftType* Type);

	/**
	 * A PA-46-500TP Meridian - the airframe that is actually on screen.
	 *
	 * Authored because SM_PiperMeridian is what ARoadAgentActor draws, so an agent taxiing
	 * with an A320's turn rate is a Piper moving like an airliner. It is also the FALLBACK
	 * when a route starts somewhere with no design aircraft, which is most of the graph.
	 *
	 * Its LOCAL ORIGIN IS THE MAIN-GEAR AXLE, not the nose gear this class otherwise
	 * specifies, and that is a deviation with a reason rather than an oversight - see the
	 * comment at the footprint.
	 */
	UFUNCTION(BlueprintCallable, Category = "Airside")
	static void BuildPiperMeridian(UAircraftType* Type);

	/**
	 * The Meridian's ground performance on its own, without needing a UAircraftType.
	 *
	 * Exists because the FALLBACK needs it: a route that starts on a plain taxiway node has
	 * no design aircraft to ask, and the aircraft on screen is a Piper regardless. Making
	 * the caller build a transient asset to learn three numbers would have put those three
	 * numbers at the call site instead, which is how a figure ends up written twice.
	 */
	static FGroundPerformance PiperMeridianGround();

	/** The Meridian's climb, for the same reason PiperMeridianGround exists. */
	static FClimbPerformance PiperMeridianClimb();

	/** The Meridian's approach and flare, for the same reason again. */
	static FApproachPerformance PiperMeridianApproach();

	/** The Meridian's propeller spool rates, for the same reason again. */
	static FEnginePerformance PiperMeridianEngine();
};
