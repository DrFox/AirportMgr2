#include "Model/TaxiPlanning.h"

#include "AirsideLog.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficRules.h"

// ---- PR 2 STUBS (red batch) ----

void UTaxiPlanning::SetHeadway(double Seconds)
{
}

FTaxiPlan UTaxiPlanning::Plan(const URoadNetwork& Network, const FAirframe& Airframe, const FTrafficRules& Rules,
	const FTaxiRequest& Request) const
{
	return FTaxiPlan();
}

FTaxiPlan UTaxiPlanning::PlanArrival(const URoadNetwork& Network, const FAirframe& Airframe, const FTrafficRules& Rules,
	const FTaxiRequest& Request, TArray<int32>& OutRevoke) const
{
	return FTaxiPlan();
}

bool UTaxiPlanning::Book(const URoadNetwork& Network, int32 Holder, ETaxiClearanceKind Kind, const FTaxiPlan& InPlan,
	ETaxiClearanceStage Stage, const FRoutePlan& PushRoute)
{
	return false;
}

bool UTaxiPlanning::Revoke(int32 Holder, int32 ByArrival)
{
	return false;
}

void UTaxiPlanning::Drop(int32 Holder, const TCHAR* Why)
{
}

void UTaxiPlanning::DropAll(const TCHAR* Why)
{
}

void UTaxiPlanning::NoteRefused(int32 Holder, const FString& What, const FString& Why)
{
}

int32 UTaxiPlanning::WaitingFor(int32 Holder, const FTaxiResource& Resource) const
{
	return 0;
}

bool UTaxiPlanning::OrderHold(const FRoadAgent& Agent, double Now, double T, double Head, FTaxiOrderHold& Out)
{
	return false;
}

void UTaxiPlanning::Track(const FRoadAgent& Agent, const FTrafficOccupancy& Occupancy, const FTrafficRules& Rules)
{
}

bool UTaxiPlanning::TakeReleased()
{
	return false;
}

bool UTaxiPlanning::BookPassesForTest(TConstArrayView<FTaxiPass> Passes)
{
	return false;
}

void UTaxiPlanning::Bump(bool bReleasedSomething)
{
}
