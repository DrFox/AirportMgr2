#include "Tool/RoadDrawTool.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Present/RoadNetworkActor.h"
#include "Tool/RoadHeal.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/** Position of a live node, or the cursor when there is not one. */
	FVector2D NodePosition(const FToolContext& Context, int32 NodeIndex)
	{
		if (Context.Target != nullptr && Context.Target->Network != nullptr)
		{
			const TArray<FRoadNode>& Nodes = Context.Target->Network->GetNodes();
			if (Nodes.IsValidIndex(NodeIndex) && Nodes[NodeIndex].bAlive)
			{
				return Nodes[NodeIndex].Position;
			}
		}
		return Context.Cursor;
	}

	/**
	 * Put a node where the snap says, whichever kind it is, and report whether it is new.
	 *
	 * The three outcomes are the whole difference between continuing a road, closing a
	 * junction on a node already there, and cutting a new junction into a road already
	 * drawn - and the snap chain has already decided which.
	 */
	int32 ResolveToNode(const FToolContext& Context, bool& bOutCreated)
	{
		bOutCreated = true;

		switch (Context.Snap.Kind)
		{
		case ERoadSnapKind::Node:
			bOutCreated = false;
			return Context.Snap.Node.Index;

		case ERoadSnapKind::Segment:
			return Context.Target->SplitSegment(Context.Snap.Segment.Index, Context.Snap.Position);

		case ERoadSnapKind::Free:
		default:
			return Context.Target->PlaceNode(Context.Snap.Position);
		}
	}
}

// --- Idle ---------------------------------------------------------------------------

TUniquePtr<IRoadDrawState> FRoadIdleState::OnClick(const FToolContext& Context)
{
	bool bCreated = false;
	const int32 Started = ResolveToNode(Context, bCreated);
	if (Started == INDEX_NONE)
	{
		return nullptr;
	}

	Context.Target->RebuildMesh();
	return MakeUnique<FRoadChainingState>(Started, bCreated);
}

TUniquePtr<IRoadDrawState> FRoadIdleState::OnCancel(const FToolContext& Context)
{
	// Nothing part-drawn to back out of.
	return nullptr;
}

void FRoadIdleState::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	// Only what the next click would attach to. There is no road in progress to show.
	if (Context.Snap.Kind == ERoadSnapKind::Node)
	{
		Sink.Marker(Context.Snap.Position, EPreviewStyle::Snap);
	}
}

// --- Chaining -----------------------------------------------------------------------

TUniquePtr<IRoadDrawState> FRoadChainingState::OnClick(const FToolContext& Context)
{
	// Judged BEFORE anything is created. Validating afterwards would leave a stray node
	// behind on every refused click - the road would not appear, but the graph would have
	// grown anyway.
	FRoadNodeId FromId;
	if (Context.Target->MakeLiveNodeId(From, FromId))
	{
		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Context.Target->Network, FromId, Context.Snap, Context.Limits);
		if (Judgement != ERoadPlacement::Valid)
		{
			return nullptr;
		}
	}

	bool bNextCreated = false;
	const int32 To = ResolveToNode(Context, bNextCreated);
	if (To == INDEX_NONE)
	{
		return nullptr;
	}

	if (To != From && !Context.Target->ConnectNodes(From, To))
	{
		// The facade already logged why. Drop the chain rather than leaving the player
		// clicking against a connection that will not form.
		Context.Target->RebuildMesh();
		return MakeUnique<FRoadIdleState>();
	}

	Context.Target->RebuildMesh();

	// Chain on from the node just reached, so a road is drawn click by click rather than
	// a pair of clicks per segment.
	return MakeUnique<FRoadChainingState>(To, bNextCreated);
}

