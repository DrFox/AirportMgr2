#include "Model/StandLaneClaim.h"

#include "AirsideLog.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"

FEntityInstanceId StandLaneClaim::Of(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index)
{
	const FGuidelineEdge* Edge = Plan.Steps.IsValidIndex(Index) ? Network.GetGuidelineEdge(Plan.Steps[Index].Edge) : nullptr;
	const FEntityInstanceId Stand = Edge != nullptr ? Edge->StandLanesOf() : FEntityInstanceId();
	// A DELETED STAND HAS NO LANES TO HOLD, though its edges may outlive it until the next derivation: a claim on a dead
	// entity would be a lock nobody could ever be granted again (review of #542; Airside.Model.Traffic.StandLanes.StandDeletedFreesThem).
	const FEntityInstance* Alive = Stand.IsSet() ? Network.GetEntity(Stand) : nullptr;
	return Alive != nullptr && Alive->bAlive ? Stand : FEntityInstanceId();
}

FEntityInstanceId StandLaneClaim::EnteredAfter(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index)
{
	const FEntityInstanceId Into = Of(Network, Plan, Index + 1);
	return Into.IsSet() && Of(Network, Plan, Index) != Into ? Into : FEntityInstanceId();
}

FEntityInstanceId StandLaneClaim::OnStep(const FRoadAgent& Agent, const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index)
{
	return Agent.Class == ETraversalClass::GroundVehicle ? Of(Network, Plan, Index) : FEntityInstanceId();
}

FEntityInstanceId StandLaneClaim::DoorAfter(const FRoadAgent& Agent, const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index)
{
	return Agent.Class == ETraversalClass::GroundVehicle ? EnteredAfter(Network, Plan, Index) : FEntityInstanceId();
}

void StandLaneClaim::LogChange(int32 AgentId, FEntityInstanceId Before, FEntityInstanceId After)
{
	// ON THE CHANGE ONLY - a line a tick would bury the log. The WAIT is the claim pass's own refusal line
	// ("stops N uu short of the stand lanes of entity S held by agent A"), also on its transition. LogAirsideTraffic, not
	// LogAirportOps: this is the traffic model's rule, and Airside may not name the ops plugin's category.
	if (Before == After)
	{
		return;
	}
	if (Before.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d leaves the stand lanes of entity %d"), AgentId, Before.Index);
	}
	if (After.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d enters the stand lanes of entity %d (one service vehicle at a time)"), AgentId, After.Index);
	}
}
