#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "RoadEntity.generated.h"

class UEntityDefinition;

/**
 * What a thing DOES at an anchor - not how it moves.
 *
 * Deliberately separate from ETraversalClass. A fuel truck and a baggage cart obey
 * identical movement rules and are both GroundVehicle to the network; they differ only in
 * the job they come to do, which is this. Keeping the two apart is what lets this list
 * grow with every vehicle type in the game without pathfinding ever consulting it.
 */
UENUM(BlueprintType)
enum class EServiceRole : uint8
{
	Aircraft,
	Fuel,
	Baggage,
	Tug,
	GPU,
	Passenger,
	Crew
};

/**
 * Plan-view extent of the thing a definition describes, in its own local space.
 *
 * Here rather than in the overlay because it is a fact about an A320, not a drawing
 * choice: an overlay that carried its own dimensions would be a second opinion about how
 * big the aircraft is, and the two could disagree without anything reporting it. It is
 * also what a stand's painted boundary is derived from.
 *
 * All distances are local X or spans in uu, on the same axes as the anchors: +X forward,
 * +Y starboard, origin at whatever the definition measures from.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FEntityFootprint
{
	GENERATED_BODY()

	/** Nose, forward of the origin. */
	UPROPERTY(EditAnywhere) double NoseX = 0.0;

	/** Tail, normally behind the origin and so negative. */
	UPROPERTY(EditAnywhere) double TailX = 0.0;

	/** Wingtip to wingtip. */
	UPROPERTY(EditAnywhere) double Wingspan = 0.0;

	/** Where the wing crosses the centreline. */
	UPROPERTY(EditAnywhere) double WingX = 0.0;

	UPROPERTY(EditAnywhere) double TailplaneSpan = 0.0;

	UPROPERTY(EditAnywhere) double TailplaneX = 0.0;

	/** False when nothing has been authored, so callers can skip drawing rather than draw a dot. */
	bool IsSet() const { return Wingspan > 0.0 && NoseX > TailX; }
};

/**
 * One POWER SETTING: what the airframe does when it is being flown a particular way.
 *
 * Taxi, take-off and landing are three different machines as far as motion is concerned -
 * an aircraft that accelerates onto a runway at its taxi rate never gets airborne - so the
 * numbers are grouped by the regime they belong to rather than flattened onto the airframe
 * where a caller would have to remember which one it was holding.
 *
 * ONLY TAXI EXISTS TODAY, and deliberately. A Takeoff or Landing field here would be
 * authored numbers that nothing reads, which is the failure this codebase has shipped three
 * times over - see CLAUDE.md. They become fields the day something flies, and the shape is
 * already right for that.
 *
 * Braking is here alongside thrust because both are "how fast can this change speed", and
 * the two are not equal: wheel brakes beat a propeller, which is why Decel is the larger.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FGroundRegime
{
	GENERATED_BODY()

	/** How hard it can speed up, uu per second squared. 100 is 1 m/s2. */
	UPROPERTY(EditAnywhere) double Accel = 100.0;

	/** How hard it can slow down. Larger than Accel: brakes beat thrust. */
	UPROPERTY(EditAnywhere) double Decel = 200.0;

	/** The speed this regime works up to, uu per second. 1000 is 10 m/s. */
	UPROPERTY(EditAnywhere) double SpeedCap = 1000.0;

	bool IsSet() const { return Accel > 0.0 && Decel > 0.0 && SpeedCap > 0.0; }
};