TUniquePtr<IRoadDrawState> FRoadChainingState::OnCancel(const FToolContext& Context)
{
	// A chain that placed a node and drew nothing from it leaves that node with no road on
	// it. Removed here because this gesture created it and this gesture is being abandoned
	// - and only if it is still bare, because a node that picked up a segment is part of
	// the network now, whoever made it.
	if (bCreated && Context.Target != nullptr && Context.Target->Network != nullptr)
	{
		const TArray<FRoadNode>& Nodes = Context.Target->Network->GetNodes();
		if (Nodes.IsValidIndex(From) && Nodes[From].bAlive && Nodes[From].Incident.Num() == 0)
		{
			if (Context.Target->DeleteNode(From))
			{
				Context.Target->RebuildMesh();
			}
		}
	}

	return MakeUnique<FRoadIdleState>();
}

void FRoadChainingState::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	Sink.Marker(NodePosition(Context, From), EPreviewStyle::Pending);

	if (Context.Snap.Kind == ERoadSnapKind::Node)
	{
		Sink.Marker(Context.Snap.Position, EPreviewStyle::Snap);
	}

	// The reason a click will be refused. The ghost already says THAT it will be, by
	// turning red; a colour cannot say which of four rules objected.
	FRoadNodeId FromId;
	if (Context.Target != nullptr && Context.Target->Network != nullptr
		&& Context.Target->MakeLiveNodeId(From, FromId))
	{
		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Context.Target->Network, FromId, Context.Snap, Context.Limits);
		if (Judgement != ERoadPlacement::Valid)
		{
			Sink.Label(Context.Snap.Position, RoadPlacement::Describe(Judgement), EPreviewStyle::Refused);
		}
	}
}

// --- The tool -----------------------------------------------------------------------

FRoadDrawTool::FRoadDrawTool()
	: State(MakeUnique<FRoadIdleState>())
{
}

FText FRoadDrawTool::GetDisplayName() const
{
	return LOCTEXT("RoadTool", "Road");
}

bool FRoadDrawTool::IsIdle() const
{
	return State.IsValid() && State->IsIdle();
}

int32 FRoadDrawTool::GetPendingNode() const
{
	return State.IsValid() ? State->GetPendingNode() : INDEX_NONE;
}

void FRoadDrawTool::Remove(const FToolContext& Context)
{
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
		return;
	}

	// A deletion can take the node the chain was running from, so the chain ends rather
	// than being checked. Its start may not exist any more.
	State = MakeUnique<FRoadIdleState>();
	Context.Target->RebuildMesh();
}

void FRoadDrawTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr || !State.IsValid())
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		Remove(Context);
		return;
	}

	// Shift on a road inserts a node and stops there. A plain click splits too, but also
	// starts a chain from the new node - right when drawing a road INTO an existing one,
	// and a nuisance when all you wanted was somewhere to drag from.
	if (Context.bInsertModifier && Context.Snap.Kind == ERoadSnapKind::Segment)
	{
		if (Context.Target->SplitSegment(Context.Snap.Segment.Index, Context.Snap.Position) != INDEX_NONE)
		{
			Context.Target->RebuildMesh();
		}
		return;
	}

	if (TUniquePtr<IRoadDrawState> Next = State->OnClick(Context))
	{
		State = MoveTemp(Next);
	}
}

void FRoadDrawTool::OnCancel(const FToolContext& Context)
{
	if (Context.Target == nullptr || !State.IsValid())
	{
		return;
	}

	if (TUniquePtr<IRoadDrawState> Next = State->OnCancel(Context))
	{
		State = MoveTemp(Next);
	}
}

void FRoadDrawTool::OnDragBegin(const FToolContext& Context)
{
	// Only a node can be dragged, and never while aiming a deletion - dragging something
	// about to be removed would be nonsense.
	if (Context.Target == nullptr || Context.bRemoveModifier
		|| Context.Snap.Kind != ERoadSnapKind::Node)
	{
		return;
	}

	DragNode = Context.Snap.Node.Index;

	// One undo step for the whole drag, not one per frame.
	Context.Target->BeginInteractiveEdit(TEXT("move node"));
}

