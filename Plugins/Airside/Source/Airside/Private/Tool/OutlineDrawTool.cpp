#include "Tool/OutlineDrawTool.h"

#include "Solve/RoadGeom.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/**
	 * Where a click would put the next corner: the guide's point when one holds, else the cursor.
	 *
	 * PREFIXED for the unity build, like FRoadDrawTool's RoadGuidedSnap - "GuidedCursor" is the
	 * name a second tool would also pick, and two of them collide only once they share a blob.
	 *
	 * CLOSING BEATS THE GUIDE, which is why the caller tests bClosing FIRST and never asks this
	 * when it is true. Same precedence FRoadDrawTool sets between a snap and a guide: a guide
	 * proposes where a free point lands, and never overrides a point the player aimed AT.
	 */
	FVector2D OutlineGuidedCursor(const FToolContext& Context)
	{
		return Context.GuidedCursor();
	}

	/**
	 * The dashed line to each thing the corner is lined up with, and its label.
	 *
	 * IT LIVES IN THE TOOL, and that is what caught this out. FBuildSession resolves the guide
	 * onto the context, but NOTHING DRAWS IT until a tool asks - so a tool that describes an
	 * anchor and stops has a guide computed and thrown away, showing the player nothing. That is
	 * exactly what this tool did between gaining its anchor and gaining this, and the anchor's
	 * own test passed throughout: it measured the producer and never the consumer.
	 */
	void OutlineDrawGuide(const FToolContext& Context, const FVector2D& Moving,
		IToolPreviewSink& Sink)
	{
		if (!Context.Guide.bActive)
		{
			return;
		}

		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			Sink.Line(Moving, Winner.ReferenceAt, EPreviewStyle::Guide);

			// At the line's MIDPOINT, like every other tool: two labels at the moving point
			// overprint, and the plugin has no camera to offset them by readable pixels.
			Sink.Label((Moving + Winner.ReferenceAt) * 0.5, Winner.Description,
				EPreviewStyle::Guide);
		}
	}
}

// --- Idle -----------------------------------------------------------------------------

TUniquePtr<IOutlineDrawState> FOutlineIdleState::OnClick(
	const FToolContext& Context, const IOutlineTarget& Target)
{
	if (Context.Target == nullptr)
	{
		return nullptr;
	}

	// Ctrl on open ground with nothing part-drawn removes whatever is under the cursor.
	if (Context.bRemoveModifier)
	{
		// No RebuildMesh() on success - the facade's own mutators notify on commit
		// (issue #77).
		Target.RemoveUnderCursor(Context);
		return nullptr;
	}

	// THE GUIDED CURSOR, not the raw one: the FIRST corner is guided too since 2026-09-20 -
	// FOutlineDrawTool::WantsFreeStartGuides. A dashed line drawn under the cursor and then not
	// obeyed by the click is a mark whose meaning has gone.
	return MakeUnique<FOutlineDrawingState>(OutlineGuidedCursor(Context));
}

TUniquePtr<IOutlineDrawState> FOutlineIdleState::OnCancel(
	const FToolContext& Context, const IOutlineTarget& Target)
{
	return nullptr;
}

void FOutlineIdleState::BuildPreview(const FToolContext& Context, const IOutlineTarget& Target,
	IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr)
	{
		return;
	}

	// Aiming a removal: outline the thing that would go.
	if (Context.bRemoveModifier)
	{
		Target.PreviewRemoval(Context, Sink);
		return;
	}

	// WHERE THE FIRST CORNER WOULD LAND, which is the guided point and not the raw cursor -
	// otherwise the marker sits where the mouse is while the click lands somewhere else.
	const FVector2D First = OutlineGuidedCursor(Context);
	Sink.Marker(First, EPreviewStyle::Pending);

	// AND WHAT IT IS LINED UP WITH. THE CONSUMER of the free start: the session resolves a
	// guide here and nothing would ever show it without this call. See
	// IBuildTool::WantsFreeStartGuides on why describing an anchor and stopping is half a
	// feature - it is the shape of the bug that shipped on 2026-09-20.
	OutlineDrawGuide(Context, First, Sink);
}