/**
 * Everything the VIEW needs to show an agent: where it is, and what it is doing.
 *
 * SetPose used to take a position, a heading, a surface height, an altitude and a pitch, and
 * animation wants three more - how fast the wheels are turning, whether the propeller is
 * running, whether the gear is still on the ground. Eight arguments in a row is a signature
 * nobody can call correctly, and the two that mattered were already easy to swap.
 *
 * It also keeps the view dumb, which is the point. ARoadAgentActor decides nothing: a wheel
 * turns at GroundSpeed over its radius because that is arithmetic, not judgement, and every
 * value here is one the model already holds - see FRouteFollower and FTakeoffRun.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FAgentMotion
{
	GENERATED_BODY()

	/** Road-plane XY. */
	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Radians, yaw from +X. */
	UPROPERTY() double Heading = 0.0;

	/** Above the road surface, uu. Zero on the wheels. */
	UPROPERTY() double Altitude = 0.0;

	/** Nose-up, degrees. */
	UPROPERTY() double PitchDegrees = 0.0;

	/**
	 * Speed over the ground, uu per second.
	 *
	 * Drives the wheels, so it stays meaningful in the air: a wheel that stopped the instant
	 * the aircraft lifted off would snap from spinning to still in one frame, which is the
	 * one thing real wheels visibly do not do.
	 */
	UPROPERTY() double GroundSpeed = 0.0;

	/** The propeller turns. True whenever the aircraft is under way at all. */
	UPROPERTY() bool bEngineRunning = false;

	/** Off the wheels. Stage 2's gear retraction hangs on this. */
	UPROPERTY() bool bAirborne = false;
};

/**
 * What the aircraft does once the wheels leave the ground.
 *
 * SEPARATE FROM FGroundPerformance, which is named for what it describes and would start
 * lying the moment a climb rate was added to it. The two are consumed by different things:
 * the follower and the take-off roll read the ground, and only the climb reads this.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FClimbPerformance
{
	GENERATED_BODY()

	/**
	 * The angle of attack this wing needs to fly AT ROTATION SPEED, degrees.
	 *
	 * THE ONE NUMBER THE WHOLE DEPARTURE TURNS ON, because the lift a wing makes goes as the
	 * square of speed - so the angle it needs to hold the aircraft up falls as the square of
	 * speed too:
	 *
	 *     required(V) = this * (Vr / V)^2
	 *
	 * During the rotation the nose is coming UP while that required angle is coming DOWN, and
	 * the aircraft flies at the moment they cross. That is what puts the rotation on the
	 * runway where it belongs, and it makes lift-off fall out of the roll rather than being
	 * declared at a distance or a stopwatch reading.
	 */
	UPROPERTY(EditAnywhere) double LiftAngleAtRotateDegrees = 8.0;

	/**
	 * Nose-up attitude held once established, degrees.
	 *
	 * Held, not achieved-and-forgotten: the climb rate that comes out of it changes as the
	 * aircraft accelerates, because the flight path is pitch MINUS the angle the wing is
	 * currently using, and the second of those keeps falling.
	 */
	UPROPERTY(EditAnywhere) double ClimbPitchDegrees = 11.3;

	/** How fast the nose comes up at rotation, degrees per second. */
	UPROPERTY(EditAnywhere) double RotateRateDegPerSec = 4.0;

	/** Best-rate-of-climb speed, uu per second. 6200 is 120 KIAS. */
	UPROPERTY(EditAnywhere) double ClimbSpeed = 6200.0;

	/** Height above the surface at which the departure is over and the agent goes, uu. */
	UPROPERTY(EditAnywhere) double ClearAltitude = 30000.0;

	/**
	 * The angle this wing needs at V to hold the aircraft up, degrees.
	 *
	 * Vr is passed in rather than stored because it belongs to the take-off regime, not to
	 * the climb - see FGroundRegime::SpeedCap.
	 */
	double RequiredAngleAt(double Speed, double RotateSpeed) const
	{
		if (Speed <= 0.0)
		{
			return LiftAngleAtRotateDegrees;
		}
		return LiftAngleAtRotateDegrees * FMath::Square(RotateSpeed / Speed);
	}

	/**
	 * Rate of climb at V holding this attitude, uu per second. NOT a setting.
	 *
	 * The published figure for an airframe is a rate at a speed, and it comes OUT of this
	 * rather than going in - so a climb that is too fast or too slow is an argument with the
	 * pitch or the speed, which are the things a pilot actually has.
	 */
	double ClimbRateAt(double Speed, double RotateSpeed) const
	{
		const double Gamma = ClimbPitchDegrees - RequiredAngleAt(Speed, RotateSpeed);
		return Speed * FMath::Sin(FMath::DegreesToRadians(Gamma));
	}

	bool IsSet() const
	{
		return LiftAngleAtRotateDegrees > 0.0 && ClimbPitchDegrees > 0.0
			&& RotateRateDegPerSec > 0.0 && ClimbSpeed > 0.0 && ClearAltitude > 0.0;
	}
};

