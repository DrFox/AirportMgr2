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
FString URoadBuildEdMode::RouteToolName = TEXT("RoadNet_FindRoutes");

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
	RegisterTool(Commands.FindRoutes, RouteToolName, MakeBuilder(ERoadBuildToolKind::Route));

	// Roads first, because it is the one that needs no setup - an empty level can be drawn
	// on immediately, where a stand wants somewhere to stand.
	GetInteractiveToolsContext()->StartTool(RoadToolName);
}

void URoadBuildEdMode::BindCommands()
{
	Super::BindCommands();

	const FRoadBuildEdModeCommands& Commands = FRoadBuildEdModeCommands::Get();

	// The TOOLKIT command list, not ToolCommandList. UEdMode's own BindCommands maps its
	// viewport shortcuts here, and ToolCommandList - despite its comment - is created and
	// then never consulted anywhere in UEdMode.cpp. Bound to the wrong one, Escape was
	// simply unreachable.
	if (Toolkit.IsValid())
	{
		const TSharedRef<FUICommandList>& Commands2 = Toolkit->GetToolkitCommands();
		Commands2->MapAction(Commands.CancelGesture,
			FExecuteAction::CreateUObject(this, &URoadBuildEdMode::CancelActiveGesture));

		// The tool keys go here TOO. RegisterTool binds them into ToolCommandList, which is
		// processed by the viewport and therefore only once the viewport has keyboard
		// focus - so 1/2/3 did nothing until you had clicked in it, and the click that
		// gave it focus also started a road. Bound here as well, they work immediately.
		Commands2->MapAction(Commands.DrawRoads, StartToolAction(RoadToolName));
		Commands2->MapAction(Commands.DrawAprons, StartToolAction(ApronToolName));
		Commands2->MapAction(Commands.PlaceStands, StartToolAction(StandToolName));
		Commands2->MapAction(Commands.FindRoutes, StartToolAction(RouteToolName));
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
	Toolkit = MakeShareable(new FModeToolkit);
}

TMap<FName, TArray<TSharedPtr<FUICommandInfo>>> URoadBuildEdMode::GetModeCommands() const
{
	return FRoadBuildEdModeCommands::GetCommands();
}

#undef LOCTEXT_NAMESPACE