// --- Drawing --------------------------------------------------------------------------

bool FOutlineDrawingState::WouldClose(const FToolContext& Context) const
{
	// Three corners is the least that encloses anything, so closing is not even offered
	// before then - a two-corner "polygon" would be a line.
	return Corners.Num() >= 3
		&& FVector2D::Distance(Corners[0], Context.Cursor) <= Context.SnapRadius;
}

bool FOutlineDrawingState::WouldCross(const FVector2D& Where) const
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

TUniquePtr<IOutlineDrawState> FOutlineDrawingState::OnClick(
	const FToolContext& Context, const IOutlineTarget& Target)
{
	if (Context.Target == nullptr)
	{
		return nullptr;
	}

	if (WouldClose(Context))
	{
		// The target refuses an outline it cannot build - the facade owns the
		// triangulator's contract, and a plot must reach a road - so a refusal here leaves
		// the outline in progress rather than throwing away the work.
		if (!Target.Commit(Context, Corners))
		{
			return nullptr;
		}

		// No RebuildMesh() here - the facade's mutators notify on commit (issue #77).
		return MakeUnique<FOutlineIdleState>();
	}

	// Refused rather than placed, so a tangled outline cannot be built up in the first
	// place and then rejected at the very end after eight corners of work.
	if (WouldCross(OutlineGuidedCursor(Context)))
	{
		return nullptr;
	}

	Corners.Add(OutlineGuidedCursor(Context));
	return nullptr;
}

TUniquePtr<IOutlineDrawState> FOutlineDrawingState::OnCancel(
	const FToolContext& Context, const IOutlineTarget& Target)
{
	// One corner at a time. Binning eight corners for one misclick is a harsher answer
	// than the mistake deserves, and the corners are not in the model to be undone.
	Corners.Pop();
	if (Corners.Num() == 0)
	{
		return MakeUnique<FOutlineIdleState>();
	}
	return nullptr;
}

void FOutlineDrawingState::BuildPreview(const FToolContext& Context, const IOutlineTarget& Target,
	IToolPreviewSink& Sink) const
{
	Sink.Polyline(Corners, EPreviewStyle::Pending);

	for (const FVector2D& Corner : Corners)
	{
		Sink.Marker(Corner, EPreviewStyle::Pending);
	}

	const bool bClosing = WouldClose(Context);
	const bool bCrosses = !bClosing && WouldCross(OutlineGuidedCursor(Context));

	// The first corner lights up when the cursor is near enough to close on it, which is
	// the only way to know the gesture is finishable without trying it.
	if (bClosing)
	{
		Sink.Marker(Corners[0], EPreviewStyle::Snap);
	}

	const EPreviewStyle Style = bCrosses ? EPreviewStyle::Refused : EPreviewStyle::Pending;
	const FVector2D Ahead = bClosing ? Corners[0] : OutlineGuidedCursor(Context);
	Sink.Line(Corners.Last(), Ahead, Style);

	// AND WHAT IT IS LINED UP WITH. Not drawn while closing: the corner is going to the first
	// one, so a guide line to somewhere else would be describing a constraint that is not
	// being applied.
	if (!bClosing)
	{
		OutlineDrawGuide(Context, Ahead, Sink);
	}

	if (bCrosses)
	{
		Sink.Label(OutlineGuidedCursor(Context), TEXT("crosses the outline"), EPreviewStyle::Refused);
	}

	// The closing edge, so the SHAPE is visible rather than just the path walked so far.
	if (!bClosing && Corners.Num() >= 2)
	{
		Sink.Line(Ahead, Corners[0], EPreviewStyle::Heal);
	}
}

// --- The tool -------------------------------------------------------------------------

FOutlineDrawTool::FOutlineDrawTool()
	: State(MakeUnique<FOutlineIdleState>())
{
}

bool FOutlineDrawTool::IsIdle() const
{
	return State.IsValid() && State->IsIdle();
}

