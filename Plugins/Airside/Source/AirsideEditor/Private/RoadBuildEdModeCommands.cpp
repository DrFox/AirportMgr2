#include "RoadBuildEdModeCommands.h"

#include "Styling/AppStyle.h"
#include "Tool/BuildSession.h"
#include "Tool/SnapToggleRegistry.h"

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

	// ONE PER SnapToggleRegistry() ENTRY (issue #440), the same loop a third time. TOGGLEBUTTON for
	// the verbs' reason: a guide switch is state the palette shows lit. The Grid button CYCLES
	// rather than flips, and is lit while any step is on - the PIE bar's own reading of it.
	//
	// ITS CAPTION DOES NOT FOLLOW THE STEP HERE, by choice (#468's review): FToolBarBuilder::
	// AddToolBarButton does take a label override, but only a BuildToolPalette of this module's own
	// would pass one - and the stock FModeToolkit::BuildToolPalette is the consumer
	// Airside.Editor.EveryCommandIsReachable measures. Overriding it to caption one button would
	// swap the engine's consumer for ours under the pin. The step it moved to is in the log line
	// URoadBuildEdMode::ApplySnapToggle writes, and the Grid button's check says whether it is on.
	// The registry's Key comes along, so a toggle given a key has it in both drivers - none has
	// one since the owner dropped the grid orientation's H (2026-09-30): the editor's H is its own.
	for (const FSnapToggleRegistration& Toggle : SnapToggleRegistry())
	{
		TSharedPtr<FUICommandInfo> Command;
		FUICommandInfo::MakeCommandInfo(
			AsShared(),
			Command,
			Toggle.Id,
			Toggle.Name,
			Toggle.Tooltip,
			FSlateIcon(),
			EUserInterfaceActionType::ToggleButton,
			FInputChord(Toggle.Key));
		SnapCommands.Add(Command);
	}
}

TArray<FRoadBuildPalette> FRoadBuildEdModeCommands::Palettes()
{
	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();
	TArray<FRoadBuildPalette> Out;
	// "Build" STAYS THE TOOLS PALETTE'S NAME: it is the one this mode has shipped with since PR #9.
	Out.Add({ FName(TEXT("Build")), LOCTEXT("ToolsPalette", "Tools"), Commands.ToolCommandsInOrder() });

	// EDIT: the sticky verbs, then the two one-shots, in the PIE bar's Edit order (Build before the
	// modes there too). Cancel has no bar button in PIE only because right-click is its gesture
	// there; the editor viewport's right-click is the context menu's, so here it is a button.
	TArray<TSharedPtr<FUICommandInfo>> Edit;
	Edit.Add(Commands.Build);
	Edit.Append(Commands.VerbCommandsInOrder());
	Edit.Add(Commands.CancelGesture);
	Out.Add({ FName(TEXT("Edit")), LOCTEXT("EditPalette", "Edit"), MoveTemp(Edit) });

	// SNAP and SNAP TO: the registry's two groups, as the PIE bar's two sections.
	TArray<TSharedPtr<FUICommandInfo>> Snap;
	TArray<TSharedPtr<FUICommandInfo>> SnapTo;
	const TArray<TSharedPtr<FUICommandInfo>> SnapCommands = Commands.SnapCommandsInOrder();
	const TConstArrayView<FSnapToggleRegistration> Toggles = SnapToggleRegistry();
	for (int32 Index = 0; Index < SnapCommands.Num() && Index < Toggles.Num(); ++Index)
	{
		(Toggles[Index].Group == ESnapToggleGroup::SnapTo ? SnapTo : Snap).Add(SnapCommands[Index]);
	}
	Out.Add({ FName(TEXT("Snap")), LOCTEXT("SnapPalette", "Snap"), MoveTemp(Snap) });
	Out.Add({ FName(TEXT("SnapTo")), LOCTEXT("SnapToPalette", "Snap to"), MoveTemp(SnapTo) });
	return Out;
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> FRoadBuildEdModeCommands::GetCommands()
{
	TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> Keyed;
	for (FRoadBuildPalette& Palette : Palettes())
	{
		Keyed.Add(Palette.Name, MoveTemp(Palette.Commands));
	}
	return Keyed;
}

#undef LOCTEXT_NAMESPACE
