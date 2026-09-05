#include "RoadBuildEdMode.h"

#include "EdModeInteractiveToolsContext.h"
#include "RoadBuildEdModeCommands.h"
#include "RoadBuildEditorTool.h"
#include "Tool/BuildSession.h"
#include "Toolkits/BaseToolkit.h"

#define LOCTEXT_NAMESPACE "RoadBuildEdMode"

// Every tool switch is logged, and so is every tool that gets built.
//
// Not decoration: this mode has now produced three separate rounds of "the tool looks
// broken" where the model was correct, and each was diagnosed from screenshots and guesses
// before anything measured the boundary. A tool that does not activate and a tool that
// activates and draws nothing are indistinguishable on screen, and these two lines tell
// them apart outright.
DEFINE_LOG_CATEGORY_STATIC(LogRoadBuildMode, Log, All);

namespace
{
	/**
	 * A toolkit that actually SHOWS the mode's palette.
	 *
	 * FModeToolkit::GetToolPaletteNames is `{}` - it returns nothing at all. BuildToolPalette
	 * looks up GetModeCommands() BY PALETTE NAME, so with no names it is never called with
	 * one, and a mode using the stock toolkit renders no buttons however many commands it
	 * declares. URoadBuildEdMode::GetModeCommands has been returning a "Build" palette that
	 * nothing on the other side ever asked for.
	 *
	 * This is the SAME defect as the ToolCommandList one: a list declared in the right shape,
	 * consumed by nobody. Check where a list is READ, not where it is filled in.
	 */
	class FRoadBuildModeToolkit : public FModeToolkit
	{
	public:
		virtual void GetToolPaletteNames(TArray<FName>& PaletteNames) const override
		{
			// Must match the key GetModeCommands fills in, or the lookup misses and this
			// is right back to drawing nothing.
			PaletteNames.Add(FName(TEXT("Build")));
		}

		virtual FText GetToolPaletteDisplayName(FName Palette) const override
		{
			return LOCTEXT("BuildPalette", "Build");
		}
	};
}

const FEditorModeID URoadBuildEdMode::EM_RoadBuild = TEXT("EM_RoadBuild");

URoadBuildEdMode::URoadBuildEdMode()
{
	Info = FEditorModeInfo(
		EM_RoadBuild,
		LOCTEXT("ModeName", "Road Build"),
		FSlateIcon(),
		true);
}

FString URoadBuildEdMode::MakeToolName(int32 Index)
{
	return FString::Printf(TEXT("Airside_%s"), *ToolRegistry()[Index].Name.ToString());
}

void URoadBuildEdMode::Enter()
{
	UEdMode::Enter();

	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();
	const TArray<TSharedPtr<FUICommandInfo>> ToolCommands = Commands.ToolCommandsInOrder();
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();

	// Logged rather than a hard check(): a crash over a wiring mistake is worse than a tool
	// with no command. The runtime controller has no equivalent check any more - its tool
	// list, its key bindings and its banner are ALL read from ToolRegistry() itself, so
	// they cannot disagree by construction. This CAN still disagree:
	// FRoadBuildEdModeCommands::ToolCommandsInOrder() is a hand-written UI_COMMAND list, a
	// separate thing from ToolRegistry(), so this is the one place left that has to check
	// the two agree rather than being able to assume it.
	//
	// A RUNTIME check, not an automation test, and that is a real gap rather than a stylistic
	// choice: AirsideTests does not depend on AirsideEditor (Model/Present/Tool are tested
	// world-free with no editor module loaded at all), so nothing short of opening this mode
	// exercises FRoadBuildEdModeCommands. This log line is what stands in for that test.
	if (ToolCommands.Num() != Registry.Num())
	{
		UE_LOG(LogRoadBuildMode, Error,
			TEXT("%d editor commands but %d registry entries - see FRoadBuildEdModeCommands::"
				 "ToolCommandsInOrder and ToolRegistry(). A tool past the shorter count gets "
				 "no command."),
			ToolCommands.Num(), Registry.Num());
	}

	FString Banner;
	for (int32 Index = 0; Index < FMath::Min(ToolCommands.Num(), Registry.Num()); ++Index)
	{
		// COUNT alone does not catch a reordering - six of each, wrongly paired, still
		// passes the check above. Identity does: a command's label is a human-authored
		// UI_COMMAND string, so it names which tool the palette THINKS index Index is,
		// independent of the FKey/Make lambda the registry entry actually carries.
		if (const TSharedPtr<FUICommandInfo>& Command = ToolCommands[Index];
			Command.IsValid() && !Command->GetLabel().EqualTo(Registry[Index].Name))
		{
			UE_LOG(LogRoadBuildMode, Error,
				TEXT("Tool %d: command label \"%s\" does not match registry name \"%s\" - ")
				TEXT("FRoadBuildEdModeCommands::ToolCommandsInOrder and ToolRegistry() have ")
				TEXT("drifted out of order."),
				Index, *Command->GetLabel().ToString(), *Registry[Index].Name.ToString());
		}

		URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(this);
		Builder->ToolIndex = Index;
		RegisterTool(ToolCommands[Index], MakeToolName(Index), Builder);

		Banner += FString::Printf(TEXT("%s%d %s"),
			Index == 0 ? TEXT("") : TEXT(", "), Index + 1, *Registry[Index].Name.ToString());
	}

	UE_LOG(LogRoadBuildMode, Log,
		TEXT("Road Build mode entered. Tools: %s. If a number key does nothing, the palette "
			 "buttons do the same job."),
		*Banner);

	// Roads first, because it is the one that needs no setup - an empty level can be drawn
	// on immediately, where a stand wants somewhere to stand.
	GetInteractiveToolsContext()->StartTool(MakeToolName(0));

	// TODO(#33): key 7 (land an aircraft) has no editor equivalent. It is not a SelectTool
	// at runtime either - see ARoadBuildController::OnLandAircraft - so wiring it up here
	// needs its own command and its own cursor-to-plane resolution, not a seventh registry
	// entry; out of scope for making the two drivers share ONE tool table.
}

