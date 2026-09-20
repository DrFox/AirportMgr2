#include "Tool/RoadDrawTool.h"

#include "Model/BuildPurse.h"

#include "AirsideLog.h"

#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Solve/GuideArbiter.h"
#include "Tool/RoadHeal.h"

#define LOCTEXT_NAMESPACE "Airside"

namespace
{
	/**
	 * The snap as this tool should ACT on it: its position moved onto the guide when the chain
	 * claimed nothing.
	 *
	 * A SNAP BEATS A GUIDE. When the chain claimed a node or a segment the player is attaching
	 * to something REAL - closing a junction, splitting a run - and that is a statement about
	 * the graph, where an alignment is only an aid. A guide allowed to override it would make a
	 * junction impossible to close while any guide was live, which is far worse than a guide
	 * occasionally not applying.
	 *
	 * RETURNS THE WHOLE RESULT so the ghost, all three placement judgements and the click take
	 * the same value. Handing some of them a position and others the raw snap is how a preview
	 * comes to promise what a click does not do.
	 *
	 * PREFIXED because this module is a unity build and "GuidedSnap" is exactly the name a
	 * second tool would also choose - see the SegmentEnds collision in SnapGuideChain.cpp.
	 */
	FRoadSnapResult RoadGuidedSnap(const FToolContext& Context)
	{
		FRoadSnapResult Guided = Context.Snap;
		if (Guided.Kind == ERoadSnapKind::Free)
		{
			Guided.Position = Context.GuidedCursor();
		}
		return Guided;
	}

