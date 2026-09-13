#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Tool/RoadSnap.h"
#include "RoadPlacement.generated.h"

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

	/**
	 * A corner at either end whose arms are too short to hold it: the junction's cut, even
	 * with no fillet at all, would reach past the allowance of one of the segments. The
	 * solver would then fail that node and draw nothing from it (2026-09-06, the road that
	 * vanished at an acute angle), so it is refused here where the ghost can say why.
	 */
	TooShortForCorner,
};

/**
 * PER-AIRPORT, not per-driver, like FRoadSnapSettings - see that struct's own comment and
 * issue #93. `ARoadNetworkActor::PlacementLimits` is the one UPROPERTY(EditAnywhere) copy of
 * MinSegmentLength/MinTurnDegrees both drivers build their `FBuildSessionTunables` from, via
 * `ARoadNetworkActor::MakeTunables`. Before this, the editor tool never set either one at
 * all - a corner PIE would refuse as TooSharp the editor mode would happily draw.
 */
USTRUCT(BlueprintType)
struct AIRSIDE_API FRoadPlacementLimits
{
	GENERATED_BODY()

	/** Shortest segment that may be built, in uu. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0"))
	double MinSegmentLength = 250.0;

	/** Tightest corner allowed against an existing arm at the start node, in degrees. */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", ClampMax = "180.0"))
	double MinTurnDegrees = 25.0;

	/**
	 * Half-width of the road being drawn, uu, for the corner-fit check. 0 disables that
	 * check, which is what a caller with no profile in hand gets and what every test that
	 * predates the check relies on.
	 *
	 * NOT a UPROPERTY: this is resolved fresh from whichever road profile is about to be
	 * drawn (see ARoadNetworkActor::MakeTunables), never authored - a value saved into a
	 * level here would silently outlive the profile it was measured from.
	 */
	double NewRoadHalfWidth = 0.0;
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

	/**
	 * Would every corner at Node, and every corner at its neighbours that its arms take
	 * part in, still fit if Node stood at Position?
	 *
	 * The drag path: Validate judges a segment before it exists, but moving a node changes
	 * the corners at that node AND at the far end of each incident segment, none of which
	 * Validate sees. The facade asks this before accepting a move, so a drag cannot create
	 * the corner the solver would then fail. True for a node with no arms.
	 */
	AIRSIDE_API bool NodeCornersFit(const URoadNetwork& Network, FRoadNodeId Node, const FVector2D& Position);
}