/**
 * How an airframe MOVES on the ground. A property of the aircraft, never of the pavement.
 *
 * The distinction matters and this project has already had to make it once, the other way
 * round: painted geometry - a stand's lead-in sweep, a taxiway fillet - is sized for the
 * LARGEST type admitted and is deliberately not per-aircraft, because concrete cannot be
 * repoured per movement. A turn RATE is the opposite. Nothing about the taxiway decides
 * how fast a nosewheel can be slewed; the airframe does, and an A320 and a Piper differ by
 * a factor of two and a half.
 *
 * Here in Model/ rather than on UAircraftType so FRouteFollower can take one. Entities/
 * depends on Model/ and never the reverse - the same reason FEntityFootprint lives here.
 *
 * Distances are uu, and a uu is a centimetre.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FGroundPerformance
{
	GENERATED_BODY()

	/** Moving about the airport. */
	UPROPERTY(EditAnywhere) FGroundRegime Taxi;

	/**
	 * Full power down a runway. SpeedCap here is ROTATION SPEED - the speed at which the
	 * nose comes up - which is exactly what "the speed this regime works up to" means for a
	 * take-off roll.
	 *
	 * Decel is the ABORT: a rejected take-off is braking from near Vr, which is harder than
	 * anything a taxi asks for.
	 */
	UPROPERTY(EditAnywhere) FGroundRegime Takeoff;

	/**
	 * The slowest this type can be kept rolling WHILE STEERING - NOT zero, and that is the
	 * point.
	 *
	 * A wheeled aircraft cannot yaw without rolling: a prop or a fan produces thrust along
	 * the airframe, and a nosewheel steers the direction that thrust is taken in. It has no
	 * way to pivot on the spot. So a turn that cannot be made at speed is made at a crawl,
	 * which is what a pilot riding the brakes against idle thrust actually does.
	 *
	 * Not a floor on speed in general: an aircraft parked at its destination is stopped.
	 * This bounds only what a TURN may slow it to.
	 */
	UPROPERTY(EditAnywhere) double MinTaxiSpeed = 50.0;

	/**
	 * How fast the nose can be swung, in DEGREES per second.
	 *
	 * Outside the regime because it is a fact about the STEERING rather than about a power
	 * setting - the same nosewheel, at the same speed, slews at the same rate whatever the
	 * throttle is doing.
	 *
	 * Degrees because this is authored and read by hand, matching the convention the
	 * service-point builders use; FRouteFollower converts once, on Start.
	 *
	 * It is v/R at the tightest turn the steering allows, taken at the speed a pilot would
	 * take it - so it is derived from two published figures rather than dialled in until it
	 * looked right. See UAircraftType::PiperMeridianGround for that arithmetic.
	 */
	UPROPERTY(EditAnywhere) double MaxTurnRateDegPerSec = 10.0;

	/** False when nothing was authored, so a caller can fall back rather than freeze an agent. */
	bool IsSet() const
	{
		return Taxi.IsSet() && MinTaxiSpeed > 0.0 && MaxTurnRateDegPerSec > 0.0;
	}
};

/** A connection point between an entity and the guideline graph, in the entity's local space. */
USTRUCT(BlueprintType)
struct AIRSIDE_API FEntityAnchor
{
	GENERATED_BODY()

