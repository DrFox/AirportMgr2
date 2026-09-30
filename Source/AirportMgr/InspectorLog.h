#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

/**
 * The inspector's one log category: the widget, its cards and its purchase rows all log under it, so one grep for
 * LogInspector finds what the panel did whichever file did it (CLAUDE.md "Diagnosing": a log line is a feature).
 *
 * DECLARE/DEFINE (the definition is in InspectorWidget.cpp) rather than a DEFINE_LOG_CATEGORY_STATIC per file: a static
 * definition has internal linkage, so a second file of this UNITY-BUILD module defining the same name compiles alone and
 * collides once unified - RoadBuildLog.h's reason, and the reason the category left InspectorWidget.cpp when the cards
 * and the purchase rows became files of their own.
 * ENFORCED BY: Check-Architecture rule 2 (log-category)
 */
DECLARE_LOG_CATEGORY_EXTERN(LogInspector, Log, All);
