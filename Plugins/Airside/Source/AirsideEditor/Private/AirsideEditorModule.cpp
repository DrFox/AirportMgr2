#include "AirsideEditorModule.h"

#include "RoadBuildEdModeCommands.h"

#define LOCTEXT_NAMESPACE "AirsideEditor"

void FAirsideEditorModule::StartupModule()
{
	// The mode itself needs no registration: UEdMode subclasses are discovered from their
	// FEditorModeInfo. Only the command list has to be registered by hand.
	FRoadBuildEdModeCommands::Register();
}

void FAirsideEditorModule::ShutdownModule()
{
	FRoadBuildEdModeCommands::Unregister();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAirsideEditorModule, AirsideEditor)
