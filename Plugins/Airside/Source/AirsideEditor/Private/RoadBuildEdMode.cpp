#include "RoadBuildEdMode.h"

#include "AirsideEditorLog.h"
#include "Editor.h"
#include "EdModeInteractiveToolsContext.h"
#include "Present/AirsideBuildingsActor.h"
#include "Present/RoadNetworkActor.h"
#include "RoadBuildEdModeCommands.h"
#include "RoadBuildEditorTool.h"
#include "Tool/BuildSession.h"
#include "Toolkits/BaseToolkit.h"

#define LOCTEXT_NAMESPACE "RoadBuildEdMode"

// Every tool switch is logged under LogAirsideEditor, and so is every tool that gets built.
//
// Not decoration: this mode has now produced three separate rounds of "the tool looks
// broken" where the model was correct, and each was diagnosed from screenshots and guesses
// before anything measured the boundary. A tool that does not activate and a tool that
// activates and draws nothing are indistinguishable on screen, and these two lines tell
// them apart outright.

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

UWorld* URoadBuildEdMode::GetWorld() const
{
	// See WorldOverrideForTest's own comment: this only ever has a value when a test set it,
	// since production reaches this class exclusively through the mode manager, which sets
	// up EditorToolsContext long before GetWorld() is ever asked for an answer.
	return WorldOverrideForTest != nullptr ? WorldOverrideForTest.Get() : Super::GetWorld();
}

void URoadBuildEdMode::Enter()
{
	UEdMode::Enter();

	// ISSUE #191/#92-#93: this mode had no FEditorUndoClient at all before now, so an editor
	// Ctrl+Z never reached PostUndo below - see that method's own comment. Registered here,
	// unregistered in Exit(), the same pairing GEditor's own clients use (FEditorModeTools
	// itself is one - see EditorModeManager.h).
	if (GEditor != nullptr)
	{
		GEditor->RegisterForUndo(this);
	}

	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();
	const TArray<TSharedPtr<FUICommandInfo>> ToolCommands = Commands.ToolCommandsInOrder();
	const TConstArrayView<FToolRegistration> Registry = ToolRegistry();

	// NO DRIFT CHECK HERE ANY MORE (issue #105 item 10): ToolCommandsInOrder() is now BUILT
	// FROM ToolRegistry() one entry at a time (FRoadBuildEdModeCommands::RegisterCommands),
	// so the two cannot disagree in count or in order - there is only the one list, read
	// twice. The runtime controller has read that way from the start; this mode's own
	// UI_COMMAND list used to be the one exception, and Airside.Editor.ToolCommandsMatchRegistry
	// (RoadBuildEdModeCommandsTest.cpp) is kept as the regression guard against that coming back.
	FString Banner;
	for (int32 Index = 0; Index < FMath::Min(ToolCommands.Num(), Registry.Num()); ++Index)
	{
		URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(this);
		Builder->ToolIndex = Index;
		RegisterTool(ToolCommands[Index], MakeToolName(Index), Builder);

		// IMMEDIATELY AFTER RegisterTool, not in BindCommands (issue #184): RegisterTool just
		// mapped this SAME command onto this SAME toolkit command list with the engine's own
		// StartTool, and MapAction is a TMap::Add - whichever call runs LAST wins. Calling this
		// here, after, is what makes the reselect guard the one a key press actually reaches;
		// see MapReselectAwareToolCommand's own comment for the whole account.
		MapReselectAwareToolCommand(Index, ToolCommands[Index]);

		// THE REGISTRY'S OWN KEY, not Index + 1. The two agreed only while the table happened
		// to run 1..N with no gaps, and the holding-position tool broke that - it sits at index 6
		// but is bound to EIGHT, because key 7 is "land an aircraft" and is not a tool. The
		// banner would then have advertised "7 Holding point" for a key that does nothing,
		// which is precisely the defect CLAUDE.md records ("4 routes" against an unbound
		// EKeys::Four): a log line describing a mechanism is not evidence of it.
		Banner += FString::Printf(TEXT("%s%s %s"),
			Index == 0 ? TEXT("") : TEXT(", "), *Registry[Index].Key.GetDisplayName().ToString(),
			*Registry[Index].Name.ToString());
	}

	UE_LOG(LogAirsideEditor, Log,
		TEXT("Road Build mode entered. Tools: %s. If a number key does nothing, the palette "
			 "buttons do the same job."),
		*Banner);

	// Roads first, because it is the one that needs no setup - an empty level can be drawn
	// on immediately, where a stand wants somewhere to stand.
	GetInteractiveToolsContext()->StartTool(MakeToolName(0));

	// TODO(#33): key 7 (land an aircraft) has no editor equivalent. It is not a SelectTool
	// at runtime either - see ARoadBuildController::LandAircraftNearViewFocus - so wiring it up here
	// needs its own command and its own cursor-to-plane resolution, not a seventh registry
	// entry; out of scope for making the two drivers share ONE tool table.
}

