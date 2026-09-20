#include "Tool/EditTool.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FEditTool::GetDisplayName() const
{
	return LOCTEXT("EditTool", "Edit");
}

void FEditTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	// Handles and the drag arrive next. Empty rather than absent so the seam exists from
	// the first build and the overlay has something to call - a preview added later to a
	// tool the overlay never asked to draw is how a mode ends up invisible.
}

#undef LOCTEXT_NAMESPACE
