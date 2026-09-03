#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;

/** Every node's solved boundary, keyed by FRoadNodeId::Index. */
struct FRoadSolveResult
{
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
};

/**
 * Walks a URoadNetwork, solves every live node, and writes each segment's trim
 * distances AND its four cut vertices back into the model.
 *
 * This is the only writer of FRoadSegment::TrimA/TrimB and the cut vertices. It lives
 * in Build/ rather than Solve/ because it touches UObjects, and Solve/ must stay free
 * of engine dependencies so its tests can run without a World.
 */
class ROADNET_API FRoadNetworkSolver
{
public:
	static FRoadSolveResult SolveAll(URoadNetwork& Network, int32 ArcSegments = 12);

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
		FRoadNodeCuts& Out);

	/**
	 * How far a node's pavement reaches from its centre, in uu. Zero when it has no arms.
	 *
	 * Deliberately CONSERVATIVE: an arm's furthest pavement corner is at
	 * sqrt(Cut^2 + HalfWidth^2), and this returns Cut + HalfWidth, which is never smaller.
	 * A reach that slightly exceeds the pavement is what stops two junctions being placed
	 * exactly tangent, where their rims would land on coincident edges.
	 */
	static double NodeReach(const URoadNetwork& Network, FRoadNodeId Node, int32 ArcSegments = 12);
};