void URoadBuildEdMode::Exit()
{
	// See Enter's own comment. GEditor can already be null on shutdown; UnregisterForUndo on a
	// client that never registered (or a null GEditor) is a documented no-op, not a hazard, but
	// the null check matches the one Enter's registration already makes rather than trusting
	// that symmetry silently.
	if (GEditor != nullptr)
	{
		GEditor->UnregisterForUndo(this);
	}

	UEdMode::Exit();
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
	//
	// THE TOOL KEYS DO NOT GO HERE ANY MORE (issue #184). They used to, with the comment that
	// RegisterTool binds them into ToolCommandList, "which is processed by the viewport" - false
	// for 5.8: RegisterTool binds them into THIS SAME toolkit list, with the engine's own
	// StartTool, and since this method runs before RegisterTool ever does (see above), the
	// engine's binding always landed second and always won - MapAction is a TMap::Add, which
	// replaces. Mapped instead from Enter(), immediately after each RegisterTool call, where
	// this class's own binding is the one that runs last; see MapReselectAwareToolCommand.
	if (Toolkit.IsValid())
	{
		Toolkit->GetToolkitCommands()->MapAction(Commands.CancelGesture,
			FExecuteAction::CreateUObject(this, &URoadBuildEdMode::CancelActiveGesture));

		// ISSUE #185, same shape as Cancel just above: not a tool command, so RegisterTool
		// never touches this binding and there is no reselect guard to protect - it can be
		// mapped here, once, rather than re-mapped after each RegisterTool the way the tool
		// keys are.
		Toolkit->GetToolkitCommands()->MapAction(Commands.Build,
			FExecuteAction::CreateUObject(this, &URoadBuildEdMode::CommitActiveGesture));

		// ONE PER BuildVerbRegistry() ENTRY (issue #304) - Remove, Insert and Edit, the sticky
		// EGestureMode trio `grep GestureMode AirsideEditor/Private/*.cpp` found NOWHERE in this
		// module before now. TOGGLEBUTTON commands need an IsChecked too, unlike Cancel/Build's
		// plain Button just above - IsVerbActive is what lights the palette entry the way
		// MapReselectAwareToolCommand's own IsChecked lights a tool button.
		const TArray<TSharedPtr<FUICommandInfo>> VerbCommands = Commands.VerbCommandsInOrder();
		for (int32 Index = 0; Index < VerbCommands.Num(); ++Index)
		{
			Toolkit->GetToolkitCommands()->MapAction(VerbCommands[Index],
				FExecuteAction::CreateUObject(this, &URoadBuildEdMode::ApplyVerb, Index),
				FCanExecuteAction(),
				FIsActionChecked::CreateUObject(this, &URoadBuildEdMode::IsVerbActive, Index));
		}
	}

	// ToolCommandList itself is NOT bound to any more, tool keys or Escape: it is the list
	// named in this function's own comment above as "created and then never consulted
	// anywhere in UEdMode.cpp" - a mapping there was dead code, not a second, harmless copy.
}

