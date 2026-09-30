#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "Tool/BuildSession.h"
#include "Tools/UEdMode.h"
#include "RoadBuildEdMode.generated.h"

class SWidget;

/**
 * Editor mode hosting the same build tools the runtime controller drives.
 *
 * The point of it is persistence. The runtime tool is a PlayerController, so it only exists
 * in PIE and everything built there is discarded when play stops. Here the edits land in the
 * real editor world and save with the level, which is what makes an airport authorable at
 * all rather than a thing you rebuild every session.
 *
 * It registers one tool per ToolRegistry() entry, under THAT ENTRY'S OWN KEY, so a tool is
 * reached by the same number in the editor as in play - see issue #33, which added the
 * guideline and runway tools here; before it this mode stopped at four, and the two build
 * drivers had quietly drifted apart.
 *
 * THE KEYS ARE NOT CONTIGUOUS, and nothing here may assume they are. Holding-position is the
 * seventh entry and is bound to EIGHT, because seven is "land an aircraft" - a decision
 * taken at the cursor, not a tool, so it is not in the table at all. Anywhere a key is
 * printed or bound it comes from Registry[Index].Key; Index + 1 is a different number and
 * was briefly used as if it were the same one.
 *
 * Nothing about what a click MEANS lives here - that is all in the shared tools.
 */
UCLASS()
class URoadBuildEdMode : public UEdMode, public FEditorUndoClient
{
	GENERATED_BODY()

public:
	const static FEditorModeID EM_RoadBuild;

	URoadBuildEdMode();

	/**
	 * THE ONE SESSION, and the reason it lives on the MODE rather than on the tool.
	 *
	 * It used to be a member of URoadBuildEditorTool, and the InteractiveTools framework
	 * builds a NEW tool object on every activation - so the session, and with it every
	 * FRunwayTool's WidthIndex, was destroyed and rebuilt each time a tool was picked. Two
	 * consequences, both invisible until somebody tried to lay a wider runway:
	 *
	 *   a fresh session always has ActiveTool 0, so selecting a tool was always a SWITCH and
	 *   IBuildTool::OnReselect could never be reached in the editor at all;
	 *
	 *   and even had it been reached, the width it chose would have gone with the session.
	 *
	 * The runtime driver has always held one long-lived FBuildSession on the
	 * PlayerController, which is why width, surface and approach cycle in PIE and did not
	 * here. That is the drift issue #33 named; this closes it for tool STATE as #33 closed
	 * it for the tool LIST.
	 */
	FBuildSession& GetSession() { return Session; }

	/**
	 * The context StartToolAction's reselect passes to FBuildSession::SelectTool - see its
	 * own .cpp comment for why a null Target there silently broke runway width cycling in
	 * this mode (issue #78's review), and for why this is public: a headless test cannot
	 * drive StartToolAction's lambda at all (it is gated on a live
	 * UInteractiveToolManager/UEditorInteractiveToolsContext, neither of which exists without
	 * Enter() and a real editor viewport), so
	 * Airside.Editor.ToolStateOutlivesTheToolInstance calls this directly instead of building
	 * an unrelated target of its own - which is what let a prior version of that test pass
	 * while the mode itself stayed broken.
	 *
	 * bRemoveModifier/bInsertModifier DEFAULT FALSE, not read from anywhere here: the MODE has
	 * no keyboard of its own to poll (issue #191/#92-#93 - "reselect modifiers only in PIE").
	 * StartToolAction supplies the real answer by asking the ACTIVE URoadBuildEditorTool
	 * instance for the modifiers ITF's own behaviours already told it about
	 * (IsRemoveModifierHeld/IsInsertModifierHeld) - the same cast CancelActiveGesture and
	 * CommitActiveGesture already make. The defaults stay false so
	 * Airside.Editor.ToolStateOutlivesTheToolInstance's plain reselect, which has no tool
	 * instance to ask, keeps behaving exactly as it did before this parameter existed.
	 */
	FToolContext MakeReselectContext(bool bRemoveModifier = false, bool bInsertModifier = false) const;

	/**
	 * (Re)binds ToolRegistry()[Index]'s command to StartToolAction(Index) on the toolkit's own
	 * command list, replacing whatever is mapped there already.
	 *
	 * THE FIX FOR ISSUE #184. `UEdMode::Enter()` calls `BindCommands()` before running this
	 * class's own `Enter()` body, so a binding installed in `BindCommands()` is always in place
	 * BEFORE `RegisterTool()` runs for that same command - and `RegisterTool()` maps the engine's
	 * own `StartTool` onto the exact same toolkit command list. `UICommandList::MapAction` is a
	 * `TMap::Add`, which replaces: whichever call happens last wins, so the engine's binding
	 * always overwrote this one and the reselect guard in `StartToolAction` was unreachable.
	 * Called from `Enter()` immediately after each `RegisterTool()` instead, so this one runs
	 * last and wins.
	 *
	 * `FIsActionChecked` still asks the ENGINE whether this tool is the active one
	 * (`UInteractiveToolsContext::IsToolActive`, inherited onto `UEditorInteractiveToolsContext`)
	 * rather than a flag kept here - re-pointing the execute action must not also silently stop
	 * a palette button lighting up while its own tool is running.
	 *
	 * PUBLIC for the same reason `MakeReselectContext` is: a headless test cannot reach the real
	 * `RegisterTool` (it needs a live `UEditorInteractiveToolsContext`, which only `Enter()` with
	 * a real editor viewport creates), so `Airside.Editor.ToolCommandBindingSurvivesRegisterTool`
	 * simulates `RegisterTool`'s own `MapAction` with a spy and calls this directly afterwards,
	 * in the same order `Enter()` does.
	 */
	void MapReselectAwareToolCommand(int32 Index, const TSharedPtr<FUICommandInfo>& Command);

