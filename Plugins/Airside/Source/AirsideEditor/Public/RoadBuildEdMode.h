#pragma once

#include "CoreMinimal.h"
#include "Tools/UEdMode.h"
#include "RoadBuildEdMode.generated.h"

/**
 * Editor mode hosting the same build tools the runtime controller drives.
 *
 * The point of it is persistence. The runtime tool is a PlayerController, so it only exists
 * in PIE and everything built there is discarded when play stops. Here the edits land in the
 * real editor world and save with the level, which is what makes an airport authorable at
 * all rather than a thing you rebuild every session.
 *
 * It registers one tool per ToolRegistry() entry, keyed 1 through 6 to match the runtime
 * shortcuts - see issue #33, which added the guideline and runway tools here; before it
 * this mode stopped at four, and the two build drivers had quietly drifted apart. Nothing
 * about what a click MEANS lives here - that is all in the shared tools.
 */
UCLASS()
class URoadBuildEdMode : public UEdMode
{
	GENERATED_BODY()

public:
	const static FEditorModeID EM_RoadBuild;

	URoadBuildEdMode();

	virtual void Enter() override;
	virtual void CreateToolkit() override;
	virtual TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetModeCommands() const override;
	virtual void BindCommands() override;

private:
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

	/** The four (now six) commands, in ToolRegistry() order. Shared by Enter and BindCommands so the two cannot list them in two different orders. */
	static TArray<TSharedPtr<FUICommandInfo>> ToolCommandsInOrder(const class FRoadBuildEdModeCommands& Commands);

	/** Escape: tell whichever build tool is active to drop what it was holding. */
	void CancelActiveGesture();

	/** Starts a tool by name, unless it is already the active one. */
	FExecuteAction StartToolAction(const FString& ToolName);
};
