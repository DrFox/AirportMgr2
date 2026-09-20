#include "Tool/RemoveGesture.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Tool/RoadHeal.h"
#include "Tool/RoadPlacement.h"

void RemoveGesture::Describe(const FToolContext& Context, IToolPreviewSink& Sink)
{
	if (Context.Network() == nullptr)
	{
		return;
	}

	const URoadNetwork& Network = *Context.Network();

	auto SegmentEnds = [&Network](int32 SegmentIndex, FVector2D& OutA, FVector2D& OutB)
	{
		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		if (!Segments.IsValidIndex(SegmentIndex) || !Segments[SegmentIndex].bAlive)
		{
			return false;
		}
		const FRoadNode* EndA = Network.GetNode(Segments[SegmentIndex].A);
		const FRoadNode* EndB = Network.GetNode(Segments[SegmentIndex].B);
		if (EndA == nullptr || EndB == nullptr)
		{
			return false;
		}
		OutA = EndA->Position;
		OutB = EndB->Position;
		return true;
	};

	switch (Context.Snap.Kind)
	{
	case ERoadSnapKind::Node:
	{
		// The whole plan, asked of the model rather than guessed at here, so what is drawn
		// and what the click does are one answer - including the refusal.
		const FRoadDeletionPlan Plan = Context.Target->PlanNodeDeletion(Context.Snap.Node.Index);

		Sink.Marker(Context.Snap.Position, EPreviewStyle::Doomed);

		for (const FRoadSegmentId& Doomed : Plan.Doomed)
		{
			FVector2D A;
			FVector2D B;
			if (SegmentEnds(Doomed.Index, A, B))
			{
				Sink.Line(A, B, EPreviewStyle::Doomed);
			}
		}

		if (!Plan.bValid)
		{
			// Drawing a heal it cannot perform would be a promise it will break.
			Sink.Label(Context.Snap.Position,
				FString::Printf(TEXT("cannot rejoin node %d (%s)"),
					Plan.RefusedNeighbour.Index, RoadPlacement::Describe(Plan.Refusal)),
				EPreviewStyle::Refused);
			break;
		}

		for (const FRoadNodeId& Swept : Plan.Swept)
		{
			if (const FRoadNode* Gone = Network.GetNode(Swept))
			{
				Sink.Marker(Gone->Position, EPreviewStyle::Doomed);
			}
		}

		// Deleting is no longer purely subtractive, so showing only what goes would be
		// half the truth.
		const FRoadNode* Anchor = Network.GetNode(Plan.Anchor);
		for (const FRoadNodeId& Stranded : Plan.Rejoin)
		{
			const FRoadNode* End = Network.GetNode(Stranded);
			if (Anchor != nullptr && End != nullptr)
			{
				Sink.Line(End->Position, Anchor->Position, EPreviewStyle::Heal);
			}
		}
		break;
	}

	case ERoadSnapKind::Segment:
	{
		FVector2D A;
		FVector2D B;
		if (SegmentEnds(Context.Snap.Segment.Index, A, B))
		{
			Sink.Line(A, B, EPreviewStyle::Doomed);
		}
		break;
	}

	case ERoadSnapKind::Free:
	default:
		break;
	}
}

bool RemoveGesture::Apply(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return false;
	}

	switch (Context.Snap.Kind)
	{
	case ERoadSnapKind::Node:
		Context.Target->DeleteNode(Context.Snap.Node.Index);
		break;

	case ERoadSnapKind::Segment:
		Context.Target->DeleteSegment(Context.Snap.Segment.Index);
		break;

	case ERoadSnapKind::Free:
	default:
		// Open ground. Nothing to remove is the correct outcome, not a refusal.
		return false;
	}

	// No RebuildMesh() here - DeleteNode/DeleteSegment notify on commit (issue #77).
	return true;
}
