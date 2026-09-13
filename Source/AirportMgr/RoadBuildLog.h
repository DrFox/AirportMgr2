#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/**
 * The category every build-driver log line in this module uses: ARoadBuildController,
 * UBuildCameraComponent and UBuildHudLayer alike.
 *
 * Its OWN header rather than living in RoadBuildController.h (where it started): a
 * component or a widget-owning subobject including its OWNER's header just to reach a log
 * category is a layering smell - RoadBuildController.h pulls in the whole controller's
 * public surface for one macro. This file has nothing else in it, so anything that logs
 * under LogRoadBuild can depend on exactly that and nothing more.
 *
 * DECLARE/DEFINE (in RoadBuildLog.cpp) rather than DEFINE_LOG_CATEGORY_STATIC: a STATIC
 * definition has internal linkage, so a second file in this UNITY-BUILD module defining the
 * same name would compile alone and collide once unified with this one - the exact failure
 * mode CLAUDE.md's "one log category per name" rule exists to catch.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogRoadBuild, Log, All);