	/** Position of a live node, or the cursor when there is not one. */
	FVector2D NodePosition(const FToolContext& Context, int32 NodeIndex)
	{
		if (Context.Network() != nullptr)
		{
			const TArray<FRoadNode>& Nodes = Context.Network()->GetNodes();
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
			return Context.Target->PlaceNode(RoadGuidedSnap(Context).Position);
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

	// No RebuildMesh() here any more - PlaceNode/SplitSegment, whichever ResolveToNode just
	// called, now notify the facade's own OnChanged on commit (issue #77).
	return MakeUnique<FRoadChainingState>(Started, bCreated, Kind, WidthIndex);
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
		// Guarded the same way BuildPreview is. The preview and the click used to disagree
		// here - preview checked GetNetwork() for null, the click dereferenced it - and the
		// click is where a null actually crashed (2026-09-06, RoadSlotMap.h:61 reading 0x40).
		// Dropped to idle rather than silently ignored so the player is not left chaining
		// from a node the network cannot see.
		const URoadNetwork* Network = Context.Network();
		if (Network == nullptr)
		{
			UE_LOG(LogAirside, Warning,
				TEXT("Road chaining from node %d refused: the target's network is null although the node is live"), From);
			return MakeUnique<FRoadIdleState>(Kind, WidthIndex);
		}
		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Network, FromId, RoadGuidedSnap(Context), Context.Limits);
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

	if (To != From && !Context.Target->ConnectNodes(From, To, Kind, WidthIndex))
	{
		// The facade already logged why. Drop the chain rather than leaving the player
		// clicking against a connection that will not form. NO RebuildMesh() here any more -
		// it used to cover the node or split ResolveToNode just made a few lines up, which
		// notifies on its OWN commit now (PlaceNode/SplitSegment, issue #77); the refused
		// ConnectNodes itself changed nothing further that needs showing.
		return MakeUnique<FRoadIdleState>(Kind, WidthIndex);
	}

	// No RebuildMesh() here either - ResolveToNode and ConnectNodes each notify the facade's
	// own OnChanged on commit now (issue #77).

	// Chain on from the node just reached, so a road is drawn click by click rather than
	// a pair of clicks per segment.
	return MakeUnique<FRoadChainingState>(To, bNextCreated, Kind, WidthIndex);
}

TUniquePtr<IRoadDrawState> FRoadChainingState::OnCancel(const FToolContext& Context)
{
	// A chain that placed a node and drew nothing from it leaves that node with no road on
	// it. Removed here because this gesture created it and this gesture is being abandoned
	// - and only if it is still bare, because a node that picked up a segment is part of
	// the network now, whoever made it.
	if (bCreated && Context.Network() != nullptr)
	{
		const TArray<FRoadNode>& Nodes = Context.Network()->GetNodes();
		if (Nodes.IsValidIndex(From) && Nodes[From].bAlive && Nodes[From].Incident.Num() == 0)
		{
			// No RebuildMesh() on success any more - DeleteNode notifies on commit (issue #77).
			Context.Target->DeleteNode(From);
		}
	}

	return MakeUnique<FRoadIdleState>(Kind, WidthIndex);
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
	if (Context.Network() != nullptr && Context.Target->MakeLiveNodeId(From, FromId))
	{
		const ERoadPlacement Judgement =
			RoadPlacement::Validate(*Context.Network(), FromId, RoadGuidedSnap(Context),
				Context.Limits);
		if (Judgement != ERoadPlacement::Valid)
		{
			Sink.Label(RoadGuidedSnap(Context).Position, RoadPlacement::Describe(Judgement),
				EPreviewStyle::Refused);
		}
		else if (const IBuildPurse* Purse = Context.Target->GetPurse())
		{
			// THE PRICE BEFORE THE CLICK. NO NEW SINK MESSAGE: Label already exists and
			// EPreviewStyle::Refused already means "something the gesture cannot do, with the
			// reason" - which is exactly what an unaffordable road is. The purse formats the
			// money, so no currency symbol ever enters this plugin.
			const FBuildQuote Quote = Context.Target->QuoteForConnect(
				From, Context.Snap.Position, Kind, WidthIndex);
			if (!Quote.IsFree())
			{
				Sink.Label(Context.Snap.Position, Purse->Describe(Quote).ToString(),
					Purse->CanAfford(Quote) ? EPreviewStyle::Pending : EPreviewStyle::Refused);
			}
		}
	}
}

// --- The tool -----------------------------------------------------------------------

FRoadDrawTool::FRoadDrawTool(ERoadKind InKind)
	: State(MakeUnique<FRoadIdleState>(InKind))
	, Kind(InKind)
{
}

FText FRoadDrawTool::GetDisplayName() const
{
	// TWO NAMES FOR ONE TOOL, and they must match the registry's own Name for each entry -
	// Airside.Tool.BuildSession asserts the two cannot drift, which is exactly the class of
	// bug the registry exists to make impossible elsewhere.
	//
	// "Road" was once refused here on the grounds that a key labelled Road promised a
	// vehicle tool that did not exist (2026-09-07). It exists now: key 9 lays the service
	// road profile, with a GroundVehicle guideline, and key 1 keeps the taxiway.
	return Kind == ERoadKind::ServiceRoad
		? LOCTEXT("RoadTool", "Road")
		: LOCTEXT("TaxiwayTool", "Taxiway");
}

bool FRoadDrawTool::IsIdle() const
{
	return State.IsValid() && State->IsIdle();
}

bool FRoadDrawTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// NOTHING PENDING MEANS NOTHING TO EXTEND. The first click of a chain has no direction to
	// speak of, and a guide offered there would be squaring to an edge that does not exist.
	const int32 Pending = GetPendingNode();
	if (Network == nullptr || Pending == INDEX_NONE)
	{
		return false;
	}

	const FRoadNodeId FromId = Network->NodeIdAt(Pending);
	const FRoadNode* From = Network->GetNode(FromId);
	if (From == nullptr)
	{
		return false;
	}

	Out.Origin = From->Position;

	// THE WIDTH THIS GESTURE WOULD LAY - the same question the ghost asks, through the same one
	// resolver, so a guide cannot disagree with the pavement it is guiding. A null Target is a
	// supported state and leaves the widths at zero, which means "no width" rather than "unknown".
	Out.Point = EDragPoint::Centreline;
	if (Target != nullptr)
	{
		if (const URoadProfile* Profile = Target->ResolveProfileFor(Kind, WidthIndex))
		{
			Out.HalfWidthLeft = Profile->GetHalfWidthLeft();
			Out.HalfWidthRight = Profile->GetHalfWidthRight();
		}
	}

	// THE SEGMENT ALREADY ARRIVING AT THE PENDING NODE. With exactly one incident segment the
	// answer is unambiguous - that is the road being extended. At a junction there are several
	// and none of them is "the" incoming one, so no reference is offered rather than an
	// arbitrary one: a guide that squared to whichever segment happened to be stored first
	// would change with an edit nobody connected to guides at all.
	int32 Incident = 0;
	FVector2D Along = FVector2D::ZeroVector;
	FVector2D OtherEnd = FVector2D::ZeroVector;

