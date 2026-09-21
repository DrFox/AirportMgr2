#include "Modules/ModuleManager.h"

#include "AirsideTestsLog.h"

// One log category for the whole module (issue #194) - see AirsideTestsLog.h's own comment
// for the nine per-file DEFINE_LOG_CATEGORY_STATICs this replaces.
DEFINE_LOG_CATEGORY(LogAirsideTests);

IMPLEMENT_MODULE(FDefaultModuleImpl, AirsideTests)
