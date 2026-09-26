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
			// Tool.Id, not Tool.Name (PR #137 review): the command id is what
			// FUICommandInfo persists to the editor's per-user keybindings ini, and Name is
			// LOCTEXT - a localised id would orphan every saved keybinding on a locale change.
			Tool.Id,
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

	// ALSO NOT in ToolRegistry(), for the same reason - see the header's own comment on
	// issue #185. Same chord as PIE's edit.build (BuildActions.cpp).
	UI_COMMAND(Build, "Build", "Commit the gesture the active tool has staged.",
		EUserInterfaceActionType::Button, FInputChord(EKeys::Enter));

	// ONE PER BuildVerbRegistry() ENTRY (issue #304), the SAME MakeCommandInfo loop as
	// ToolCommands above rather than nine more hand-written UI_COMMANDs - this is exactly the
	// shape "check where a list is CONSUMED" asks for: Remove/Insert/Edit already had a row in
	// BuildActions.cpp, and reading them from the one shared table means this loop and that one
	// cannot drift the way the tool commands once did.
	//
	// TOGGLEBUTTON, unlike CancelGesture/Build's plain Button above: these three are STICKY
	// state the palette should show lit, not a one-shot action.
	for (const FBuildVerbRegistration& Verb : BuildVerbRegistry())
	{
		TSharedPtr<FUICommandInfo> Command;
		FUICommandInfo::MakeCommandInfo(
			AsShared(),
			Command,
			Verb.Id,
			Verb.Name,
			Verb.Tooltip,
			FSlateIcon(),
			EUserInterfaceActionType::ToggleButton,
			FInputChord(Verb.Key));
		VerbCommands.Add(Command);
	}
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FRoadBuildEdModeCommands::GetCommands()
{
	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Palettes;
	Palettes.Add(FName(TEXT("Build")), FRoadBuildEdModeCommands::Get().ToolCommandsInOrder());
	return Palettes;
}

#undef LOCTEXT_NAMESPACE
