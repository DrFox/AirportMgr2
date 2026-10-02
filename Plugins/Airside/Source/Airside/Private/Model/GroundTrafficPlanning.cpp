// UGroundTraffic's half of space-time taxi planning (spec 2026-10-02): the requests it makes of UTaxiPlanning for an
// arrival and a departure, the per-tick tracking of every cleared aircraft, and the pushes a booked departure is due.
// The planning owner itself - table, clearances, order - is Model/TaxiPlanning.h.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/DeparturePlanner.h"
#include "Model/PushbackPlanner.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiPlanning.h"

// ---- PR 2 STUBS (red batch) ----

void UGroundTraffic::PostInitProperties()
{
	Super::PostInitProperties();
}

uint32 UGroundTraffic::TaxiPlanRevision() const
{
	return 0;
}

EArrivalRefusal UGroundTraffic::TaxiInRefusal(const URoadNetwork& Network, const FArrivalPlan& Plan, const FAirframe& Airframe) const
{
	return EArrivalRefusal::None;
}

FTaxiRequest UGroundTraffic::TaxiInRequestNow(const FArrivalPlan& Plan, const FAirframe& Airframe) const
{
	return FTaxiRequest();
}

FTaxiRequest UGroundTraffic::TaxiOutRequest(const FRoadAgent& Agent, const FAirframe& Aircraft, const FDeparturePlan& Departure,
	const FPushbackPlan* Push) const
{
	return FTaxiRequest();
}

void UGroundTraffic::TrackTaxiPlans()
{
}

void UGroundTraffic::StartDuePushes(const URoadNetwork& Network)
{
}

bool UGroundTraffic::StartPlannedDeparture(int32 AgentId, const URoadNetwork& Network, const FRoutePlan& PushRoute,
	const FRoutePlan& TaxiRoute)
{
	return false;
}
