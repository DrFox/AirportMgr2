#include "Tool/ApronDrawTool.h"

#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "Airside"

bool FApronOutlineTarget::Commit(const FToolContext& Context,
	const TArray<FVector2D>& Corners) const
{
	// The facade refuses a self-crossing outline on its own - it owns the triangulator's
	// contract - so a refusal here leaves the outline in progress rather than throwing
	// away the work.
	return Context.Target != nullptr
		&& Context.Target->AddApron(Corners) != INDEX_NONE;
}

bool FApronOutlineTarget::RemoveUnderCursor(const FToolContext& Context) const
{
	if (Context.Target == nullptr)
	{
		return false;
	}

	// No RebuildMesh() on success any more - DeleteApron notifies on commit (issue #77).
	const int32 Under = Context.Target->FindApronAt(Context.Cursor);
	return Under != INDEX_NONE && Context.Target->DeleteApron(Under);
}

void FApronOutlineTarget::PreviewRemoval(const FToolContext& Context,
	IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Network() == nullptr)
	{
		return;
	}

	const int32 Under = Context.Target->FindApronAt(Context.Cursor);
	if (Under == INDEX_NONE)
	{
		return;
	}

	const TArray<FApronSurface>& Aprons = Context.Network()->GetAprons();
	if (Aprons.IsValidIndex(Under))
	{
		Sink.Polygon(Aprons[Under].Outline, EPreviewStyle::Doomed);
	}
}

FText FApronDrawTool::GetDisplayName() const
{
	return LOCTEXT("ApronTool", "Apron");
}

#undef LOCTEXT_NAMESPACE