	/**
	 * The toolkit's own command list, once `CreateToolkit()` has run - for the same test.
	 * `Toolkit` is otherwise private to `UEdMode` and unreachable without a full `Enter()`.
	 */
	TSharedPtr<FUICommandList> ToolkitCommandsForTest() const;

	/**
	 * What the toolkit's palettes DRAW, palette by palette in GetToolPaletteNames order: each
	 * palette run through FModeToolkit::BuildToolPalette - the engine's own consumer, which looks
	 * GetModeCommands() up by the name GetToolPaletteNames gave it - into a real toolbar builder,
	 * and the command behind every block read back off the result.
	 *
	 * NOT GetModeCommands() ITSELF (issue #440): that is the DECLARED list, and this mode has
	 * shipped "declared, consumed by nobody" three times - a "Build" palette no toolkit asked for
	 * (FModeToolkit::GetToolPaletteNames is empty by default) was the first. Needs CreateToolkit()
	 * only: the toolkit is handed this mode as its owner at construction, which is what
	 * BuildToolPalette reads (and what Init would have set, to the same mode, in a real Enter()).
	 */
	TArray<TPair<FName, TArray<TSharedPtr<const FUICommandInfo>>>> DrawnPalettesForTest() const;

	/** FModeToolkit::GetInlineContent - what the mode panel shows under the palettes - once
	 *  CreateToolkit() has run; null before. For the test that the variant rows are in it. */
	TSharedPtr<SWidget> InlineContentForTest() const;

	/**
	 * The active tool's variant rows - a taxiway or road's Mode (Build / Upgrade), width and
	 * surface, a runway's, a stand's pavement - exactly as FBuildSession::GetActiveVariantAxes
	 * numbers them. What the mode panel's variant section draws, polled every tick the way PIE's
	 * UBuildBarWidget::RefreshVariantsFor polls ARoadBuildController::GetActiveVariantAxes.
	 *
	 * ISSUE #440: the variant rows were PIE-only. FBuildSession's variant API had one production
	 * caller, the controller, so in the editor Upgrade mode was unreachable (SelectVariant is the
	 * only writer of FRoadDrawTool's Mode, and OnReselect never steps it) and a stand's pavement
	 * was whatever a PIE session last wrote to the preferences ini.
	 * ENFORCED BY: Check-Architecture rule 47 (session-api-both-drivers) - the editor must call every
	 * FBuildSession mutator the controller calls, SelectActiveVariant included.
	 *
	 * THE TARGET AND NOTHING ELSE, found and never created - ARoadBuildController::
	 * MakeVariantContext's reason (a variant answer reads the target's content lists only), and a
	 * per-tick poll must not spawn an actor. No road network yet means no rows, not an error.
	 */
	void GetActiveVariantAxes(TArray<FToolVariantAxis>& Out) const;

	/**
	 * Picks Option on Axis for the active tool - the variant panel's click. Forwards to the
	 * ACTIVE URoadBuildEditorTool's SelectVariant, the cast-and-forward ApplyVerb and
	 * CancelActiveGesture already make, because a pick can touch the graph (a Mode switch ends a
	 * part-drawn chain, which drops its stranded node) and the tool owns the transaction that
	 * makes that undoable. False, changing nothing, when no build tool is active.
	 */
	bool SelectActiveVariant(int32 Axis, int32 Option);

	/**
	 * Substitutes for GetWorld()'s usual answer (EditorToolsContext->GetWorld()), which is
	 * unconditionally null without Enter() and a real FEditorModeTools/viewport - see
	 * GetWorld's own comment. Null in production; nothing here ever sets it, and
	 * MakeReselectContext's ARoadNetworkActor::FindOrCreate(GetWorld()) is exactly why
	 * Airside.Editor.ToolStateOutlivesTheToolInstance needs a real world to resolve a real
	 * target against, without the weight of registering this mode with a live editor.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UWorld> WorldOverrideForTest = nullptr;

	/** See WorldOverrideForTest. */
	virtual UWorld* GetWorld() const override;

	virtual void Enter() override;
	virtual void Exit() override;
	virtual void CreateToolkit() override;
	virtual TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetModeCommands() const override;
	virtual void BindCommands() override;

