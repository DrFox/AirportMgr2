#include "RoadBuildEdModeCommands.h"

#include "Styling/AppStyle.h"
#include "Tool/BuildSession.h"

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
	// ONE PER ToolRegistry() ENTRY, built directly from it (issue #105 item 10) rather than
	// nine hand-written UI_COMMAND calls whose LABEL had to be checked against the registry's
	// Name BY STRING at URoadBuildEdMode::Enter - a runtime check that two lists still
	// agreed, where building this one FROM the other means there is only one to agree with
	// itself. UI_COMMAND cannot do this directly: it requires a compile-time NAMED FIELD per
	// command (this class used to carry nine - SelectEntities, DrawRoads, ...), so this loop
	// calls FUICommandInfo::MakeCommandInfo, the primitive UI_COMMAND itself expands to.
	//
	// Same order as the runtime tool keys, so 1, 2 and 3 mean the same thing in the editor as
	// they do in play - which now follows automatically from reading the same table, rather
	// than needing nine hand-typed keys to happen to still match it.
	//
	// NO ICON: none of the nine hand-written commands passed one either (UI_COMMAND's
	// optional trailing arg), so MakeUICommand_InternalUseOnly resolved one from a per-command
	// style path ("RoadBuildEdMode.<FieldName>") that had nothing registered under it - the
	// same nothing FSlateIcon() renders here, by a shorter route.
	for (const FToolRegistration& Tool : ToolRegistry())
	{
		TSharedPtr<FUICommandInfo> Command;
		FUICommandInfo::MakeCommandInfo(
			AsShared(),
			Command,
			FName(*Tool.Name.ToString()),
			Tool.Name,
			Tool.Tooltip,
			FSlateIcon(),
			EUserInterfaceActionType::ToggleButton,
			FInputChord(Tool.Key));
		ToolCommands.Add(Command);
	}

	// NOT in ToolRegistry(): it is not a tool, so it stays a hand-written UI_COMMAND.
	UI_COMMAND(CancelGesture, "Cancel", "End the road chain or abandon the apron being drawn.",
		EUserInterfaceActionType::Button, FInputChord(EKeys::Escape));
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FRoadBuildEdModeCommands::GetCommands()
{
	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Palettes;
	Palettes.Add(FName(TEXT("Build")), FRoadBuildEdModeCommands::Get().ToolCommandsInOrder());
	return Palettes;
}

#undef LOCTEXT_NAMESPACE
