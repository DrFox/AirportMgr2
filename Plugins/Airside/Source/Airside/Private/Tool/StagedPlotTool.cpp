#include "Tool/StagedPlotTool.h"

#include "Model/RoadNetwork.h"

int32 FStagedPlotTool::EntityUnder(const FToolContext& Context) const
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return INDEX_NONE;
	}
	const int32 Under = Context.Target->FindEntityAt(Context.Cursor, Context.SnapRadius);
	if (!Network->GetEntities().IsValidIndex(Under))
	{
		return INDEX_NONE;
	}
	return IsMine(Network->GetEntities()[Under]) ? Under : INDEX_NONE;
}

void FStagedPlotTool::OnClick(const FToolContext& Context)
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return;
	}

	// REMOVE: whatever this tool's own kind is under the cursor, and nothing else - no
	// gesture is started or advanced. IsMine is what used to be PlotUnder's "DEPOTS ONLY" or
	// StandUnder's "STANDS ONLY" comment; the two bodies around it were identical.
	if (Context.bRemoveModifier)
	{
		const int32 Doomed = EntityUnder(Context);
		if (Doomed != INDEX_NONE)
		{
			Context.Target->DeleteEntity(Doomed);
		}
		return;
	}

	// THE SHARED FIRST CLICK - the grid, the side and the kerb offset all live in
	// PlotGesture::AnchorAt, which every staged plot tool asks of its own Filter().
	if (Pinned == 0)
	{
		PlotGesture::FAnchor Anchor;
		auto Accept = [this](const URoadNetwork& N, FRoadSegmentId Id) { return Filter(N, Id); };
		if (!PlotGesture::AnchorAt(*Network, Context.Cursor, Accept, Anchor))
		{
			return;
		}
		Corners[0] = Anchor.Corner;
		Along = Anchor.Along;
		Inward = Anchor.Inward;

		// A FRESH GESTURE, so any refusal the LAST one earned at commit is no longer about
		// anything on screen.
		bLastCommitRefused = false;
		Pinned = 1;
		return;
	}

	if (Pinned >= MaxPinned)
	{
		// NOTHING. The gesture is locked and the Build button is the only way on - a click
		// that committed here would delete the review beat the staging exists for.
		return;
	}

	// PINNED FROM WHAT WAS ON SCREEN, not recomputed. Shape() is what the ghost drew this
	// frame, so a click can only ever pin the shape the player was looking at.
	TArray<FVector2D> Shown;
	Shape(Context, Shown);
	if (Shown.Num() <= Pinned)
	{
		return;
	}

	// THE CLOSING CLICK ONLY: refused at the click that would make it, not at commit, so the
	// player is never left holding a shape that can only be escaped by cancelling. Every
	// earlier click is unconditional - see CanCloseShape's own comment.
	const bool bClosing = Pinned + 1 == MaxPinned;
	if (bClosing && !CanCloseShape(Shown))
	{
		return;
	}

	Corners[Pinned] = Shown[Pinned];
	++Pinned;
}

void FStagedPlotTool::OnCancel(const FToolContext& Context)
{
	// STEPPING BACK RE-OPENS THE GESTURE, so a refusal earned by the shape being left behind
	// no longer describes anything the player can still commit.
	bLastCommitRefused = false;

	// ONE STAGE AT A TIME, the same answer the outline tool gives a misclick: binning the
	// whole gesture is a harsher response than the mistake deserves.
	if (Pinned > 0)
	{
		--Pinned;
	}
}

void FStagedPlotTool::OnCommit(const FToolContext& Context)
{
	// EVERY STAGE BUT THE LAST IGNORES THIS. Build is a widget, not a stage of the gesture,
	// so it is reachable whenever the bar is on screen.
	if (!IsConfirmed() || Context.Target == nullptr)
	{
		return;
	}

	TArray<FVector2D> Outline;
	Shape(Context, Outline);
	if (Outline.Num() < 4)
	{
		return;
	}

	// THE SAME SHAPE THE GHOST DREW. Built from Shape() rather than rebuilt from a width and a
	// depth, so what is committed cannot differ from what was on screen when Build was hit.
	const int32 Placed = Place(Context, Outline);

	// HONOUR THE RETURN - issue #182. INDEX_NONE keeps the gesture in Confirm - the same shape
	// stays on screen, and the next readout warns through bLastCommitRefused. See that field's
	// own comment on why this branch is a safety net rather than the ordinary path.
	if (Placed == INDEX_NONE)
	{
		bLastCommitRefused = true;
		return;
	}

	// BACK TO IDLE, ready for the next one. A tool that stayed in Confirm would let the
	// player press Build twice and get two entities stacked on one shape.
	Pinned = 0;
}