void URoadBuildEdMode::MapReselectAwareToolCommand(int32 Index, const TSharedPtr<FUICommandInfo>& Command)
{
	if (!Toolkit.IsValid())
	{
		return;
	}

	// GetInteractiveToolsContext() is null in Airside.Editor.ToolCommandBindingSurvivesRegisterTool
	// (no Enter(), no viewport - the same gate StartToolAction's own comment describes), so this
	// degrades to an unchecked binding there rather than refusing to install the fix at all. In
	// production Enter() has already run CreateInteractiveToolsContexts() by the time this is
	// called, so the checked delegate is always live there.
	UEditorInteractiveToolsContext* ToolsContext = GetInteractiveToolsContext();
	const FIsActionChecked IsChecked = ToolsContext != nullptr
		? FIsActionChecked::CreateUObject(ToolsContext,
			&UEditorInteractiveToolsContext::IsToolActive, EToolSide::Mouse, MakeToolName(Index))
		: FIsActionChecked();

	Toolkit->GetToolkitCommands()->MapAction(Command, StartToolAction(Index), FCanExecuteAction(), IsChecked);
}

TSharedPtr<FUICommandList> URoadBuildEdMode::ToolkitCommandsForTest() const
{
	// GetToolkitCommands() returns a TSharedRef, not a TSharedPtr, so the two arms of a ternary
	// against nullptr do not share a common type - hence the explicit if rather than one line.
	if (!Toolkit.IsValid())
	{
		return nullptr;
	}
	return Toolkit->GetToolkitCommands();
}

FToolContext URoadBuildEdMode::MakeReselectContext(bool bRemoveModifier, bool bInsertModifier) const
{
	// FRunwayTool::OnReselect calls NextWidth, which since issue #78 asks Context.Target for
	// the profile count instead of reading content directly - so a reselect with a null
	// Target here silently stopped the width cycling in the editor mode while PIE's
	// ARoadBuildController (which always resolves a target before calling SelectTool) kept
	// working. That split is the exact thing this mode exists to close (see GetSession's own
	// comment) and it reopened here by omission - a plain FToolContext used to be enough
	// because NextWidth ignored it. Resolved the same way
	// URoadBuildEditorTool::ResolveTarget does: found or created, never left for the player
	// to drag one in by hand first.
	//
	// ITS OWN FUNCTION, not inlined into StartToolAction, so
	// Airside.Editor.ToolStateOutlivesTheToolInstance can call the exact resolution a
	// reselect uses without needing a live UInteractiveToolManager - GetToolManager() and
	// GetInteractiveToolsContext() both return null on a bare NewObject<URoadBuildEdMode>
	// with no Enter(), which is why that test cannot drive StartToolAction's lambda directly
	// (see its own header comment) and drives this instead.
	FToolContext Context;
	ARoadNetworkActor* Road = ARoadNetworkActor::FindOrCreate(GetWorld());
	// AND ITS BUILDINGS, created beside it: a level with a road network and no buildings
	// actor draws no depots and says nothing about why.
	AAirsideBuildingsActor::FindOrCreate(GetWorld(), Road);
	Context.Target = Road;
	Context.bRemoveModifier = bRemoveModifier;
	Context.bInsertModifier = bInsertModifier;
	return Context;
}

