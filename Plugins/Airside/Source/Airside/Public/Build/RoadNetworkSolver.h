#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;
struct FChassis;
struct FRoadDesignVehicles;

/**
 * Whether a solve may DRIVE a bend's design vehicle to widen its inside (BendWidening), or only
 * read what an earlier one traced. A PLAIN enum (nothing reflects it). Review of 75d3cbc0: the
 * trace is milliseconds per bend, and the snap's claim, cut and reach queries run per cursor move
 * and the ghost's solve per drag frame - so only a Topology rebuild traces, and everything else
 * reads its answer, keyed by the bend's geometry.
 * ENFORCED BY: Airside.Build.BendLanes.SnapAndDragDoNotTrace
 */
enum class EWideningTrace : uint8
{
	/** Trace any bend whose geometry or design vehicle the cache has not seen: a Topology rebuild. */
	Trace,
	/** Read the cache only; a bend it has not seen is laid unwidened until the next Topology rebuild. */
	ReadCached
};

/**
 * A bend whose inside widening a short arm capped (BendWidening): its design vehicle still leaves
 * the tarmac there. Collected by the solve, said once per rebuild by SolveAll.
 */
struct FCappedWidening
{
	int32 NodeIndex = INDEX_NONE;
	FVector2D Position = FVector2D::ZeroVector;
	/** How much of the traced widening the cap cost, uu, and how far the vehicle still leaves the tarmac (that less the margin). */
	double Missing = 0.0;
	double Overrun = 0.0;
	/** The capped arm's segment, its length, and the length that would hold the widening. */
	FRoadSegmentId Segment;
	double Length = 0.0;
	double LengthNeeded = 0.0;
};

/** Every node's solved boundary, keyed by FRoadNodeId::Index. */
struct FRoadSolveResult
{
	/** Bends whose widening a short arm capped - see FCappedWidening. */
	TArray<FCappedWidening> CappedWidenings;

	TMap<int32, FJunctionResult> NodeResults;

	/**
	 * Each solved node's arms, in the same order as that node's FJunctionResult::Arms,
	 * naming the segment each arm belongs to.
	 *
	 * Published rather than left for callers to re-derive. Rebuilding it means walking
	 * Node.Incident and re-applying SolveAll's skip rule, and any divergence writes one
	 * arm's geometry onto another arm's segment - silently.
	 */
	TMap<int32, TArray<FRoadSegmentId>> NodeArmSegments;

	int32 SolvedNodes = 0;
	int32 FailedNodes = 0;
};

/**
 * One node's solved cuts, before SolveBoundary and before anything is written back.
 *
 * Input and ArmSegments are index-parallel and are published together for the same reason
 * FRoadSolveResult::NodeArmSegments is: rebuilding either means re-applying the skip rule
 * that drops a dangling segment, and any divergence silently attributes one arm's geometry
 * to another.
 */
struct FRoadNodeCuts
{
	FJunctionInput Input;
	FJunctionResult Result;
	TArray<FRoadSegmentId> ArmSegments;
	/** Set when this node's inside widening was capped by a short arm (NodeIndex INDEX_NONE otherwise). */
	FCappedWidening Capped;
};

/**
 * Walks a URoadNetwork, solves every live node, and writes each segment's trim
 * distances AND its four cut vertices back into the model.
 *
 * This is the only writer of FRoadSegment::TrimA/TrimB and the cut vertices. It lives
 * in Build/ rather than Solve/ because it touches UObjects, and Solve/ must stay free
 * of engine dependencies so its tests can run without a World.
 */
class AIRSIDE_API FRoadNetworkSolver
{
public:
	/**
	 * Each end's share of a segment's slack (its length less both ends' floors). UNDER a
	 * half, so two ends that each take their whole share still leave a ribbon between them:
	 * at exactly a half the cut centres would touch and the ribbon would have no length.
	 * Public so FRoadGuidelineBuilder's capped-taper warning names a segment length from the
	 * same figure that capped it.
	 */
	static constexpr double SlackShare = 0.45;

	/**
	 * DesignVehicles IS OPTIONAL, and null means "resolve it yourself" - see
	 * BuildNodeInput's own comment (issue #190). A caller mid-rebuild (URoadSurfacePresenter)
	 * has already resolved it once and passes the answer down every arm of every node reads;
	 * every other caller - every test in this plugin, the debug gallery - keeps asking each
	 * profile to resolve its own, exactly as before this parameter existed.
	 */
	static FRoadSolveResult SolveAll(URoadNetwork& Network, int32 ArcSegments = 12,
		const FRoadDesignVehicles* DesignVehicles = nullptr, EWideningTrace Widening = EWideningTrace::Trace);

	/**
	 * Solve ONE node's cut distances, writing nothing back to the model.
	 *
	 * False when the node is not live or has no arms. Read-only and const, so a tool may
	 * call it while the cursor moves - SolveAll cannot be used that way, because it takes
	 * the network mutably and rewrites every segment's trims as a side effect.
	 *
	 * SolveAll is built on this. That is the point: the arm gathering, the skip rule and
	 * the fillet clamping live in exactly one place, so the distance a build tool believes
	 * a junction reaches and the distance the mesh actually paves cannot drift apart.
	 */
	static bool SolveNodeCuts(const URoadNetwork& Network, int32 NodeIndex, int32 ArcSegments,
		FRoadNodeCuts& Out, const FRoadDesignVehicles* DesignVehicles = nullptr,
		EWideningTrace Widening = EWideningTrace::ReadCached);

