#include "RoadNetEditorModule.h"

#include "RoadBuildEdModeCommands.h"

#define LOCTEXT_NAMESPACE "RoadNetEditor"

void FRoadNetEditorModule::StartupModule()
{
	// The mode itself needs no registration: UEdMode subclasses are discovered from their
	// FEditorModeInfo. Only the command list has to be registered by hand.
	FRoadBuildEdModeCommands::Register();
}

void FRoadNetEditorModule::ShutdownModule()
{
	FRoadBuildEdModeCommands::Unregister();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FRoadNetEditorModule, RoadNetEditor)
