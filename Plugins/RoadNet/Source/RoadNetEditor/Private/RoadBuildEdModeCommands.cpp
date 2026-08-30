#include "RoadBuildEdModeCommands.h"

#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "RoadBuildEdModeCommands"

FRoadBuildEdModeCommands::FRoadBuildEdModeCommands()
	: TCommands<FRoadBuildEdModeCommands>(
		TEXT("RoadBuildEdMode"),
		LOCTEXT("RoadBuildEdMode", "Road Build"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FRoadBuildEdModeCommands::RegisterCommands()
{
	// Same order as the runtime tool keys, so 1, 2 and 3 mean the same thing in the editor
	// as they do in play. A tool that changed number between the two would be worse than
	// having no shortcut at all.
	UI_COMMAND(DrawRoads, "Roads", "Draw taxiways and roads: click to chain, ctrl to remove, shift to insert a node.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::One));
	UI_COMMAND(DrawAprons, "Aprons", "Draw a polygon of pavement; click the first corner again to close it.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Two));
	UI_COMMAND(PlaceStands, "Stands", "Place an aircraft stand: press to position, drag to aim, release.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Three));
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FRoadBuildEdModeCommands::GetCommands()
{
	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();

	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Palettes;
	Palettes.Add(FName(TEXT("Build")), { Commands.DrawRoads, Commands.DrawAprons, Commands.PlaceStands });
	return Palettes;
}

#undef LOCTEXT_NAMESPACE
