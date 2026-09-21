#pragma once

#include "CoreMinimal.h"

/**
 * The AirsideTests module's own log category, declared once and shared by every test .cpp
 * that logs (issue #194). Before this header, nine files each declared their own file-local
 * DEFINE_LOG_CATEGORY_STATIC - LogM2TrafficTest (GroundTrafficTest.cpp), LogM2MapProbe
 * (StarterMapProbeTest.cpp), LogM2HeadOnTest (TrafficHeadOnReplanTest.cpp), LogM2DepTest
 * (TrafficDepartureReleaseTest.cpp), LogArrivalExitArcTest (ArrivalExitArcTest.cpp),
 * LogExitArcTest (RunwayExitArcTest.cpp), LogDepartureTest (DeparturePlannerTest.cpp),
 * LogNodeReachTest (NodeReachTest.cpp), LogPlanAnyTest (PlanAnyTest.cpp) - because a static
 * category is invisible outside its own .cpp, exactly the shape Airside/Public/AirsideLog.h's
 * own header comment already fixed for the plugin proper. `Tools/Mcp.py log <category>` could
 * not find any ONE of the nine by name; this gives every probe line in the module the same
 * category to be found under, whichever file printed it.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogAirsideTests, Log, All);
