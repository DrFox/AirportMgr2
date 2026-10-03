#include "Model/StandLaneClaim.h"

#include "AirsideLog.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"

FEntityInstanceId StandLaneClaim::Of(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index)
{
	const FGuidelineEdge* Edge = Plan.Steps.IsValidIndex(Index) ? Network.GetGuidelineEdge(Plan.Steps[Index].Edge) : nullptr;
	return Edge != nullptr ? Edge->StandLanesOf() : FEntityInstanceId();
}

FEntityInstanceId StandLaneClaim::EnteredAfter(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Index)
{
	const FEntityInstanceId Into = Of(Network, Plan, Index + 1);
	return Into.IsSet() && Of(Network, Plan, Index) != Into ? Into : FEntityInstanceId();
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