	/**
	 * Stable identity, and the ONLY way an anchor should be addressed.
	 *
	 * Not the array index, which shifts the moment a definition gains an anchor and leaves
	 * every instance placed before it addressing the wrong thing. Not the role either: a
	 * stand has two belt loaders, so Baggage names a category, not a position.
	 *
	 * An instance that predates a new anchor simply does not carry this id, and a lookup
	 * for it misses - which is a correct answer, where an index read the same information
	 * out of bounds.
	 */
	UPROPERTY(EditAnywhere) FName Id;

	UPROPERTY(EditAnywhere) FVector2D LocalPosition = FVector2D::ZeroVector;

	/** Radians, relative to the entity's own heading. */
	UPROPERTY(EditAnywhere) double LocalHeading = 0.0;

	UPROPERTY(EditAnywhere) EServiceRole Role = EServiceRole::Aircraft;
};

/**
 * One anchor of a placed entity, resolved to a node in the guideline graph.
 *
 * Carries the id it resolved FROM, so nothing has to be parallel to anything. This
 * replaces a bare TArray<FGuidelineNodeId> indexed in lockstep with the definition's
 * anchors - an invariant nothing enforced, and which the natural iterate-the-definition,
 * index-the-instance pattern broke by reading out of bounds the moment a saved definition
 * gained an anchor. Ordinary designer work, not an edge case.
 */
USTRUCT()
struct AIRSIDE_API FResolvedAnchor
{
	GENERATED_BODY()

	UPROPERTY() FName Id;

	UPROPERTY() FGuidelineNodeId Node;
};

/**
 * A placed entity. The Flyweight instance: pose plus a shared definition.
 *
 * ResolvedAnchors holds a guideline node per anchor the definition declared AT THE TIME IT
 * WAS PLACED, each tagged with the id it came from. Those nodes are created NON-DERIVED, so
 * FRoadGuidelineBuilder's orphan sweep never touches them and these handles stay valid
 * across every rebuild - which is what makes "drive to stand 12's cart position" an
 * ordinary path query rather than a lookup that goes stale the moment anyone edits a
 * taxiway.
 */
USTRUCT()
struct AIRSIDE_API FEntityInstance
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Radians. */
	UPROPERTY() double Heading = 0.0;

	UPROPERTY() TObjectPtr<UEntityDefinition> Definition = nullptr;

	/** Keyed by anchor id, never by position in the definition's array. */
	UPROPERTY() TArray<FResolvedAnchor> ResolvedAnchors;

	/**
	 * The entity's OWN pose as a guideline node - for a stand, the nose gear stop.
	 *
	 * Not an anchor, and deliberately not in the array above. An anchor is a FIXTURE: a
	 * hydrant pit, a ground power point, a painted equipment box - something dug into or
	 * painted onto the concrete. There is nothing at the nose gear mark except paint, and
	 * a definition that declared one would be claiming a Code C stand has a piece of plant
	 * where the aircraft parks. RoadEntityTest asserts that invariant directly.
	 *
	 * But an aircraft routed to this stand has to be routed SOMEWHERE, and that somewhere
	 * is the stop position. So it gets a node of its own, named for what it is, rather than
	 * being smuggled into the fixture list to make pathfinding easier.
	 *
	 * Non-derived, like the anchor nodes, so the rebuild sweep leaves it alone.
	 */
	UPROPERTY() FGuidelineNodeId PoseNode;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};

/**
 * How a thing that comes to do this job MOVES.
 *
 * The two enums are deliberately separate - see EServiceRole - but a lead-in still has to
 * decide which traffic its guideline admits, and that decision has exactly one right
 * answer per role. Here rather than at each call site, so a fuel truck and a belt loader
 * cannot end up classified differently by two functions that both looked obvious.
 */
AIRSIDE_API ETraversalClass TraversalForRole(EServiceRole Role);