void FStagedPlotTool::OnDeactivate(const FToolContext& Context)
{
	// Discarded outright, like the outline tool's part-drawn shape: it exists only on this
	// object, so nothing in the model has to be cleaned up.
	Pinned = 0;
	bLastCommitRefused = false;
}

void FStagedPlotTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	// REMOVE shows what a click would take - the whole shape, outlined doomed - and none of
	// the placement ghost, which would be offering to build while the click deletes.
	if (Context.bRemoveModifier)
	{
		const int32 Doomed = EntityUnder(Context);
		if (Doomed != INDEX_NONE)
		{
			const FEntityInstance& Entity = Network->GetEntities()[Doomed];
			// IsMine is already true here - EntityUnder only ever returns one of this tool's
			// own - but named on this line too, not left implicit in the caller, so
			// Check-Architecture's IsPlotted rule reads "has ground to outline" here rather
			// than trusting a kind test at a distance.
			if (IsMine(Entity) && Entity.IsPlotted())
			{
				Sink.Polygon(Entity.Outline, EPreviewStyle::Doomed);
			}
			else
			{
				Sink.Marker(Entity.Position, EPreviewStyle::Doomed);
			}
			Sink.Label(Context.Cursor, RemoveLabel(), EPreviewStyle::Doomed);
			DescribeRemoveExtra(Context, Doomed, Entity, Sink);
		}
		return;
	}

	if (Pinned == 0)
	{
		// THE SAME GRID THE CLICK ANCHORS ON - PlotGesture::DescribeAnchors and AnchorAt are
		// one rule written once, so the heavier dot is the anchor a click takes.
		auto Accept = [this](const URoadNetwork& N, FRoadSegmentId Id) { return Filter(N, Id); };
		if (!PlotGesture::DescribeAnchors(*Network, Context.Cursor, Accept, Sink))
		{
			Sink.Label(Context.Cursor, FString::Printf(TEXT("move near %s"), *RoadNoun()),
				EPreviewStyle::Refused);
		}
		return;
	}

	TArray<FVector2D> Shown;
	Shape(Context, Shown);
	if (Shown.Num() < 2)
	{
		return;
	}

	// A DOT PER CORNER ALREADY PLACED, so "N/M" has something on the ground to count against
	// rather than being a number the player has to take on trust.
	for (int32 Index = 0; Index < Pinned && Index < Shown.Num(); ++Index)
	{
		Sink.Marker(Shown[Index], EPreviewStyle::Pinned);
	}

	// THE FIRST EDGE IS PINNED FROM THE SECOND CLICK ON. At one corner it still follows the
	// cursor, so it is drawn solid only once it has stopped moving.
	Sink.Line(Shown[0], Shown[1], Pinned >= 2 ? EPreviewStyle::Pinned : EPreviewStyle::Provisional);

	if (Shown.Num() < 4)
	{
		return;
	}

	// THE REST OF THE BOUNDARY, the guide line and whatever is drawn inside the shape all
	// differ by tool - which edge counts as settled once a rectangle's derived fourth corner
	// exists is not the same question as it is for a free quad's own fourth click.
	Describe(Context, Shown, Sink);
}

void FStagedPlotTool::BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();

	// REMOVE ASKS A DIFFERENT QUESTION from placement, so it gets its own answer: the Idle
	// readout's road warning would be advice about a gesture Remove never makes.
	if (Context.bRemoveModifier)
	{
		if (EntityUnder(Context) == INDEX_NONE)
		{
			Sink.Warning(RemoveWarning());
		}
		Sink.Committable(false);
		return;
	}

	if (Pinned == 0)
	{
		// THE SAME QUESTION THE CLICK ASKS, so the warning cannot say "move near X" while a
		// click would have anchored perfectly well.
		auto Accept = [this](const URoadNetwork& N, FRoadSegmentId Id) { return Filter(N, Id); };
		PlotGesture::FAnchor Unused;
		if (Network == nullptr
			|| !PlotGesture::AnchorAt(*Network, Context.Cursor, Accept, Unused))
		{
			Sink.Warning(FString::Printf(TEXT("Move near %s"), *RoadNoun()));
		}
		Sink.Committable(false);
		return;
	}

	TArray<FVector2D> Shown;
	Shape(Context, Shown);
	if (Shown.Num() < 2)
	{
		Sink.Committable(false);
		return;
	}

	// FIRST, because it is the line that says where the player is in the gesture; every other
	// fact is about a shape that may not be finished.
	Sink.Fact(PointsFactLabel(), FString::Printf(TEXT("%d/%d"), Pinned, MaxPinned));

	DescribeReadout(Context, Shown, Sink);
}
