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
	 * The six tool commands, in ToolRegistry() order.
	 *
	 * The ONE list both GetCommands()'s palette and URoadBuildEdMode's RegisterTool/
	 * BindCommands loops read from - see CLAUDE.md's "check where a list is CONSUMED, not
	 * where it is declared". A second hard-coded copy of this ordering (the palette array
	 * used to be one, spelled out again inside GetCommands()) is exactly the defect this
	 * class exists to prevent: nothing checked that copy against this one, or against
	 * ToolRegistry() itself.
	 */
	TArray<TSharedPtr<FUICommandInfo>> ToolCommandsInOrder() const;

	TSharedPtr<FUICommandInfo> DrawRoads;
	TSharedPtr<FUICommandInfo> DrawAprons;
	TSharedPtr<FUICommandInfo> PlaceStands;
	TSharedPtr<FUICommandInfo> FindRoutes;

	/**
	 * Issue #33: the editor previously had four tools against the runtime's six, with no
	 * way to draw a guideline link or lay a runway outside PIE. One command per
	 * ToolRegistry() entry now, in the same order, so the two cannot drift apart again.
	 */
	TSharedPtr<FUICommandInfo> DrawGuidelines;
	TSharedPtr<FUICommandInfo> PlaceRunways;

	/**
	 * Ends the gesture in progress - a road chain, a half-drawn apron.
	 *
	 * Exists because RIGHT-CLICK CANNOT DO THIS IN THE EDITOR. At runtime right-click
	 * cancels, but an editor viewport has already claimed it for the context menu, so a
	 * chain started there could never be ended and both tools felt broken. Escape is the
	 * editor's own idiom for the same thing.
	 */
	TSharedPtr<FUICommandInfo> CancelGesture;

};
