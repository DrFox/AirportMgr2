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
};
