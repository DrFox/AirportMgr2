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
 * It registers three tools, one per IBuildTool, keyed 1, 2 and 3 to match the runtime
 * shortcuts. Nothing about what a click MEANS lives here - that is all in the shared tools.
 */
UCLASS()
class URoadBuildEdMode : public UEdMode
{
	GENERATED_BODY()

public:
	const static FEditorModeID EM_RoadBuild;

	static FString RoadToolName;
	static FString ApronToolName;
	static FString StandToolName;

	URoadBuildEdMode();

	virtual void Enter() override;
	virtual void CreateToolkit() override;
	virtual TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> GetModeCommands() const override;
};