	const TArray<FRoadSegment>& Segments = Network->GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || (Segment.A != FromId && Segment.B != FromId))
		{
			continue;
		}

		const FRoadNode* Far = Network->GetNode(Segment.A == FromId ? Segment.B : Segment.A);
		if (Far == nullptr)
		{
			continue;
		}

		++Incident;
		Along = (From->Position - Far->Position).GetSafeNormal();
		OtherEnd = Far->Position;
	}

	if (Incident == 1 && !Along.IsNearlyZero())
	{
		Out.Reference = Along;
		Out.ReferenceAt = OtherEnd;
		Out.ReferenceName = TEXT("this road");
	}

	// EVERY LIVE NODE IN REACH IS SOMETHING TO LINE UP WITH - "level with that junction" is what
	// a player squinting at a taxiway layout actually wants. A node has no name, so the label
	// cannot say WHICH; the dashed line drawn to it is what does.
	//
	// A DELETED NODE KEEPS ITS SLOT, so bAlive is checked here as the segment loop above checks
	// its own: offering one would draw a guide to a junction the player has removed.
	const double Reach = SnapGuide::FTuning().SearchRadiusUu;
	const TArray<FRoadNode>& Nodes = Network->GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FRoadNode& Node = Nodes[Index];

		// NOT THE NODE BEING EXTENDED FROM: its own lines pass through the origin, so both would
		// always be in tolerance and the guide would say "you are level with yourself".
		if (Index == Pending || !Node.bAlive
			|| FVector2D::DistSquared(Node.Position, Out.Origin) > Reach * Reach)
		{
			continue;
		}
		// SPELT OUT, not braced: a third member arrived on FGuidePoint in 2026-09-20 and a
		// braced initialiser would have taken the default for it in silence.
		FGuidePoint Point;
		Point.At = Node.Position;
		Point.Name = TEXT("that node");

		// A LIVE NODE IS THE ROAD COLUMN'S. It is the one place a network fact reaches a
		// tool-fed source, and the tag is what lets the Road button switch it off.
		Point.Reference = SnapGuide::EReference::Road;
		Out.AlignTo.Add(Point);
	}

	return true;
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
	// WITH THIS TOOL'S OWN KIND. Dropping to a default-constructed idle state would silently
	// put the Road tool back into taxiway mode after a Ctrl+click removal, and the next
	// click would lay a 23 m aircraft lane where the player was drawing a road.
	State = MakeUnique<FRoadIdleState>(Kind, WidthIndex);
	// No RebuildMesh() here any more - DeleteNode/DeleteSegment notify on commit (issue #77).
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
		// No RebuildMesh() on success any more - SplitSegment notifies on commit (issue #77).
		Context.Target->SplitSegment(Context.Snap.Segment.Index, Context.Snap.Position);
		return;
	}

	if (TUniquePtr<IRoadDrawState> Next = State->OnClick(Context))
	{
		State = MoveTemp(Next);
	}
}