void URoadBuildEdMode::BindCommands()
{
	Super::BindCommands();

	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();

	// The TOOLKIT command list, not ToolCommandList. UEdMode's own BindCommands maps its
	// viewport shortcuts here, and ToolCommandList - despite its comment - is created and
	// then never consulted anywhere in UEdMode.cpp. Bound to the wrong one, Escape was
	// simply unreachable.
	//
	// Called from WITHIN Super::Enter(), before URoadBuildEdMode::Enter()'s own body runs -
	// so this cannot read anything Enter() fills in later. MakeToolName(Index) is a pure
	// function of ToolRegistry() for exactly that reason; see its comment in the header.
	if (Toolkit.IsValid())
	{
		const TSharedRef<FUICommandList>& Commands2 = Toolkit->GetToolkitCommands();
		Commands2->MapAction(Commands.CancelGesture,
			FExecuteAction::CreateUObject(this, &URoadBuildEdMode::CancelActiveGesture));

		// The tool keys go here TOO. RegisterTool binds them into ToolCommandList, which is
		// processed by the viewport and therefore only once the viewport has keyboard
		// focus - so 1/2/3 did nothing until you had clicked in it, and the click that
		// gave it focus also started a road. Bound here as well, they work immediately.
		const TArray<TSharedPtr<FUICommandInfo>> ToolCommands = Commands.ToolCommandsInOrder();
		for (int32 Index = 0; Index < FMath::Min(ToolCommands.Num(), ToolRegistry().Num()); ++Index)
		{
			Commands2->MapAction(ToolCommands[Index], StartToolAction(MakeToolName(Index)));
		}
	}

	if (ToolCommandList.IsValid())
	{
		ToolCommandList->MapAction(Commands.CancelGesture,
			FExecuteAction::CreateUObject(this, &URoadBuildEdMode::CancelActiveGesture));
	}
}

FExecuteAction URoadBuildEdMode::StartToolAction(const FString& ToolName)
{
	return FExecuteAction::CreateLambda([this, ToolName]()
	{
		UE_LOG(LogRoadBuildMode, Log, TEXT("Tool switch requested: %s"), *ToolName);

		// Guarded because both command lists can carry the same key: restarting the tool
		// already running would silently abandon a chain half-drawn.
		UInteractiveToolManager* Manager = GetToolManager();
		if (Manager != nullptr && Manager->GetActiveToolName(EToolSide::Mouse) == ToolName)
		{
			return;
		}

		if (UEditorInteractiveToolsContext* Context = GetInteractiveToolsContext())
		{
			Context->StartTool(ToolName);
		}
	});
}

void URoadBuildEdMode::CancelActiveGesture()
{
	if (UInteractiveToolManager* Manager = GetToolManager())
	{
		if (URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(
			Manager->GetActiveTool(EToolSide::Mouse)))
		{
			Tool->CancelGesture();
		}
	}
}

void URoadBuildEdMode::CreateToolkit()
{
	// Not FModeToolkit: the stock one names no palettes and so draws no buttons.
	Toolkit = MakeShareable(new FRoadBuildModeToolkit);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> URoadBuildEdMode::GetModeCommands() const
{
	return FRoadBuildEdModeCommands::GetCommands();
}

#undef LOCTEXT_NAMESPACE
