#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

/** One command per build tool, so each gets a button in the mode's palette. */
class FRoadBuildEdModeCommands : public TCommands<FRoadBuildEdModeCommands>
{
public:
	FRoadBuildEdModeCommands();

	virtual void RegisterCommands() override;

	/** Commands grouped by palette name, for UEdMode::GetModeCommands. */
	static TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetCommands();

	/**
	 * One command per tool, in ToolRegistry() order.
	 *
	 * The ONE list both GetCommands()'s palette and URoadBuildEdMode's RegisterTool/
	 * BindCommands loops read from - see CLAUDE.md's "check where a list is CONSUMED, not
	 * where it is declared". A second hard-coded copy of this ordering (the palette array
	 * used to be one, spelled out again inside GetCommands()) is exactly the defect this
	 * class exists to prevent: nothing checked that copy against this one, or against
	 * ToolRegistry() itself.
	 *
	 * BUILT FROM ToolRegistry() IN RegisterCommands() (issue #105 item 10), not nine
	 * hand-written UI_COMMAND fields any more - see that function's own comment. What used to
	 * be SelectEntities/DrawRoads/DrawAprons/PlaceStands/DrawGuidelines/PlaceRunways/
	 * PlaceHoldingPoint/DrawServiceRoads/PlaceFuelDepots is ToolCommands[0..8], index for
	 * index with ToolRegistry(). NEEDS RESAVE: nothing - editor keybindings are not a content
	 * asset - but a user who customised one of these nine commands' keys in the editor's
	 * keybinding settings will find it reset to the tool's ToolRegistry() key, since the
	 * command's internal id changed from e.g. "SelectEntities" to "Select" (its ToolRegistry
	 * Name) along with this move.
	 */
	TArray<TSharedPtr<FUICommandInfo>> ToolCommandsInOrder() const { return ToolCommands; }

	/**
	 * One command per BuildVerbRegistry() entry (Remove/Insert/Edit), in registry order - the
	 * SAME shape ToolCommandsInOrder is, applied to the sticky EGestureMode trio issue #304
	 * found unreachable in the editor (`grep GestureMode AirsideEditor/Private/*.cpp` was 0).
	 * Airside.Editor.VerbCommandsMatchRegistry is the twin of
	 * Airside.Editor.ToolCommandsMatchRegistry, guarding this list the same way.
	 */
	TArray<TSharedPtr<FUICommandInfo>> VerbCommandsInOrder() const { return VerbCommands; }

	/**
	 * Ends the gesture in progress - a road chain, a half-drawn apron.
	 *
	 * Exists because RIGHT-CLICK CANNOT DO THIS IN THE EDITOR. At runtime right-click
	 * cancels, but an editor viewport has already claimed it for the context menu, so a
	 * chain started there could never be ended and both tools felt broken. Escape is the
	 * editor's own idiom for the same thing. NOT in ToolRegistry(): it is not a tool, so it
	 * stays a hand-written UI_COMMAND like before.
	 */
	TSharedPtr<FUICommandInfo> CancelGesture;

	/**
	 * Commits the gesture the active tool has staged - the fuel depot's last step.
	 *
	 * ISSUE #185: this command did not exist at all. PIE has always had `edit.build` on
	 * Enter (BuildActions.cpp) reaching `IBuildTool::OnCommit`, but the editor mode's command
	 * set was the nine tool commands plus Cancel and nothing else - "list declared, consumer
	 * missing" in its fourth form here (see CLAUDE.md's "Check where a list is CONSUMED").
	 * A fuel depot could be drawn in the editor and never placed.
	 *
	 * SAME KEY AS PIE (Enter), for the reason CancelGesture's own comment gives about Escape:
	 * a player who has learned one driver's verb should not have to learn a second key for
	 * the same thing in the other. NOT in ToolRegistry(): like Cancel, it selects no tool, so
	 * it stays a hand-written UI_COMMAND rather than a registry entry.
	 */
	TSharedPtr<FUICommandInfo> Build;

private:
	/** See ToolCommandsInOrder's own comment for why this replaced nine named fields. */
	TArray<TSharedPtr<FUICommandInfo>> ToolCommands;

	/** See VerbCommandsInOrder's own comment. */
	TArray<TSharedPtr<FUICommandInfo>> VerbCommands;
};
