#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Tool/RoadSnap.h"

class URoadNetwork;

/** Whether a hypothetical segment may be built, and if not, why not. */
enum class ERoadPlacement : uint8
{
	Valid,

	/** The start node is not live. Nothing can be built from it. */
	NoStart,

	/** Both ends are the same node. A segment from a node to itself has no direction. */
	SameNode,

	/** A segment already runs between these two nodes. */
	AlreadyJoined,

	/**
	 * Shorter than MinSegmentLength.
	 *
	 * A segment has to be long enough to survive being trimmed back at BOTH ends by its
	 * junctions' fillets. Below that the two cut lines cross and the ribbon inverts.
	 */
	TooShort,

	/**
	 * Turns tighter than MinTurnDegrees against a road already leaving the start node.
	 *
	 * The fillet between two arms grows without bound as the angle between them closes,
	 * so a hairpin either clamps to a stub or eats the whole segment.
	 */
	TooSharp,

	/**
	 * The same corner, at the FAR end: too tight against an arm already at the
	 * destination, or against the segment this click would split.
	 *
	 * A distinct value because the two are fixed differently - the player moves the far
	 * end, not the near one - and because a single reason cannot say which end is at
	 * fault when the ghost is red at both.
	 */
	TooSharpAtEnd,
};

struct FRoadPlacementLimits
{
	/** Shortest segment that may be built, in uu. */
	double MinSegmentLength = 250.0;

	/** Tightest corner allowed against an existing arm at the start node, in degrees. */
	double MinTurnDegrees = 25.0;
};

/**
 * The subset of design spec section 7.5 that a ghost needs to colour itself.
 *
 * Three rules as one function, NOT the Composite of FPlacementValidator that section
 * describes. The Composite earns its place when rules are configured per tool or per
 * surface type - overlap with an existing surface, unauthorised runway crossing - and
 * none of that exists yet. Adding it now would be a framework with three hard-coded
 * leaves. When the fourth rule arrives and the first one needs turning off
 * independently, this becomes the Composite and the call site does not change.
 *
 * Deliberately NOT checked here: whether the fillet actually clamped. Spec 7.5 orders
 * validation after the solve for exactly that reason, and it is the one rule that cannot
 * be answered from the graph alone. What is here is answerable before any solve, which
 * is why the ghost can colour itself without one.
 */
namespace RoadPlacement
{
	/**
	 * Whether a segment may run from From to the snapped point To.
	 *
	 * To is a snap result rather than a bare position so that AlreadyJoined can be
	 * answered: it needs to know the far end is an EXISTING node, which a position alone
	 * cannot say. A Free or Segment snap always lands on a node that does not exist yet
	 * and so can never already be joined.
	 */
	AIRSIDE_API ERoadPlacement Validate(const URoadNetwork& Network, FRoadNodeId From,
		const FRoadSnapResult& To, const FRoadPlacementLimits& Limits);

	/** Short player-facing reason, for the overlay and the log. */
	AIRSIDE_API const TCHAR* Describe(ERoadPlacement Result);
}
