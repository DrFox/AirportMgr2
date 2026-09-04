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

	TSharedPtr<FUICommandInfo> DrawRoads;
	TSharedPtr<FUICommandInfo> DrawAprons;
	TSharedPtr<FUICommandInfo> PlaceStands;
	TSharedPtr<FUICommandInfo> FindRoutes;

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