FExecuteAction URoadBuildEdMode::StartToolAction(int32 ToolIndex)
{
	return FExecuteAction::CreateLambda([this, ToolIndex]()
	{
		const FString ToolName = MakeToolName(ToolIndex);
		UE_LOG(LogAirsideEditor, Log, TEXT("Tool switch requested: %s"), *ToolName);

		// Still refused, and for the reason it always was: restarting the tool already
		// running would silently abandon a chain half-drawn.
		//
		// BUT REFUSING TO RESTART IS NOT THE SAME AS DOING NOTHING, which is what this did
		// before. The key pressed on the already-active tool is a RESELECT - the gesture
		// that cycles a runway's width, surface and approach - and it went nowhere in the
		// editor while working in PIE, because the runtime driver routes the same press
		// through FBuildSession::SelectTool and this returned early.
		UInteractiveToolManager* Manager = GetToolManager();
		if (Manager != nullptr && Manager->GetActiveToolName(EToolSide::Mouse) == ToolName)
		{
			// The session's own SelectTool sees Index == ActiveTool and calls OnReselect.
			//
			// MODIFIERS ARE NO LONGER LEFT AT THEIR DEFAULTS (issue #191/#92-#93 - "reselect
			// modifiers only in PIE"). PIE always reads live Shift/Ctrl for every SelectTool
			// call, switch or reselect, so Ctrl+the runway key cycles its approach there
			// (FRunwayTool::OnReselect); this mode has no keyboard of its own to poll, so it
			// asks the tool that is ACTUALLY ACTIVE right now - the one whose Drag/Hover
			// behaviours have been reporting modifier state all along - for what it currently
			// holds. The active tool for ToolName is exactly this branch's own subject: if
			// Manager->GetActiveToolName(...) == ToolName, GetActiveTool(...) is that same
			// instance, the same cast CancelActiveGesture/CommitActiveGesture already make.
			const URoadBuildEditorTool* ActiveEditorTool =
				Cast<URoadBuildEditorTool>(Manager->GetActiveTool(EToolSide::Mouse));
			const bool bRemoveModifier = ActiveEditorTool != nullptr && ActiveEditorTool->IsRemoveModifierHeld();
			const bool bInsertModifier = ActiveEditorTool != nullptr && ActiveEditorTool->IsInsertModifierHeld();

			// TARGET IS NOT LEFT AT ITS DEFAULT, though - see MakeReselectContext for why.
			Session.SelectTool(ToolIndex, MakeReselectContext(bRemoveModifier, bInsertModifier));
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

void URoadBuildEdMode::CommitActiveGesture()
{
	if (UInteractiveToolManager* Manager = GetToolManager())
	{
		if (URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(
			Manager->GetActiveTool(EToolSide::Mouse)))
		{
			Tool->CommitGesture();
		}
	}
}

void URoadBuildEdMode::ApplyVerb(int32 VerbIndex)
{
	const TConstArrayView<FBuildVerbRegistration> Registry = BuildVerbRegistry();
	if (!Registry.IsValidIndex(VerbIndex))
	{
		return;
	}

	// SAME CAST-AND-FORWARD as CancelActiveGesture/CommitActiveGesture just above: what the
	// verb MEANS belongs to the shared tool (URoadBuildEditorTool::ApplyVerb), not to this
	// mode, which only finds which instance is currently active.
	if (UInteractiveToolManager* Manager = GetToolManager())
	{
		if (URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(
			Manager->GetActiveTool(EToolSide::Mouse)))
		{
			Tool->ApplyVerb(Registry[VerbIndex]);
		}
	}
}

bool URoadBuildEdMode::IsVerbActive(int32 VerbIndex) const
{
	const TConstArrayView<FBuildVerbRegistration> Registry = BuildVerbRegistry();

	// THE SESSION, not the active tool instance - Session.GetGestureMode() answers this
	// whether or not a URoadBuildEditorTool happens to be active right now, the same reason
	// GetSession() rather than a per-tool flag is what MapReselectAwareToolCommand's own
	// IsChecked would read if a tool question, rather than a session one, were being asked.
	return Registry.IsValidIndex(VerbIndex) && Registry[VerbIndex].IsActive(Session);
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

void URoadBuildEdMode::PostUndo(bool bSuccess)
{
	DeactivateActiveToolOnUndo(bSuccess);
}

void URoadBuildEdMode::PostRedo(bool bSuccess)
{
	DeactivateActiveToolOnUndo(bSuccess);
}

void URoadBuildEdMode::DeactivateActiveToolOnUndo(bool bSuccess)
{
	// MIRRORS ARoadBuildController::OnUndo/OnRedo's OWN GUARD: those only call OnDeactivate
	// once Target->Undo()/Redo() has actually returned true - "nothing happened" gets a log
	// line and nothing else. bSuccess here answers the same question for GEditor's transactor
	// (see FEditorUndoClient::PostUndo's own doc comment), so a failed undo/redo leaves the
	// active tool alone rather than abandoning a part-drawn chain over a transaction that
	// never actually applied.
	if (!bSuccess)
	{
		return;
	}

	if (UInteractiveToolManager* Manager = GetToolManager())
	{
		if (URoadBuildEditorTool* Tool = Cast<URoadBuildEditorTool>(
			Manager->GetActiveTool(EToolSide::Mouse)))
		{
			Tool->DeactivateOnUndo();
		}
	}
}

#undef LOCTEXT_NAMESPACE
