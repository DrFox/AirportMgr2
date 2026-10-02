#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;
struct FGuidelineEdge;
struct FRouteQuery;

/** One edge a query may take out of a node, as FRouteEdgeFilter admitted it. */
struct FAdmittedEdge
{
	FGuidelineEdgeId Id;

	/** Live: the filter has looked it up and refused it if not. */
	const FGuidelineEdge* Edge = nullptr;

	/** The node this edge leads to from the one being expanded. */
	FGuidelineNodeId Next;

	/** True when the edge is walked B to A. */
	bool bReversed = false;

	/**
	 * Whether the edge's source segment is a runway - the ONE answer both the avoidance filter and RouteSearch's
	 * runway cost penalty read, so the edge the filter judged is the edge the cost charged for.
	 */
	bool bRunwayEdge = false;
};

/**
 * RouteSearch's answer to "may this query take this edge out of this node" - traffic class, one-way, the bans,
 * pavement, runway avoidance and size - as a thing two searches can share (2026-10-02).
 *
 * EXTRACTED FROM RouteSearch.cpp's ExpandNode, NOT RE-WRITTEN: the A* (RouteSearch::Find, FindToGoals) and the
 * space-time planner (FTaxiPlanner) need the same admissibility under different costs and different states - A*
 * relaxes nodes by length, SIPP relaxes (node, free interval) pairs by time. A copy of the rule in the planner is
 * the shape the spec forbids ("RouteSearch's, reused - never a second copy") and the drift #297 measured ten ways.
 * So the filter is here and both searches call it; cost and relaxation stay with each search.
 *
 * ONE PER SEARCH: the runway memos below are per search for the reason ExpandNode always gave - runway-ness
 * depends on the PROFILE, which a re-profile changes, and a memo kept longer would answer for the old one.
 */
class AIRSIDE_API FRouteEdgeFilter
{
public:
	/**
	 * bInIgnoreSize lifts the wingspan and vehicle-fit gates (RouteSearch::Find's unconstrained retry). InExcluded
	 * are a tow's fold exclusions, InFitMemo a VehicleFit::Fits memo - both RouteSearch::Find's, null elsewhere.
	 */
	FRouteEdgeFilter(const URoadNetwork& InNetwork, const FRouteQuery& InQuery, bool bInIgnoreSize = false,
		const TSet<FGuidelineEdgeId>* InExcluded = nullptr, TMap<FGuidelineEdgeId, bool>* InFitMemo = nullptr);

	/** Calls Visit for every edge leaving At that the query may take, in the network's incidence order. */
	void ForEachAdmitted(FGuidelineNodeId At, TFunctionRef<void(const FAdmittedEdge&)> Visit);

private:
	bool IsRunwayEdge(FRoadSegmentId Seed);
	bool IsRunwayHeld(FRoadSegmentId Seed);
	bool VehicleFits(FGuidelineEdgeId EdgeId, const FGuidelineEdge& Edge);

	const URoadNetwork& Network;
	const FRouteQuery& Query;
	bool bIgnoreSize = false;
	const TSet<FGuidelineEdgeId>* Excluded = nullptr;
	TMap<FGuidelineEdgeId, bool>* FitMemo = nullptr;

	/** Per runway segment index: is it a runway. See IsRunwayEdge. */
	TMap<int32, bool> RunwaySeeds;

	/** Per runway segment index: is its chain in use. See IsRunwayHeld. */
	TMap<int32, bool> RunwayInUse;
};
