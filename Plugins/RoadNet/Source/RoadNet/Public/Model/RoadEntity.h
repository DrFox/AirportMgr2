#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
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
UENUM()
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
USTRUCT()
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

/** A connection point between an entity and the guideline graph, in the entity's local space. */
USTRUCT()
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

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
