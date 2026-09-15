#include "Tool/PlotDrawTool.h"

#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/** How near the cursor must be to an entity's pose to pick it, uu. */
	constexpr double PickRadiusUu = 1000.0;
}

bool FPlotOutlineTarget::Commit(const FToolContext& Context,
	const TArray<FVector2D>& Corners) const
{
	// The facade refuses a plot with no road frontage, and one smaller than a single bay,
	// and says which - so a refusal here leaves the outline in progress rather than costing
	// the player the corners they drew.
	return Context.Target != nullptr
		&& Context.Target->PlaceEntityInPlot(Corners, Modules, Kind) != INDEX_NONE;
}

bool FPlotOutlineTarget::RemoveUnderCursor(const FToolContext& Context) const
{
	if (Context.Target == nullptr)
	{
		return false;
	}

	// BY POSE, not by whether the cursor is inside the plot. The plot is large and a
	// depot's gate is where the player thinks the thing IS - picking by area would make a
	// ctrl-click anywhere in the yard delete the depot, including over the grass they left
	// as yard on purpose.
	const int32 Under = Context.Target->FindEntityAt(Context.Cursor, PickRadiusUu);
	return Under != INDEX_NONE && Context.Target->DeleteEntity(Under);
}

void FPlotOutlineTarget::PreviewRemoval(const FToolContext& Context,
	IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Network() == nullptr)
	{
		return;
	}

	const int32 Under = Context.Target->FindEntityAt(Context.Cursor, PickRadiusUu);
	if (Under == INDEX_NONE)
	{
		return;
	}

	const TArray<FEntityInstance>& Entities = Context.Network()->GetEntities();
	if (!Entities.IsValidIndex(Under))
	{
		return;
	}

	// THE PLOT, when there is one. An entity placed before plots existed - or a stand
	// caught by the same pick - has no outline, and a marker is the honest thing to show
	// rather than an empty polygon that reads as nothing being selected.
	const FEntityInstance& Entity = Entities[Under];
	if (Entity.Outline.Num() >= 3)
	{
		Sink.Polygon(Entity.Outline, EPreviewStyle::Doomed);
	}
	else
	{
		Sink.Marker(Entity.Position, EPreviewStyle::Doomed);
	}
}

FText FPlotDrawTool::GetDisplayName() const
{
	return LOCTEXT("FuelDepotTool", "Fuel depot");
}

#undef LOCTEXT_NAMESPACE
