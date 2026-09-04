#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Editor-only half of Airside: the build tools, driven from the editor viewport.
 *
 * The tools themselves live in the runtime module and know nothing about either driver.
 * This module and ARoadBuildController are two front ends onto the same IBuildTool set -
 * which is what the Strategy/State split was for, and the reason editor support is an
 * adapter rather than a second implementation.
 */
class FAirsideEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
