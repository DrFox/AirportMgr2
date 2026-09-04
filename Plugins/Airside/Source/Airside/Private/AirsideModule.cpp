#include "AirsideModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogAirside, Log, All);

void FAirsideModule::StartupModule()
{
	UE_LOG(LogAirside, Log, TEXT("Airside module started."));
}

void FAirsideModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FAirsideModule, Airside)
