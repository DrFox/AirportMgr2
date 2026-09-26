#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"

class URoadNetwork;
struct FVehicle;

/**
 * LIFTED OFF ARigTestCourse (#301): the course, then FuelService::ChooseDepot, each ran the
 * SAME per-(start,goal,vehicle) Find with a per-vehicle edge-fit cache as private members - a
 * cache worth having is worth having in one place, not two that could drift out of step with
 * FRouteQuery::FitCache's own contract (same vehicle, same graph). This file is that one place.
 */
namespace RoutePlanCache
{
	/**
	 * What a cached plan is keyed on for its VEHICLE (re-review of aa90eec2): the type code and
	 * every figure route search gates on - body, chassis, lock, each tow link, and the taxi
	 * speed figures a whole-route tow check drives the plan at. Never an instance or a slot: a
	 * caller may swap the body a key names (ARigTestCourse::SetVehicleForTest), so the key is
	 * the figures themselves.
	 * ENFORCED BY: AirportMgr.RigCourse.PlanCacheKnowsItsVehicle
	 */
	AIRSIDE_API uint32 VehicleIdentity(const FVehicle& Vehicle);
}

/** One cached answer: the plan (or its failure) and the refusal text, if any - a refusal is as
 *  much a fact about the graph and the body as a route is, so it is cached too. */
struct AIRSIDE_API FCachedRoutePlan
{
	FRoutePlan Plan;
	FString Reason;
};

/**
 * A per-(start,goal,vehicle) route-plan cache with a per-vehicle edge-fit cache
 * (FRouteQuery::FitCache), dated by the network's own GetGuidelineRevision - see
 * FRouteQuery::FitCache's own header comment for the contract this struct keeps in ONE place.
 *
 * WORLD-FREE, like every Model/ type: it holds only a weak pointer to the URoadNetwork it was
 * last asked about, for identity and revision, never a piece of its own graph - a caller still
 * owns the Find it runs on a miss.
 *
 * NOT SELF-DRIVING (Lookup/Store, not FindOrRun): ARigTestCourse::PlanBetween and
 * FuelService::ChooseDepot ask for different errands, gate different rules and log different
 * things around the Find, so the cache only remembers the ANSWER - it never issues the search.
 */
struct AIRSIDE_API FRoutePlanCache
{
	/**
	 * Clears every cached plan and edge-fit table when Network or its guideline revision has
	 * moved since the last call - THE ONLY INVALIDATION RULE. Call before Lookup/Store/
	 * FitCacheFor every time, the same way ARigTestCourse::PlanBetween used to inline it: it is
	 * cheap when nothing changed and correct when it did.
	 */
	void EnsureFresh(const URoadNetwork& Network);

	/** The cached plan for (Start, Goal, this Vehicle's figures), or null on a miss. */
	const FCachedRoutePlan* Lookup(FGuidelineNodeId Start, FGuidelineNodeId Goal, const FVehicle& Vehicle) const;

	/** Remembers Plan/Reason for (Start, Goal, this Vehicle's figures). */
	void Store(FGuidelineNodeId Start, FGuidelineNodeId Goal, const FVehicle& Vehicle,
		const FRoutePlan& Plan, const FString& Reason);

	/**
	 * The edge-fit memo to wire onto FRouteQuery::FitCache before a Find on a miss - most of a
	 * vehicle's Find is tracing it round every curve the search relaxes, and this memoises that
	 * across Finds the same graph, the same vehicle, keeps asking (FRouteQuery::FitCache's own
	 * header, ~20 ms a Find for the rig, measured 2026-09-25).
	 */
	TMap<FGuidelineEdgeId, bool>& FitCacheFor(const FVehicle& Vehicle);

	/** Every (edge, fits) any vehicle's fit cache holds - the staleness test's own reader.
	 *  ENFORCED BY: AirportMgr.RigCourse.FitCacheDropsOnRebuild */
	void ForEachFitCacheEntryForTest(TFunctionRef<void(FGuidelineEdgeId Edge, bool bFits)> Visit) const;

private:
	struct FKey
	{
		FGuidelineNodeId Start;
		FGuidelineNodeId Goal;
		uint32 Vehicle = 0;
		bool operator==(const FKey& Other) const
		{
			return Start == Other.Start && Goal == Other.Goal && Vehicle == Other.Vehicle;
		}
		friend uint32 GetTypeHash(const FKey& Key)
		{
			return HashCombine(HashCombine(GetTypeHash(Key.Start), GetTypeHash(Key.Goal)), ::GetTypeHash(Key.Vehicle));
		}
	};

	TMap<FKey, FCachedRoutePlan> Plans;
	/** Per vehicle identity, VehicleFit::Fits per edge (FRouteQuery::FitCache), cleared with Plans. */
	TMap<uint32, TMap<FGuidelineEdgeId, bool>> FitCaches;
	TWeakObjectPtr<const URoadNetwork> CachedNetwork;
	uint32 CachedRevision = 0;
};
