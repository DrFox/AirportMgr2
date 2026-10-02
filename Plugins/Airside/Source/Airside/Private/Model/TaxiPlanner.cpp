#include "Model/TaxiPlanner.h"

#include "Model/Airframe.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficRules.h"

// RED-PHASE STUBS (batched testing): compile, and fail each test on its own assertion.

FTaxiPlanner::FTaxiPlanner(const URoadNetwork& InNetwork, const FTaxiReservations& InTable, const FAirframe& InAirframe,
	const FTrafficRules& InRules)
	: Network(InNetwork)
	, Table(InTable)
	, Airframe(InAirframe)
	, Rules(InRules)
{
}

FTaxiPlan FTaxiPlanner::Plan(const FTaxiRequest& Request)
{
	return FTaxiPlan();
}

double FTaxiPlanner::RouteSeconds(const FRoutePlan& Route)
{
	return -1.0;
}

const FTaxiEdgeSeconds& FTaxiPlanner::SecondsFor(FGuidelineEdgeId Edge, bool bReversed)
{
	return EdgeSeconds.FindOrAdd(TPair<FGuidelineEdgeId, bool>(Edge, bReversed));
}

bool FTaxiPlanner::CanHoldAt(const URoadNetwork& InNetwork, const FTrafficRules& InRules, FGuidelineEdgeId Arrived,
	FGuidelineNodeId At)
{
	return false;
}
