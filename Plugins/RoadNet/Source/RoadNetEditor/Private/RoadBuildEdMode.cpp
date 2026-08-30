#include "RoadBuildEdMode.h"

#include "EdModeInteractiveToolsContext.h"
#include "RoadBuildEdModeCommands.h"
#include "RoadBuildEditorTool.h"
#include "Toolkits/BaseToolkit.h"

#define LOCTEXT_NAMESPACE "RoadBuildEdMode"

const FEditorModeID URoadBuildEdMode::EM_RoadBuild = TEXT("EM_RoadBuild");

FString URoadBuildEdMode::RoadToolName = TEXT("RoadNet_DrawRoads");
FString URoadBuildEdMode::ApronToolName = TEXT("RoadNet_DrawAprons");
FString URoadBuildEdMode::StandToolName = TEXT("RoadNet_PlaceStands");

URoadBuildEdMode::URoadBuildEdMode()
{
	Info = FEditorModeInfo(
		EM_RoadBuild,
		LOCTEXT("ModeName", "Road Build"),
		FSlateIcon(),
		true);
}

void URoadBuildEdMode::Enter()
{
	UEdMode::Enter();

	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();

	auto MakeBuilder = [this](ERoadBuildToolKind Kind)
	{
		URoadBuildEditorToolBuilder* Builder = NewObject<URoadBuildEditorToolBuilder>(this);
		Builder->Kind = Kind;
		return Builder;
	};

	RegisterTool(Commands.DrawRoads, RoadToolName, MakeBuilder(ERoadBuildToolKind::Road));
	RegisterTool(Commands.DrawAprons, ApronToolName, MakeBuilder(ERoadBuildToolKind::Apron));
	RegisterTool(Commands.PlaceStands, StandToolName, MakeBuilder(ERoadBuildToolKind::Stand));

	// Roads first, because it is the one that needs no setup - an empty level can be drawn
	// on immediately, where a stand wants somewhere to stand.
	GetInteractiveToolsContext()->StartTool(RoadToolName);
}

void URoadBuildEdMode::CreateToolkit()
{
	Toolkit = MakeShareable(new FModeToolkit);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> URoadBuildEdMode::GetModeCommands() const
{
	return FRoadBuildEdModeCommands::GetCommands();
}

#undef LOCTEXT_NAMESPACE
