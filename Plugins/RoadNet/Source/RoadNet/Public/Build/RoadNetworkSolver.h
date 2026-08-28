#pragma once

#include "CoreMinimal.h"
#include "Solve/JunctionSolver.h"

class URoadNetwork;

/** Every node's solved boundary, keyed by FRoadNodeId::Index. */
struct FRoadSolveResult
{
	TMap<int32, FJunctionResult> NodeResults;
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