void FRoadDrawTool::OnDrag(const FToolContext& Context)
{
	if (DragNode == INDEX_NONE || Context.Target == nullptr)
	{
		return;
	}

	// A refused move simply does not happen, so the node stops following the cursor rather
	// than dragging a road shorter than the solver can trim.
	if (Context.Target->MoveNode(DragNode, Context.Cursor))
	{
		Context.Target->RebuildMesh();
	}
}

void FRoadDrawTool::OnDragEnd(const FToolContext& Context)
{
	if (DragNode == INDEX_NONE || Context.Target == nullptr)
	{
		return;
	}

	DragNode = INDEX_NONE;
	Context.Target->EndInteractiveEdit(/*bKeep*/ true);
	Context.Target->RebuildMesh();
}

void FRoadDrawTool::Tick(const FToolContext& Context)
{
	if (Context.Target == nullptr)
	{
		return;
	}

	const int32 Pending = GetPendingNode();

	// No ghost while a deletion is being aimed or a node is being dragged: in one the
	// preview would offer to build the thing about to be removed, and in the other the
	// road being reshaped is already on screen.
	if (Pending == INDEX_NONE || Context.bRemoveModifier || DragNode != INDEX_NONE)
	{
		Context.Target->HideGhost();
		return;
	}

	FRoadNodeId FromId;
	if (Context.Target->Network == nullptr || !Context.Target->MakeLiveNodeId(Pending, FromId))
	{
		Context.Target->HideGhost();
		return;
	}

	// Shown even when illegal, coloured rather than withheld: hiding it would answer "why
	// can I not build here" with nothing at all.
	const ERoadPlacement Judgement =
		RoadPlacement::Validate(*Context.Target->Network, FromId, Context.Snap, Context.Limits);
	Context.Target->UpdateGhost(Pending, Context.Snap, Judgement == ERoadPlacement::Valid);
}

void FRoadDrawTool::OnDeactivate(const FToolContext& Context)
{
	// Abandon the part-drawn chain rather than leaving it to reappear when this tool is
	// picked again - a click landing on a road started minutes ago and forgotten.
	OnCancel(Context);

	if (DragNode != INDEX_NONE && Context.Target != nullptr)
	{
		Context.Target->EndInteractiveEdit(/*bKeep*/ true);
		DragNode = INDEX_NONE;
	}

	if (Context.Target != nullptr)
	{
		Context.Target->HideGhost();
	}
}

void FRoadDrawTool::PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Target->Network == nullptr)
	{
		return;
	}

	const URoadNetwork& Network = *Context.Target->Network;

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

void FRoadDrawTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr)
	{
		return;
	}

	if (Context.bRemoveModifier)
	{
		PreviewRemoval(Context, Sink);
		return;
	}

	// Where the click lands on the PLANE, which under an angled view is not where the
	// mouse pointer is drawn - and the shallower the view, the further apart they are.
	Sink.Marker(Context.Snap.Position, EPreviewStyle::Pending);

	if (Context.Snap.Kind == ERoadSnapKind::Segment && Context.Target->Network != nullptr)
	{
		const TArray<FRoadSegment>& Segments = Context.Target->Network->GetSegments();
		const int32 Index = Context.Snap.Segment.Index;
		if (Segments.IsValidIndex(Index) && Segments[Index].bAlive)
		{
			const FRoadNode* EndA = Context.Target->Network->GetNode(Segments[Index].A);
			const FRoadNode* EndB = Context.Target->Network->GetNode(Segments[Index].B);
			if (EndA != nullptr && EndB != nullptr)
			{
				Sink.CrossMark(Context.Snap.Position,
					(EndB->Position - EndA->Position).GetSafeNormal(), EPreviewStyle::Snap);
			}
		}
	}

	if (State.IsValid())
	{
		State->BuildPreview(Context, Sink);
	}
}

#undef LOCTEXT_NAMESPACE
