#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Model/RoadEntity.h"
#include "AircraftType.generated.h"

class UAnimInstance;
class USkeletalMesh;

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
	/** ICAO aerodrome reference code letter - C for an A320, E for a 777. NOT unique: an
	 *  A320 and a 737 are both C, which is why ShortCode exists beside it. */
	UPROPERTY(EditAnywhere) FName Code;

	/**
	 * The type's own short code - "PA46", "DHC6" - unique to this aircraft.
	 *
	 * SEPARATE FROM Code, which is the aerodrome reference letter and cannot tell two types
	 * apart. FAirframe::TypeCode used to be assigned Code and therefore inherited that
	 * ambiguity while its comment promised a short code. Falls back to Code when unset so an
	 * un-migrated asset still says something rather than nothing.
	 */
	UPROPERTY(EditAnywhere) FName ShortCode;

	/**
	 * What this type looks like. Null leaves UAirsideContent::AgentMesh, the game-wide
	 * default - which is what every aircraft used to wear regardless of type.
	 */
	UPROPERTY(EditAnywhere) TSoftObjectPtr<USkeletalMesh> Mesh;

	/** The anim Blueprint for Mesh. Null leaves UAirsideContent::AgentAnimClass. */
	UPROPERTY(EditAnywhere) TSoftClassPtr<UAnimInstance> AnimClass;

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

	/**
	 * How this type steers on the ground - see ESteerLaw.
	 *
	 * ROLLINGSTEER BY DEFAULT, which is the opposite of FAirframe's own default, and the
	 * asymmetry is deliberate. A bare FAirframe is a struct a test built by hand, and the
	 * safe assumption for one of those is the law that needs no measurements. An AIRCRAFT
	 * TYPE is different: every aeroplane in this game steers on a nosewheel, so Pivot here
	 * would be a claim no real type makes - and defaulting to it would have silently turned
	 * every already-authored type asset into a pivot on load, since a new UPROPERTY takes
	 * its default on assets saved before it existed.
	 *
	 * A type with this default and no axles therefore declares a law its data cannot support,
	 * which FAirframe::EffectiveSteerLaw reports as an error and falls back from. That is the
	 * intended outcome: the behaviour is what it always was, and it is now loud instead of
	 * silent.
	 */
	UPROPERTY(EditAnywhere) ESteerLaw SteerLaw = ESteerLaw::RollingSteer;

	/**
	 * The steered axle in local X, uu. Zero is this class's convention - the origin IS the
	 * nose gear, so there is nothing to offset. See FAirframe::SteerAxleX.
	 */
	UPROPERTY(EditAnywhere) double SteerAxleX = 0.0;

	/** The main-gear axle in local X, uu. Negative on a conforming airframe. */
	UPROPERTY(EditAnywhere) double FixedAxleX = 0.0;

	/**
	 * Distance between the two main wheels, uu. Zero means unmeasured - see
	 * FAirframe::MainGearTrack, which this is carried into by Airframe() below.
	 *
	 * MEASURED FROM THE RIG by the build script, not typed from a datasheet, for the same
	 * reason the axles are: it is a fact about the model that is DRAWN. Tyre smoke hung off
	 * a published figure would sit beside the wheels rather than under them whenever the two
	 * disagreed, and would stay wrong silently after a re-rig.
	 */
	UPROPERTY(EditAnywhere) double MainGearTrack = 0.0;

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
	 * How this type's landing gear retracts, and whether it does at all.
	 *
	 * UNSET ON EVERY TYPE BUT THE 737, and that is a statement about the RIGS rather than
	 * about the aeroplanes. A Dash 8 and a Meridian both retract in reality; neither mesh has
	 * a bone that could show it, and authored data nothing can consume reads as working. Each
	 * gets figures the same day its rig gets gear_* bones, and not before.
	 */
	UPROPERTY(EditAnywhere) FGearPerformance Gear;

	/**
	 * What this type needs of a runway: surface, approach aids, published field lengths.
	 *
	 * Beside the performance structs and copied into the airframe with them, because the
	 * admission check (Model/) holds an FAirframe and nothing else. The field lengths are
	 * admission figures; the rolls the physics derives from Ground and Climb are for motion.
	 */
	UPROPERTY(EditAnywhere) FRunwayRequirements Requirements;

	/**
	 * How long this type occupies a stand, in GAME seconds - see FAirframe::TurnaroundSeconds
	 * for which clock and why, and for why it travels in the bundle rather than being read
	 * from here by the service that needs it.
	 *
	 * Authored per TYPE and not once for the airport, because a widebody and a turboprop turn
	 * round in visibly different times and the player watches both on the same apron.
	 */
	UPROPERTY(EditAnywhere) double TurnaroundSeconds = 1800.0;

	/** How this type gets off a stand - see EPushbackNeed. Carried into FAirframe by
	 *  Airframe() below, because Model/ may not see this header. */
	UPROPERTY(EditAnywhere) EPushbackNeed PushbackNeed = EPushbackNeed::VehicleTug;

	/**
	 * The four performance structs plus Wingspan and Requirements, bundled - see FAirframe
	 * for why.
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
		Out.Gear = Gear;
		Out.Wingspan = Footprint.Wingspan;
		Out.SteerLaw = SteerLaw;
		Out.SteerAxleX = SteerAxleX;
		Out.MainGearTrack = MainGearTrack;
		Out.FixedAxleX = FixedAxleX;

		// DERIVED, not authored: two numbers that must agree are one number. The footprint
		// already says where the nose and tail are, so the centre is arithmetic - and an
		// authored copy would drift the first time a mesh was re-exported, which is exactly
		// how the four position figures in build_plane2_type.py went stale.
		Out.BodyCentreX = (Footprint.NoseX + Footprint.TailX) * 0.5;
		Out.Requirements = Requirements;
		// ShortCode, falling back to Code. Assigning Code alone was the defect: it is the
		// aerodrome letter, so TypeCode could not tell an A320 from a 737.
		Out.TypeCode = ShortCode.IsNone() ? Code : ShortCode;

		// The LOOK travels with the figures - see FAirframe::Mesh for why it lives there.
		Out.Mesh = Mesh;
		Out.AnimClass = AnimClass;
		Out.TurnaroundSeconds = TurnaroundSeconds;
		Out.PushbackNeed = PushbackNeed;
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
	 * comment at the footprint. It is DECLARED rather than merely described: SteerAxleX
	 * carries the measured 2.378 m wheelbase, so the follower and the stands both compose
	 * against it without anything hard-coding the offset.
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

	/**
	 * The Meridian's wingspan on its own, without needing a UAircraftType.
	 *
	 * Same reason PiperMeridianGround exists: UAirsideSettings::ResolveDefaultAirframe's
	 * fallback builds an FAirframe with no UAircraftType to read Footprint.Wingspan from,
	 * and this is the one place the 1311.0 figure is written down, so the fallback and
	 * BuildPiperMeridian's own footprint cannot drift apart the way two copies would.
	 */
	static double PiperMeridianWingspan();

	/**
	 * The Meridian's runway requirements, for the same reason PiperMeridianGround exists.
	 *
	 * Grass, visual, and the POH ground rolls rounded up: 510 m take-off, 400 m landing
	 * (the 50 ft figures include an obstacle the pavement does not have - see the .cpp).
	 * Airside.Model.FieldLengthsCoverTheRoll checks these are never SHORTER than the
	 * rolls the physics derives from Ground and Climb.
	 */
	static FRunwayRequirements PiperMeridianRequirements();
};
