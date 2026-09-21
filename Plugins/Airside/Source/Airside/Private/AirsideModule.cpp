#include "AirsideModule.h"

#include "AirsideLog.h"

// Defined here, once, for the whole module - see AirsideLog.h for why these moved off
// per-file DEFINE_LOG_CATEGORY_STATIC.
DEFINE_LOG_CATEGORY(LogAirside);
DEFINE_LOG_CATEGORY(LogAirsideTraffic);
DEFINE_LOG_CATEGORY(LogRoadMesh);

// ISSUE #216: see AirsideLog.h's own comment on GetLogRoadMeshVerbosityForTest for why this
// exists at all - the category itself cannot be read from outside this module.
ELogVerbosity::Type GetLogRoadMeshVerbosityForTest()
{
	return LogRoadMesh.GetVerbosity();
}

void FAirsideModule::StartupModule()
{
	UE_LOG(LogAirside, Log, TEXT("Airside module started."));
}

void FAirsideModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FAirsideModule, Airside)
