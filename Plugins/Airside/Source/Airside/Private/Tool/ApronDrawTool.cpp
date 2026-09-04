#include "Tool/ApronDrawTool.h"

#include "Model/RoadApron.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/RoadGeom.h"

#define LOCTEXT_NAMESPACE "Airside"

// --- Idle -----------------------------------------------------------------------------

TUniquePtr<IApronDrawState> FApronIdleState::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return nullptr;
	}

	// Ctrl on open ground with nothing part-drawn removes the apron under the cursor.
	if (Context.bRemoveModifier)
	{
		const int32 Under = Context.Target->FindApronAt(Context.Cursor);
		if (Under != INDEX_NONE && Context.Target->DeleteApron(Under))
		{
			Context.Target->RebuildMesh();
		}
		return nullptr;
	}

	return MakeUnique<FApronOutliningState>(Context.Cursor);
}

TUniquePtr<IApronDrawState> FApronIdleState::OnCancel(const FToolContext& Context)
{
	return nullptr;
}

void FApronIdleState::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr)
	{
		return;
	}

	// Aiming a removal: outline the apron that would go.
	if (Context.bRemoveModifier)
	{
		const int32 Under = Context.Target->FindApronAt(Context.Cursor);
		if (Under != INDEX_NONE && Context.Target->Network != nullptr)
		{
			const TArray<FApronSurface>& Aprons = Context.Target->Network->GetAprons();
			if (Aprons.IsValidIndex(Under))
			{
				const TArray<FVector2D>& Outline = Aprons[Under].Outline;
				for (int32 Index = 0; Index < Outline.Num(); ++Index)
				{
					Sink.Line(Outline[Index], Outline[(Index + 1) % Outline.Num()],
						EPreviewStyle::Doomed);
				}
			}
		}
		return;
	}

	Sink.Marker(Context.Cursor, EPreviewStyle::Pending);
}

// --- Outlining ------------------------------------------------------------------------

bool FApronOutliningState::WouldClose(const FToolContext& Context) const
{
	// Three corners is the least that encloses anything, so closing is not even offered
	// before then - a two-corner "polygon" would be a line.
	return Corners.Num() >= 3
		&& FVector2D::Distance(Corners[0], Context.Cursor) <= Context.SnapRadius;
}

bool FApronOutliningState::WouldCross(const FVector2D& Where) const
{
	if (Corners.Num() < 2)
	{
		return false;
	}

	// The new edge runs from the last corner. Every edge before the one it shares a corner
	// with is a candidate; that one shares an endpoint, which SegmentsCross does not count.
	const FVector2D From = Corners.Last();
	for (int32 Index = 0; Index + 1 < Corners.Num(); ++Index)
	{
		if (RoadGeom::SegmentsCross(From, Where, Corners[Index], Corners[Index + 1]))
		{
			return true;
		}
	}
	return false;
}

TUniquePtr<IApronDrawState> FApronOutliningState::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return nullptr;
	}

	if (WouldClose(Context))
	{
		// The facade refuses a self-crossing outline on its own - it owns the
		// triangulator's contract - so a refusal here leaves the outline in progress
		// rather than throwing away the work.
		if (Context.Target->AddApron(Corners) == INDEX_NONE)
		{
			return nullptr;
		}

		Context.Target->RebuildMesh();
		return MakeUnique<FApronIdleState>();
	}

	// Refused rather than placed, so a tangled outline cannot be built up in the first
	// place and then rejected at the very end after eight corners of work.
	if (WouldCross(Context.Cursor))
	{
		return nullptr;
	}

	Corners.Add(Context.Cursor);
	return nullptr;
}

TUniquePtr<IApronDrawState> FApronOutliningState::OnCancel(const FToolContext& Context)
{
	// One corner at a time. Binning eight corners for one misclick is a harsher answer
	// than the mistake deserves, and the corners are not in the model to be undone.
	Corners.Pop();
	if (Corners.Num() == 0)
	{
		return MakeUnique<FApronIdleState>();
	}
	return nullptr;
}

void FApronOutliningState::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	for (int32 Index = 0; Index + 1 < Corners.Num(); ++Index)
	{
		Sink.Line(Corners[Index], Corners[Index + 1], EPreviewStyle::Pending);
	}

	for (const FVector2D& Corner : Corners)
	{
		Sink.Marker(Corner, EPreviewStyle::Pending);
	}

	const bool bClosing = WouldClose(Context);
	const bool bCrosses = !bClosing && WouldCross(Context.Cursor);

	// The first corner lights up when the cursor is near enough to close on it, which is
	// the only way to know the gesture is finishable without trying it.
	if (bClosing)
	{
		Sink.Marker(Corners[0], EPreviewStyle::Snap);
	}

	const EPreviewStyle Style = bCrosses ? EPreviewStyle::Refused : EPreviewStyle::Pending;
	const FVector2D Ahead = bClosing ? Corners[0] : Context.Cursor;
	Sink.Line(Corners.Last(), Ahead, Style);

	if (bCrosses)
	{
		Sink.Label(Context.Cursor, TEXT("crosses the outline"), EPreviewStyle::Refused);
	}

	// The closing edge, so the SHAPE is visible rather than just the path walked so far.
	if (!bClosing && Corners.Num() >= 2)
	{
		Sink.Line(Ahead, Corners[0], EPreviewStyle::Heal);
	}
}

// --- The tool -------------------------------------------------------------------------

FApronDrawTool::FApronDrawTool()
	: State(MakeUnique<FApronIdleState>())
{
}

FText FApronDrawTool::GetDisplayName() const
{
	return LOCTEXT("ApronTool", "Apron");
}

bool FApronDrawTool::IsIdle() const
{
	return State.IsValid() && State->IsIdle();
}

TArrayView<const FVector2D> FApronDrawTool::GetCorners() const
{
	return State.IsValid() ? State->GetCorners() : TArrayView<const FVector2D>();
}

void FApronDrawTool::OnClick(const FToolContext& Context)
{
	if (!State.IsValid())
	{
		return;
	}

	if (TUniquePtr<IApronDrawState> Next = State->OnClick(Context))
	{
		State = MoveTemp(Next);
	}
}

void FApronDrawTool::OnCancel(const FToolContext& Context)
{
	if (!State.IsValid())
	{
		return;
	}

	if (TUniquePtr<IApronDrawState> Next = State->OnCancel(Context))
	{
		State = MoveTemp(Next);
	}
}

void FApronDrawTool::OnDeactivate(const FToolContext& Context)
{
	// A part-drawn outline is discarded outright rather than kept for a return visit. It
	// exists only in this state object, so nothing in the model has to be cleaned up -
	// which is the payoff for not putting it there.
	State = MakeUnique<FApronIdleState>();
}

void FApronDrawTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (State.IsValid())
	{
		State->BuildPreview(Context, Sink);
	}
}

#undef LOCTEXT_NAMESPACE
