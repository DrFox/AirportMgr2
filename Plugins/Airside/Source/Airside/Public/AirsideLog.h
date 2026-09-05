#pragma once

#include "CoreMinimal.h"

/**
 * The plugin's log categories, declared once and shared across translation units.
 *
 * Before this header, each file that logged declared its own DEFINE_LOG_CATEGORY_STATIC
 * (LogLanding, LogTakeoff, LogRoadAgent, LogRoadMesh, LogAirside) - a static category is
 * invisible outside its own .cpp, so that was the only way for each file to get a name at
 * all. But the module is a unity build: two _STATIC categories of the SAME name in
 * different files silently collide into one translation unit, so per-file statics only
 * worked because every file picked a different name. That produced one category per FILE,
 * not one per CONCERN - LogLanding and LogTakeoff say "which .cpp logged this", when what a
 * reader wants to filter on is "which concern" (traffic moving through the field, versus
 * the road surface itself). Extern categories fix both problems at once: one name can be
 * shared by every file that logs the same concern, however many .cpp files that spans.
 *
 * LogAirside: general/startup. LogAirsideTraffic: agents, dispatch, arrivals, departures,
 * landings, take-offs, taxi, parking and shutdown - the field's traffic, wherever in the
 * plugin it is logged from. LogRoadMesh (declared here too, for the same reason) stays the
 * surface/edit-facade category: solve, rebuild census, ghost preview, apron and node edits.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogAirside, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogAirsideTraffic, Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogRoadMesh, Log, All);
