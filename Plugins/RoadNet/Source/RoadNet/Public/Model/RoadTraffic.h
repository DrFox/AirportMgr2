#pragma once

#include "CoreMinimal.h"
#include "RoadTraffic.generated.h"

/**
 * How a thing MOVES - not what it does.
 *
 * A refuel truck, a baggage cart and a catering van are all GroundVehicle: they obey
 * identical movement rules and differ only in job. What they do is a service role, on an
 * entity anchor, and it is deliberately not this enum - otherwise this grows with every
 * vehicle type in the game and gets consulted by pathfinding for no reason.
 */
UENUM()
enum class ETraversalClass : uint8
{
	Aircraft,
	GroundVehicle,
	Pedestrian,
	Emergency
};

/**
 * Which traversal classes a guideline admits.
 *
 * Defaults to EMPTY, admitting nobody. A guideline whose access was never set is then a
 * visible dead end rather than a silent free-for-all - the failure that is easy to see
 * beats the failure that routes a 747 down a footpath.
 */
USTRUCT()
struct ROADNET_API FTrafficMask
{
	GENERATED_BODY()

	UPROPERTY() uint8 Bits = 0;

	bool Allows(ETraversalClass Class) const
	{
		return (Bits & (1u << static_cast<uint8>(Class))) != 0;
	}

	void Add(ETraversalClass Class)
	{
		Bits |= static_cast<uint8>(1u << static_cast<uint8>(Class));
	}

	static FTrafficMask All()
	{
		FTrafficMask Mask;
		Mask.Add(ETraversalClass::Aircraft);
		Mask.Add(ETraversalClass::GroundVehicle);
		Mask.Add(ETraversalClass::Pedestrian);
		Mask.Add(ETraversalClass::Emergency);
		return Mask;
	}

	static FTrafficMask Only(ETraversalClass Class)
	{
		FTrafficMask Mask;
		Mask.Add(Class);
		return Mask;
	}
};

/** Which way a guideline may be traversed. */
UENUM()
enum class EGuidelineDir : uint8
{
	Bidirectional,
	AToB,
	BToA
};

/**
 * Right-of-way rank. Higher wins. Spec 5.4:
 *
 *     Emergency > Aircraft > Pedestrian > GroundVehicle
 *
 * On the class rather than on the edge or the crossing, so the common case needs no
 * authoring at all. Authoring priority per crossing would be laborious AND wrong the
 * moment a fire truck arrives - the fire truck's precedence is a fact about fire trucks,
 * not about any particular junction.
 *
 * Every value is distinct, which makes the order total: no crossing can be reached that
 * has no defined winner.
 */
ROADNET_API int32 TraversalPriority(ETraversalClass Class);

/** Which of two contending classes proceeds. Returns the class itself when they match. */
ROADNET_API ETraversalClass ResolveRightOfWay(ETraversalClass A, ETraversalClass B);