	// --- FEditorUndoClient ---------------------------------------------------------------
	//
	// PIE calls Tool->OnDeactivate on Undo/Redo/Clear/load (the facade's OnReplaced, answered by
	// ARoadBuildController::OnNetworkReplaced since #426 - OnUndo/OnRedo/OnClearNetwork by hand
	// before it) because the tool may be part-way through a chain built on a graph node
	// the undo/redo just changed underneath it. THIS MODE DOES NOT LISTEN TO OnReplaced: in an
	// editor world the facade has no history (HistoryForEdit), no runtime loads, and nothing
	// here calls ClearNetwork - GEditor's transactor below is the one replacement it meets.
	// ENFORCED BY: Check-Architecture rule 4 (allowed callers of ClearNetwork/RestoreInPlace - none in AirsideEditor)
	// This mode had NO EQUIVALENT AT ALL until issue
	// #191/#92-#93 (`grep PostEditUndo|FEditorUndoClient|PostUndo` found zero hits) - so an
	// editor Ctrl+Z that removed a node FRoadDrawTool was chaining from left the tool still
	// holding it, silently. Registered in Enter(), unregistered in Exit() - GEditor's undo
	// client list does not know to drop a mode that never told it to stop listening.
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
	/** The shared body of PostUndo/PostRedo: both mean the same thing to the active tool -
	 *  whatever it had part-drawn may no longer make sense - so there is one implementation,
	 *  not two copies that could answer differently. */
	void DeactivateActiveToolOnUndo(bool bSuccess);

	/**
	 * The registered ITF tool name for ToolRegistry()[Index] - "Airside_Road" and so on.
	 *
	 * A pure function of the registry rather than a static array filled in during Enter():
	 * UEdMode::Enter() calls BindCommands() - virtual, so it reaches THIS class's override -
	 * before returning to run the rest of URoadBuildEdMode::Enter() where the tools get
	 * registered. An array populated there would still be empty the first time BindCommands
	 * asked it for a name.
	 */
	static FString MakeToolName(int32 Index);

	/** Escape: tell whichever build tool is active to drop what it was holding. */
	void CancelActiveGesture();

	/**
	 * Enter: tell whichever build tool is active to commit what it has staged.
	 *
	 * ISSUE #185. Mirrors CancelActiveGesture exactly - same cast to URoadBuildEditorTool off
	 * the active tool, same reason the mode itself does no more than forward: what a commit
	 * MEANS (the fuel depot's OnCommit places it; every other tool ignores the call) is the
	 * shared tool's business, not this driver's.
	 */
	void CommitActiveGesture();

	/**
	 * Enter/Insert/Remove's mapped command (issue #304): forwards Verb Index of
	 * BuildVerbRegistry() to the active URoadBuildEditorTool's own ApplyVerb, the identical
	 * cast-and-forward shape CancelActiveGesture/CommitActiveGesture already use for the two
	 * verbs that predate this table.
	 */
	void ApplyVerb(int32 VerbIndex);

	/** BuildVerbRegistry()[VerbIndex].IsActive(Session) - what lights the palette's toggle
	 *  button, the way MapReselectAwareToolCommand's own IsChecked lights a tool button. */
	bool IsVerbActive(int32 VerbIndex) const;

	/** BuildVerbRegistry()[VerbIndex].IsEnabled(Session) - greys the palette button (and refuses
	 *  its key) exactly where PIE's bar greys the row and TryRun refuses it: Edit, while the lit
	 *  tool exposes no handles (issue #440; the editor's toggle used to be always enabled). */
	bool IsVerbEnabled(int32 VerbIndex) const;

	/**
	 * A Snap / Snap to palette button (issue #440): applies SnapToggleRegistry()[Index] to
	 * this level's airport - ARoadBuildController::ApplySnapToggle's twin, from the same table.
	 *
	 * A TRANSACTION, unlike PIE: GuideSources is a UPROPERTY on the level's ARoadNetworkActor, so
	 * a switch is a level edit - Modify() is what makes Ctrl+Z reach it and the level save it, the
	 * same as the Details panel's edit of the same field. Found, never created, like
	 * GetActiveVariantAxes: a switch with no airport says so in the log and does nothing.
	 */
	void ApplySnapToggle(int32 Index);

	/** Whether SnapToggleRegistry()[Index] is lit on this level's airport - the palette's check. */
	bool IsSnapToggleOn(int32 Index) const;

	/**
	 * Starts the tool at this registry index - or, when it is already running, RESELECTS it.
	 *
	 * Taking the index rather than the name because a reselect has to reach the session, and
	 * the session speaks indices. Restarting the running tool is still refused: that would
	 * silently abandon a half-drawn chain, which is what the old guard was protecting. What
	 * changed is that refusing to restart no longer means doing nothing.
	 */
	FExecuteAction StartToolAction(int32 ToolIndex);

	/** Shared by every editor tool instance - see GetSession. */
	FBuildSession Session;
};