void FRoadDrawTool::OnReselect(const FToolContext& Context)
{
	// KEY-AGAIN CYCLES THE WIDTH, the gesture FRunwayTool::OnReselect already gives
	// runways. Borrowed rather than given a key of its own for the reason that tool states:
	// the number keys are spoken for, and "press the tool's key again" is a gesture a
	// player already knows from it.
	//
	// A SERVICE ROAD HAS NOTHING TO CYCLE. It carries one authored cross-section - see
	// UAirsideContent::ServiceRoadProfile - so this refuses rather than reaching for the
	// taxiway list, which would lay a 23 m lane for vans.
	if (Kind == ERoadKind::ServiceRoad)
	{
		UE_LOG(LogAirside, Log,
			TEXT("Road width unchanged: a service road has one authored cross-section"));
		return;
	}

	if (Context.Target == nullptr)
	{
		// A DIFFERENT REFUSAL from an empty content set, said differently - the same
		// distinction FRunwayTool::NextWidth draws, and for the same reason: this is a
		// caller bug, not a fresh project, and one shared message would blame the content
		// set for a null target.
		UE_LOG(LogAirside, Warning,
			TEXT("Taxiway width unchanged: no edit target in context, so there is nothing "
			     "to ask for widths"));
		return;
	}

	const int32 Count = Context.Target->GetTaxiwayProfileCount();
	if (Count <= 0)
	{
		// SAID OUT LOUD. Returning in silence is indistinguishable from a key that never
		// arrived: the player presses the tool's key again, nothing widens, and nothing
		// anywhere says why. A content set with no taxiway profiles is a real state - it
		// is what a project that has not run build_road_profiles.py has.
		UE_LOG(LogAirside, Warning,
			TEXT("Taxiway width unchanged: the content set declares no taxiway profiles, so "
			     "there is nothing to cycle through. Author them with "
			     "Tools/Python/build_road_profiles.py."));
		return;
	}

	// FROM THE LEVEL'S DEFAULT INTO THE LIST, then round it. INDEX_NONE is not a slot in
	// the cycle - it is "whatever this level was tuned for" - so the first press picks the
	// narrowest standard width rather than the one after some remembered position.
	WidthIndex = WidthIndex == INDEX_NONE ? 0 : (WidthIndex + 1) % Count;

	// The width is otherwise visible only in the ghost, and only once a chain is started -
	// so a player who has not clicked yet has no way to tell the key did anything.
	const URoadProfile* Profile = Context.Target->ResolveTaxiwayProfile(WidthIndex);
	UE_LOG(LogAirside, Log, TEXT("Taxiway width -> %d of %d, %.1f m"),
		WidthIndex + 1, Count, Profile != nullptr ? Profile->GetTotalWidth() / 100.0 : 0.0);

	// The part-drawn chain, if any, must hear about it: the state carries its own copy so
	// it can build its successor, and a chain left on the old width would finish at a
	// width the ghost has stopped showing.
	if (State.IsValid())
	{
		State->WidthIndex = WidthIndex;
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
	// than dragging a road shorter than the solver can trim. No RebuildMesh() here any more -
	// MoveNode notifies every successful call, drag frame included (issue #77).
	Context.Target->MoveNode(DragNode, Context.Cursor);
}

void FRoadDrawTool::OnDragEnd(const FToolContext& Context)
{
	if (DragNode == INDEX_NONE || Context.Target == nullptr)
	{
		return;
	}

	DragNode = INDEX_NONE;
	Context.Target->EndInteractiveEdit(/*bKeep*/ true);
	// No RebuildMesh() here any more - the last OnDrag's MoveNode already notified for the
	// final position; EndInteractiveEdit only closes the undo step, it moves nothing (#77).
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
	if (Context.Network() == nullptr || !Context.Target->MakeLiveNodeId(Pending, FromId))
	{
		Context.Target->HideGhost();
		return;
	}

	// Shown even when illegal, coloured rather than withheld: hiding it would answer "why
	// can I not build here" with nothing at all.
	const ERoadPlacement Judgement =
		RoadPlacement::Validate(*Context.Network(), FromId, RoadGuidedSnap(Context), Context.Limits);
	Context.Target->UpdateGhost(Pending, RoadGuidedSnap(Context),
		Judgement == ERoadPlacement::Valid, Kind, WidthIndex);
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
	Sink.Marker(RoadGuidedSnap(Context).Position, EPreviewStyle::Pending);

	// THE DASHED LINE TO WHAT IT IS LINED UP WITH, one per winner - the same emission the plot
	// gesture makes, and deliberately the same shape: two tools drawing one meaning two
	// different ways would be presentation drifting apart inside the plugin.
	//
	// FROM THE POINT THE CLICK WOULD TAKE, so the line touches the marker above rather than
	// floating beside it.
	if (Context.Guide.bActive)
	{
		const FVector2D Moving = RoadGuidedSnap(Context).Position;
		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			Sink.Line(Moving, Winner.ReferenceAt, EPreviewStyle::Guide);

			// At the line's MIDPOINT: two labels at the moving point overprint, and the plugin
			// has no camera to offset them by a readable number of pixels. Design section 6.
			Sink.Label((Moving + Winner.ReferenceAt) * 0.5, Winner.Description,
				EPreviewStyle::Guide);
		}
	}

	if (Context.Snap.Kind == ERoadSnapKind::Segment && Context.Network() != nullptr)
	{
		const TArray<FRoadSegment>& Segments = Context.Network()->GetSegments();
		const int32 Index = Context.Snap.Segment.Index;
		if (Segments.IsValidIndex(Index) && Segments[Index].bAlive)
		{
			const FRoadNode* EndA = Context.Network()->GetNode(Segments[Index].A);
			const FRoadNode* EndB = Context.Network()->GetNode(Segments[Index].B);
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
