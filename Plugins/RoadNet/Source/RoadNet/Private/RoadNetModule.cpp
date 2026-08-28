#include "RoadNetModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogRoadNet, Log, All);

void FRoadNetModule::StartupModule()
{
	UE_LOG(LogRoadNet, Log, TEXT("RoadNet module started."));
}

void FRoadNetModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FRoadNetModule, RoadNet)
