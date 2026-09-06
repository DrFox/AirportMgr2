#pragma once

#include "CoreMinimal.h"

/**
 * The one log category for the AirportOps module. DECLARED here, DEFINED once in
 * AirportOpsModule.cpp. The module is a unity build, so a second DEFINE_LOG_CATEGORY_STATIC
 * of the same name in another .cpp compiles alone and collides when the files are stitched -
 * Airside learned this the hard way, and Check-Architecture.ps1 now fails it.
 */
AIRPORTOPS_API DECLARE_LOG_CATEGORY_EXTERN(LogAirportOps, Log, All);
