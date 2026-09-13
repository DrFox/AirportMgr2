#pragma once

#include "CoreMinimal.h"

/**
 * The one log category for the AirsideEditor module. DECLARED here, DEFINED once in
 * AirsideEditorModule.cpp - see AirportOpsLog.h for the same pattern and why: the module is
 * a unity build, so a second DEFINE_LOG_CATEGORY_STATIC of the same name in another .cpp
 * compiles alone and collides when the files are stitched together. Before this header,
 * RoadBuildEditorTool.cpp logged under the engine's own LogTemp and RoadBuildEdMode.cpp
 * declared its own file-local LogRoadBuildMode - neither is filterable as "this plugin's
 * editor tools" the way every other module here is (#104).
 */
AIRSIDEEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(LogAirsideEditor, Log, All);