	/** How many bend lane turns the widening has DRIVEN (VehicleSweep::Drive) since the reset - see EWideningTrace. */
	static int32 WideningTraceCountForTest;
	static void ResetWideningTraceCountForTest() { WideningTraceCountForTest = 0; }

	/**
	 * SolveAll's own per-node body, exposed for exactly ONE node: solve its cuts, solve its
	 * boundary, and write the result back into the model AND into InOutResult - the same
	 * writes SolveAll makes for this node, because this IS the code SolveAll's loop runs
	 * (#166).
	 *
	 * EXISTS so a caller that only cares about a HANDFUL of nodes - the ghost preview solves
	 * exactly the segment it draws and the two junctions it reshapes - is not made to
	 * re-solve and rewrite every OTHER junction in the network to get them. Calling this for
	 * every live node, in order, is what SolveAll now does; calling it for two is the
	 * ghost's whole solve. Either way there is exactly one place deciding what a junction
	 * writes - a second copy of this loop body is a second place that could decide where
	 * pavement stops and drift from the first.
	 *
	 * Leaves InOutResult and the model untouched when NodeIndex is not live or has no arms,
	 * mirroring SolveAll's own `continue` for that case - so a caller may call this for a
	 * node it is not sure is still live without checking first.
	 */
	static void SolveNodeInto(URoadNetwork& Network, int32 NodeIndex, int32 ArcSegments,
		FRoadSolveResult& InOutResult, const FRoadDesignVehicles* DesignVehicles = nullptr,
		EWideningTrace Widening = EWideningTrace::ReadCached);

	/**
	 * How far a node's pavement reaches from its centre, in uu. Zero when it has no arms.
	 *
	 * Deliberately CONSERVATIVE: an arm's furthest pavement corner is at
	 * sqrt(Cut^2 + HalfWidth^2), and this returns Cut + HalfWidth, which is never smaller.
	 * A reach that slightly exceeds the pavement is what stops two junctions being placed
	 * exactly tangent, where their rims would land on coincident edges.
	 */
	static double NodeReach(const URoadNetwork& Network, FRoadNodeId Node, int32 ArcSegments = 12,
		const FRoadDesignVehicles* DesignVehicles = nullptr);

	/**
	 * The LEAST a segment can be cut back at AtNode: the junction there solved with every
	 * fillet at zero radius - the inner corner itself - or the half-width of an end cap for
	 * a dead end. What the segment's OTHER end must leave room for, and what a placement
	 * rule asks before creating a corner the solver could only fail. 0 when the node cannot
	 * be solved at all, so a caller never sees a floor it cannot reason about.
	 */
	static double ZeroRadiusCut(const URoadNetwork& Network, FRoadSegmentId Segment, FRoadNodeId AtNode,
		const FRoadDesignVehicles* DesignVehicles = nullptr);

	/**
	 * Does the junction at Node PAVE this point? True when the point lies inside the
	 * junction's own boundary polygon, scaled by Factor about the node (1.0 = the pavement
	 * exactly). What the snap chain asks so a junction claims the cursor where there is
	 * concrete under it and nowhere else.
	 *
	 * Replaces a circle of NodeReach: a tight corner's fitted cut runs deep along its arms,
	 * and a circle of that radius covered open ground beside the junction too - the cursor
	 * a hand's width off the pavement still read "same node" (2026-09-06). A node with no
	 * polygon - a dead end, or one that failed to solve - falls back to that circle at the
	 * half-width, which is the cap it does pave.
	 */
	static bool NodeClaims(const URoadNetwork& Network, FRoadNodeId Node, const FVector2D& Point, double Factor = 1.0,
		const FRoadDesignVehicles* DesignVehicles = nullptr);

	/**
	 * How many times NodeClaims has actually run a junction solve, for the issue #167 test
	 * that a node far outside its own claim radius never reaches it - FRoadNodeSnapRule's
	 * cheap MaxPossibleNodeClaimReach reject is meant to keep it that way. A free-standing
	 * counter rather than a member, because the snap rule holds no FRoadNetworkSolver
	 * instance to count on - this is the one static entry point every caller goes through.
	 */
	static int32 NodeClaimsCallCountForTest;

	/** Zeroes the counter above. A test calls this right before the measurement it cares
	 *  about, so an earlier test's solves cannot be mistaken for this one's. */
	static void ResetNodeClaimsCallCountForTest() { NodeClaimsCallCountForTest = 0; }

	/**
	 * How far along Segment, from AtNode, the junction there is paved: the arm's SOLVED cut
	 * distance, fillet fitted. Where the segment's own pavement begins, so the segment snap
	 * rule can stand off a junction by exactly what the junction covers - NodeReach adds a
	 * half-width to that, which left a band of segment pavement that neither rule claimed
	 * (2026-09-06). 0 when the node cannot be solved or Segment is not one of its arms.
	 */
	static double ArmCutDistance(const URoadNetwork& Network, FRoadSegmentId Segment, FRoadNodeId AtNode,
		const FRoadDesignVehicles* DesignVehicles = nullptr);
};