bool FOutlineDrawTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	const TArrayView<const FVector2D> Corners = GetCorners();

	// A BOUNDARY DRAG WHETHER OR NOT THERE IS AN EDGE YET, so this is set before either decline
	// below: the FIRST corner of an outline is as much a point on the shape's own limit as the
	// fifth, and FApronLineGuideSource reads exactly this to decide whether to displace by a
	// half-width. A free start that forgot it would offer to lay an apron corner half a road's
	// width off the edge it was being lined up with.
	Out.Point = EDragPoint::Boundary;

	// TWO CORNERS BEFORE THERE IS AN EDGE. One corner is a point with no direction, so there is
	// nothing for Extending to extend - the same reason FRoadDrawTool declines its first click.
	//
	// AND WITH NO CORNERS AT ALL the base answers a FREE START: starting an outline flush with
	// a road edge, or in line with an apron already down, is what that buys. Note the middle
	// case falls between the two - ONE corner down is neither an edge nor a free start, and the
	// base declines it on IsIdle() without this function having to say so twice.
	if (Corners.Num() < 2)
	{
		return IBuildTool::DescribeGuideAnchor(Network, Target, Out);
	}

	const FVector2D Last = Corners[Corners.Num() - 1];
	const FVector2D Previous = Corners[Corners.Num() - 2];
	const FVector2D Edge = Last - Previous;
	if (Edge.IsNearlyZero())
	{
		// TWO CORNERS IN THE SAME PLACE. The gesture has begun, so IsIdle() is false and the
		// base declines too; routed through it anyway so the exits cannot come to disagree.
		return IBuildTool::DescribeGuideAnchor(Network, Target, Out);
	}

	Out.Origin = Last;
	Out.Reference = Edge.GetSafeNormal();
	Out.ReferenceAt = Previous;
	Out.ReferenceName = TEXT("this edge");

	// EVERY CORNER BUT THE LAST. The one being dragged from is the origin, and a point cannot
	// line up with itself: its own two lines pass through wherever the cursor is, so both would
	// always be in tolerance - the trap FPlotPlaceTool's anchor records.
	//
	// NUMBERED AS THE PLAYER COUNTS THEM, from one.
	for (int32 Index = 0; Index < Corners.Num() - 1; ++Index)
	{
		FGuidePoint Point;
		Point.At = Corners[Index];
		Point.Name = FString::Printf(TEXT("corner %d"), Index + 1);

		// THE GESTURE'S OWN, so the Road and Apron buttons do not govern them - a corner you
		// placed ten seconds ago is not a thing on the map yet.
		Point.Reference = SnapGuide::EReference::ThisGesture;
		Out.AlignTo.Add(Point);
	}
	return true;
}

TArrayView<const FVector2D> FOutlineDrawTool::GetCorners() const
{
	return State.IsValid() ? State->GetCorners() : TArrayView<const FVector2D>();
}

void FOutlineDrawTool::OnClick(const FToolContext& Context)
{
	if (!State.IsValid())
	{
		return;
	}

	if (TUniquePtr<IOutlineDrawState> Next = State->OnClick(Context, GetOutlineTarget()))
	{
		State = MoveTemp(Next);
	}
}

void FOutlineDrawTool::OnCancel(const FToolContext& Context)
{
	if (!State.IsValid())
	{
		return;
	}

	if (TUniquePtr<IOutlineDrawState> Next = State->OnCancel(Context, GetOutlineTarget()))
	{
		State = MoveTemp(Next);
	}
}

void FOutlineDrawTool::OnDeactivate(const FToolContext& Context)
{
	// A part-drawn outline is discarded outright rather than kept for a return visit. It
	// exists only in this state object, so nothing in the model has to be cleaned up -
	// which is the payoff for not putting it there.
	State = MakeUnique<FOutlineIdleState>();
}

void FOutlineDrawTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (State.IsValid())
	{
		State->BuildPreview(Context, GetOutlineTarget(), Sink);
	}
}

#undef LOCTEXT_NAMESPACE
