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
};
