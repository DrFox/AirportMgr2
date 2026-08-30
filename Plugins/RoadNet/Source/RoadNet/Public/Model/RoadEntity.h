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

/** A connection point between an entity and the guideline graph, in the entity's local space. */
USTRUCT()
struct ROADNET_API FEntityAnchor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FVector2D LocalPosition = FVector2D::ZeroVector;

	/** Radians, relative to the entity's own heading. */
	UPROPERTY(EditAnywhere) double LocalHeading = 0.0;

	UPROPERTY(EditAnywhere) EServiceRole Role = EServiceRole::Aircraft;
};

/**
 * A placed entity. The Flyweight instance: pose plus a shared definition.
 *
 * ResolvedAnchors is parallel to Definition->Anchors, and holds a guideline node per
 * anchor. Those nodes are created NON-DERIVED, so FRoadGuidelineBuilder's orphan sweep
 * never touches them and these handles stay valid across every rebuild - which is what
 * makes "drive to stand 12's cart position" an ordinary path query rather than a lookup
 * that goes stale the moment anyone edits a taxiway.
 */
USTRUCT()
struct ROADNET_API FEntityInstance
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Radians. */
	UPROPERTY() double Heading = 0.0;

	UPROPERTY() TObjectPtr<UEntityDefinition> Definition = nullptr;

	/** Parallel to Definition->Anchors. */
	UPROPERTY() TArray<FGuidelineNodeId> ResolvedAnchors;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
