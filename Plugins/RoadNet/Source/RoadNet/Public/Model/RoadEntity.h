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
struct ROADNET_API FEntityFootprint
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
 * Distances are uu per second, and a uu is a centimetre.
 */
USTRUCT(BlueprintType)
struct ROADNET_API FTaxiPerformance
{
	GENERATED_BODY()

	/**
	 * Normal taxi speed on a straight. 1000 is 10 m/s, a brisk taxi.
	 *
	 * The defaults on this struct are a deliberately GENERIC airframe, not any real one.
	 * A real type's figures live in its builder - see UAircraftType::PiperMeridianTaxi -
	 * so that no aircraft's numbers are written down in two places and free to drift.
	 * They are non-zero rather than zero so a caller that forgets to author them gets an
	 * aircraft that moves sluggishly, which is visible, rather than one that never turns.
	 */
	UPROPERTY(EditAnywhere) double TaxiSpeed = 1000.0;

	/**
	 * The slowest this type can be kept rolling - NOT zero, and that is the point.
	 *
	 * A wheeled aircraft cannot yaw without rolling: a prop or a fan produces thrust along
	 * the airframe, and a nosewheel steers the direction that thrust is taken in. It has no
	 * way to pivot on the spot. So a turn that cannot be made at speed is made at a crawl,
	 * which is what a pilot riding the brakes against idle thrust actually does.
	 *
	 * Without this floor the speed law below reaches zero at a sharp corner and the
	 * aircraft stops dead and spins - the one thing an aeroplane provably cannot do.
	 */
	UPROPERTY(EditAnywhere) double MinTaxiSpeed = 50.0;

	/**
	 * How fast the nose can be swung, in DEGREES per second.
	 *
	 * Degrees because this is authored and read by hand, matching the convention the
	 * service-point builders use; FRouteFollower converts once, on Start.
	 *
	 * It is v/R at the tightest turn the steering allows, taken at the speed a pilot would
	 * take it - so it is derived from two published figures rather than dialled in until it
	 * looked right. See BuildPiperMeridian for that arithmetic.
	 */
	UPROPERTY(EditAnywhere) double MaxTurnRateDegPerSec = 10.0;

	/** False when nothing was authored, so a caller can fall back rather than freeze an agent. */
	bool IsSet() const
	{
		return TaxiSpeed > 0.0 && MinTaxiSpeed > 0.0 && MaxTurnRateDegPerSec > 0.0;
	}
};

/** A connection point between an entity and the guideline graph, in the entity's local space. */
USTRUCT(BlueprintType)
struct ROADNET_API FEntityAnchor
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
struct ROADNET_API FResolvedAnchor
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
struct ROADNET_API FEntityInstance
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
ROADNET_API ETraversalClass TraversalForRole(EServiceRole Role);
