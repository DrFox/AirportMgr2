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
	// INDEX 0 in ToolCommandsInOrder, matching ToolRegistry(): the Select tool. Key 4 as at
	// runtime. The label must equal the registry's "Select" exactly - see Enter's check.
	UI_COMMAND(SelectEntities, "Select", "Click an aircraft or a stand to inspect it. Escape deselects.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Four));
	UI_COMMAND(DrawRoads, "Taxiway", "Draw taxiways: click to chain, ctrl to remove, shift to insert a node.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::One));
	UI_COMMAND(DrawAprons, "Apron", "Draw a polygon of pavement; click the first corner again to close it.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Two));
	UI_COMMAND(PlaceStands, "Stand", "Place an aircraft stand: press to position, drag to aim, release.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Three));

	// Issue #33: the editor had no way to make these two, so an airport authored here could
	// never get a hand-drawn guideline link or a runway without a trip through PIE.
	UI_COMMAND(DrawGuidelines, "Guidelines", "Draw a routing link the derivation never made: click a node, click another.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Five));
	UI_COMMAND(PlaceRunways, "Runway", "Click one threshold, then the other. In play the runway key pressed again cycles the width, with Shift the surface, with Ctrl the approach.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Six));

	// EIGHT, matching the registry: seven is "land an aircraft" at runtime and is not a
	// tool. The LABEL must equal ToolRegistry()'s "Holding point" exactly - URoadBuildEdMode::
	// Enter compares the two BY STRING and logs an error when they drift.
	UI_COMMAND(PlaceHoldingPoint, "Holding point", "Click a taxiway junction node to place an intermediate holding position; click it again to remove it. Runway holding positions are derived from the runway.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Eight));

	// NINE and ZERO, the two the registry grew after this list was written. Their absence was
	// not a missing shortcut: key 9 simply left the PREVIOUS tool active, so a player aiming
	// for a service road drew a TAXIWAY - an Aircraft-only guideline (see
	// RoadGuidelineBuilder's FTrafficMask::Only(Declared.Class)) that no stand's service lane
	// and no fuel depot anchor can ever join. The only symptom was "joins nothing: no derived
	// vehicle guideline" against a road plainly on screen.
	//
	// The LABELS must equal ToolRegistry()'s "Road" and "Fuel depot" exactly - URoadBuildEdMode::
	// Enter compares the two BY STRING, index for index.
	UI_COMMAND(DrawServiceRoads, "Road", "Draw service roads for ground vehicles: click to chain, ctrl to remove, shift to insert a node.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Nine));
	UI_COMMAND(PlaceFuelDepots, "Fuel depot", "Place a fuel depot: press to position, drag to aim, release. It needs a service road within reach to be of any use.",
		EUserInterfaceActionType::ToggleButton, FInputChord(EKeys::Zero));

	UI_COMMAND(CancelGesture, "Cancel", "End the road chain or abandon the apron being drawn.",
		EUserInterfaceActionType::Button, FInputChord(EKeys::Escape));
}

TArray<TSharedPtr<FUICommandInfo>> FRoadBuildEdModeCommands::ToolCommandsInOrder() const
{
	return { SelectEntities, DrawRoads, DrawAprons, PlaceStands, DrawGuidelines, PlaceRunways,
		PlaceHoldingPoint, DrawServiceRoads, PlaceFuelDepots };
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FRoadBuildEdModeCommands::GetCommands()
{
	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Palettes;
	Palettes.Add(FName(TEXT("Build")), FRoadBuildEdModeCommands::Get().ToolCommandsInOrder());
	return Palettes;
}

#undef LOCTEXT_NAMESPACE
