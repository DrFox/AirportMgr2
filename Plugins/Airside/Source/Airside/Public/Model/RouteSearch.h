#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "RouteSearch.generated.h"

class URoadNetwork;

/**
 * Why a route query answered the way it did.
 *
 * A bare "no route" is the least useful thing a pathfinder can say to someone building an
 * airport, because every cause has a different fix: connect the taxiway, reverse the
 * one-way, or bring a smaller aircraft. These are the distinctions the tool puts on screen.
 */
UENUM()
enum class ERouteResult : uint8
{
	Found,

	/** The start handle names no live node. */
	NoStart,

	/** The goal handle names no live node. */
	NoGoal,

	/** Start and goal are the same node. Not a failure of the graph, but not a route. */
	SameNode,

	/** The graph is connected wrongly, or not at all, for this traversal class. */
	Unreachable,

	/**
	 * Reachable, but only over guidelines this wingspan is too wide for.
	 *
	 * Distinguished from Unreachable by re-running the search unconstrained ONLY after a
	 * failure, so the common case pays nothing. Without it a Code F aircraft aimed at a
	 * Code C stand reports the same thing as a taxiway nobody ever joined up.
	 */
	TooWide,
};

/** One edge of a found route, in traversal order. */
USTRUCT()
struct AIRSIDE_API FRouteStep
{
	GENERATED_BODY()

	UPROPERTY() FGuidelineEdgeId Edge;

	/** The node this step arrives at, so the route reads forwards without re-deriving it. */
	UPROPERTY() FGuidelineNodeId To;

	/** True when the edge is traversed B to A, which is what reverses its sampled points. */
	UPROPERTY() bool bReversed = false;

	/**
	 * Cumulative route distance at which this step's edge ends, and the index of that point
	 * in FRoutePlan::Polyline. Filled by RunSearch from the SAME polyline it appends - never
	 * from the Bezier - so "which edge am I on at Travelled" is answered off the array the
	 * follower walks. A step map derived from the curve would disagree on every bend, and
	 * the arbiter would then stop an agent for a node it had already crossed.
	 */
	UPROPERTY() double EndDistance = 0.0;
	UPROPERTY() int32 EndVertex = 0;
};

/**
 * The answer to one route query: what to draw, what to drive, and why not.
 *
 * Polyline is the point of this struct. The overlay draws it and the follower walks it -
 * the SAME array, never two evaluations of one curve - so a cube physically cannot leave
 * the line the player was shown. See GuidelineGeom.
 */
USTRUCT()
struct AIRSIDE_API FRoutePlan
{
	GENERATED_BODY()

	UPROPERTY() ERouteResult Result = ERouteResult::NoStart;

	UPROPERTY() FGuidelineNodeId Start;

	UPROPERTY() TArray<FRouteStep> Steps;

	/** Start to goal, welded across edges: consecutive edges contribute one shared point. */
	UPROPERTY() TArray<FVector2D> Polyline;

	UPROPERTY() double Length = 0.0;

	bool IsValid() const { return Result == ERouteResult::Found; }
};

struct FTrafficOccupancy;

/** What is being routed, and what it is allowed to use. */
USTRUCT()
struct AIRSIDE_API FRouteQuery
{
	GENERATED_BODY()

	UPROPERTY() FGuidelineNodeId Start;

	UPROPERTY() FGuidelineNodeId Goal;

	UPROPERTY() ETraversalClass Class = ETraversalClass::GroundVehicle;

	/**
	 * Wingspan in uu, or 0 for unconstrained.
	 *
	 * Compared against FGuidelineEdge::MaxWingspan, where 0 means UNLIMITED - so the test
	 * is not a plain >, and getting it backwards routes a widebody down a link that cannot
	 * take it.
	 */
	UPROPERTY() double Wingspan = 0.0;

	/**
	 * An edge the search may not use. Set by a deadlock replan to forbid the edge the agent
	 * was refused. One edge, not a set: the resolver bans exactly the thing it is stuck on
	 * and lets the occupancy cost steer round the rest.
	 */
	UPROPERTY() FGuidelineEdgeId BannedEdge;

	/**
	 * Who holds what, for the congestion cost term - or null for a plain shortest route,
	 * which is bitwise the search this class ran before occupancy existed. A raw pointer
	 * rather than a UPROPERTY: a query lives on the stack for one call and the table it
	 * reads outlives it.
	 */
	const FTrafficOccupancy* Occupancy = nullptr;

	/** The agent asking, so its own claims do not cost it. 0 when nobody is. */
	UPROPERTY() int32 QueryingAgent = 0;

	/** Weight on held length. Ignored when Occupancy is null. */
	UPROPERTY() double CongestionWeight = 2.0;
};

/**
 * Shortest route over the guideline graph, by A*.
 *
 * NOT in Solve/ and not behind a graph adapter, unlike the junction solver. That solver is
 * pure because its maths stands alone and is reusable; this is a query over URoadNetwork's
 * own handles, incidence and traffic masks, with no second consumer in sight. An adapter
 * interface here would exist to satisfy a rule rather than a need, and the model is
 * already testable without a world - every model test simply NewObjects a network.
 * A NAMED deviation, on those grounds.
 *
 * The heuristic is straight-line distance to the goal, which is admissible because an
 * edge's cost is its sampled polyline length and a polyline is never shorter than the
 * chord between its ends. So the first time the goal is popped, it is optimal.
 */
namespace RouteSearch
{
	AIRSIDE_API FRoutePlan Find(const URoadNetwork& Network, const FRouteQuery& Query);

	/**
	 * Head's first KeepSteps steps followed by all of Tail, welded at the node Tail starts
	 * from. Precondition: Tail.Start is the node Head's KeepSteps-th step arrives at (or
	 * Head.Start when KeepSteps is 0). EndDistance and EndVertex are re-based so the
	 * follower and the arbiter keep reading one set of numbers. Returns an invalid plan
	 * when the precondition fails, because splicing two lines that do not meet would put a
	 * jump in the polyline the agent would then drive across.
	 */
	AIRSIDE_API FRoutePlan Splice(const FRoutePlan& Head, int32 KeepSteps, const FRoutePlan& Tail);

	/**
	 * The nearest guideline node to a world position that this class could actually use.
	 *
	 * Nodes with no edge admitting the class are skipped rather than returned and refused
	 * later: an aircraft-only node is not a sensible start for a van, and offering it as a
	 * snap only to fail the search afterwards reads as a broken pathfinder rather than as
	 * a node the van was never entitled to.
	 *
	 * Returns an unset handle when nothing qualifies within MaxDistance.
	 */
	AIRSIDE_API FGuidelineNodeId FindNearestNode(
		const URoadNetwork& Network, const FVector2D& Position,
		ETraversalClass Class, double MaxDistance);
}
