#include "Tool/EditTool.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadApron.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
#include "Tool/RemoveGesture.h"
#include "Tool/RoadGuideAnchor.h"
#include "Tool/RoadNaming.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FEditTool::GetDisplayName() const
{
	return LOCTEXT("EditTool", "Edit");
}

void FEditTool::GatherNodeHandles(const FToolContext& Context, TArray<int32>& Out)
{
	Out.Reset();

	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr || Context.EditHandles == EEditHandleKind::None)
	{
		return;
	}

	const TArray<FRoadNode>& Nodes = Network->GetNodes();
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		// A DELETED NODE KEEPS ITS SLOT. Offering one would draw a handle on a junction the
		// player has already removed - the guard the guide candidates make for the same
		// reason.
		if (!Nodes[Index].bAlive)
		{
			continue;
		}

		bool bWanted = false;
		for (const FRoadSegmentId Arm : Nodes[Index].Incident)
		{
			// THROUGH RoadNaming, the ONE classification - see that header. Asking the
			// profile's guidelines here would be a fourth private answer to a question it
			// records already having been answered four ways once.
			SnapGuide::EReference Of = SnapGuide::EReference::Taxiway;
			if (!RoadNaming::ReferenceOf(*Network, Arm, Of))
			{
				continue;
			}

			switch (Context.EditHandles)
			{
			case EEditHandleKind::AirsideNode:
				bWanted |= (Of == SnapGuide::EReference::Taxiway
						 || Of == SnapGuide::EReference::Runway);
				break;

			case EEditHandleKind::ServiceRoadNode:
				bWanted |= (Of == SnapGuide::EReference::ServiceRoad);
				break;

			case EEditHandleKind::RunwayThreshold:
				// A THRESHOLD IS AN END, not any runway node. A split runway has interior
				// nodes, and dragging one of those sideways would kink the strip rather than
				// reposition it. Exactly one incident arm is what makes a node an end.
				bWanted |= (Of == SnapGuide::EReference::Runway
						 && Nodes[Index].Incident.Num() == 1);
				break;

			default:
				break;
			}

			if (bWanted)
			{
				break;
			}
		}

		if (bWanted)
		{
			Out.Add(Index);
		}
	}
}

bool FEditHandle::PositionIn(const URoadNetwork& Network, FVector2D& Out) const
{
	switch (Kind)
	{
	case EKind::Node:
		if (Network.GetNodes().IsValidIndex(Owner) && Network.GetNodes()[Owner].bAlive)
		{
			Out = Network.GetNodes()[Owner].Position;
			return true;
		}
		return false;

	case EKind::ApronCorner:
	{
		const FApronSurface* Apron = Network.GetApron(Network.ApronIdAt(Owner));
		if (Apron != nullptr && Apron->Outline.IsValidIndex(Corner))
		{
			Out = Apron->Outline[Corner];
			return true;
		}
		return false;
	}

	default:
		return false;
	}
}

void FEditTool::GatherHandles(const FToolContext& Context, TArray<FEditHandle>& Out)
{
	Out.Reset();

	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	if (Context.EditHandles == EEditHandleKind::ApronCorner)
	{
		const TArray<FApronSurface>& Aprons = Network->GetAprons();
		for (int32 Index = 0; Index < Aprons.Num(); ++Index)
		{
			if (!Aprons[Index].bAlive)
			{
				continue;
			}
			for (int32 Corner = 0; Corner < Aprons[Index].Outline.Num(); ++Corner)
			{
				FEditHandle Handle;
				Handle.Kind = FEditHandle::EKind::ApronCorner;
				Handle.Owner = Index;
				Handle.Corner = Corner;
				Out.Add(Handle);
			}
		}
		return;
	}

	TArray<int32> Nodes;
	GatherNodeHandles(Context, Nodes);
	for (const int32 Index : Nodes)
	{
		FEditHandle Handle;
		Handle.Kind = FEditHandle::EKind::Node;
		Handle.Owner = Index;
		Out.Add(Handle);
	}
}

/**
 * The handle nearest the cursor within Radius, or an unset one.
 *
 * BY DISTANCE, not through the snap chain: the chain answers "where would a ROAD NODE go",
 * which is the wrong question for an apron corner and would never mention one. The node
 * path still consults the chain, in OnDragBegin, because there the two agree and the
 * chain's answer is the one the drop must later match bitwise.
 */
static FEditHandle NearestHandle(const FToolContext& Context, const URoadNetwork& Network)
{
	TArray<FEditHandle> Handles;
	FEditTool::GatherHandles(Context, Handles);

	FEditHandle Best;
	double BestSquared = Context.SnapRadius * Context.SnapRadius;
	for (const FEditHandle& Handle : Handles)
	{
		FVector2D At;
		if (!Handle.PositionIn(Network, At))
		{
			continue;
		}
		const double Squared = FVector2D::DistSquared(At, Context.Cursor);
		if (Squared <= BestSquared)
		{
			BestSquared = Squared;
			Best = Handle;
		}
	}
	return Best;
}

/**
 * Whether the thing the snap chain found is one THIS mode offered.
 *
 * The chain answers "where would a road node go" and knows nothing of the lit tool, so it
 * will happily name a service-road node while the Taxiway button is lit. Every gesture in
 * this tool asks the filter before acting, or the handles drawn on screen would not be the
 * things that respond.
 */
static bool IsOfferedHandle(const FToolContext& Context)
{
	if (Context.Snap.Kind != ERoadSnapKind::Node)
	{
		return false;
	}
	TArray<int32> Handles;
	FEditTool::GatherNodeHandles(Context, Handles);
	return Handles.Contains(Context.Snap.Node.Index);
}

void FEditTool::OnClick(const FToolContext& Context)
{
	// A PLAIN CLICK BUILDS NOTHING AND SELECTS NOTHING - see the class comment. Only the
	// held Ctrl means anything here yet.
	if (!Context.bRemoveModifier || Context.Target == nullptr)
	{
		return;
	}

	// HANDLES ONLY, which is also why a segment is not removable from this mode: Edit is
	// about the POINTS the lit tool exposes, they are the things drawn, and the build tools
	// keep segment removal. A Ctrl+click on open pavement here does nothing, deliberately.
	if (!IsOfferedHandle(Context))
	{
		return;
	}

	if (RemoveGesture::Apply(Context))
	{
		UE_LOG(LogAirside, Log, TEXT("Edit: removed node %d"), Context.Snap.Node.Index);
	}
}

void FEditTool::OnDragBegin(const FToolContext& Context)
{
	const URoadNetwork* Network = Context.Network();
	if (Context.Target == nullptr || Network == nullptr)
	{
		return;
	}

	// AN APRON CORNER IS NOT IN THE ROAD GRAPH, so the snap chain will never mention one -
	// it answers "where would a road node go". Picked by distance instead, against the same
	// ToolPickRadius every other non-road pick uses.
	if (Context.EditHandles == EEditHandleKind::ApronCorner)
	{
		Drag = NearestHandle(Context, *Network);
		if (!Drag.IsSet())
		{
			return;
		}
		Context.Target->BeginInteractiveEdit(TEXT("move apron corner"));
		UE_LOG(LogAirside, Log, TEXT("Edit: grabbed apron %d corner %d"), Drag.Owner, Drag.Corner);
		return;
	}

	// A NODE COMES THROUGH THE SNAP CHAIN, not by distance, and the difference matters: the
	// chain's Node result carries the graph's stored coordinates verbatim, and the drop has
	// to match them bitwise for a merge to be exact.
	if (Context.Snap.Kind != ERoadSnapKind::Node)
	{
		return;
	}

	// ONLY A HANDLE THIS TOOL OFFERED - see IsOfferedHandle. A drag that ignored the filter
	// would move a taxiway node while the player had the Runway button lit and was looking
	// only at thresholds.
	if (!IsOfferedHandle(Context))
	{
		return;
	}

	Drag.Kind = FEditHandle::EKind::Node;
	Drag.Owner = Context.Snap.Node.Index;

	// One undo step for the whole drag, not one per frame.
	Context.Target->BeginInteractiveEdit(TEXT("move node"));
	UE_LOG(LogAirside, Log, TEXT("Edit: grabbed node %d"), Drag.Owner);
}

void FEditTool::OnDrag(const FToolContext& Context)
{
	if (!Drag.IsSet() || Context.Target == nullptr)
	{
		return;
	}

	if (Drag.Kind == FEditHandle::EKind::ApronCorner)
	{
		// The guided cursor, but never a road snap: an apron corner landing exactly on a road
		// node is not a merge and has no meaning - the two live in different structures.
		Context.Target->MoveApronCorner(Drag.Owner, Drag.Corner, Context.GuidedCursor());
		return;
	}

	// THE SNAPPED, GUIDED POSITION - the whole of "editing should feel like placing".
	// FRoadDrawTool passed the RAW cursor here, which is why a drag ignored both the snap
	// chain and the guides that landed in #162.
	//
	// SNAP FIRST, GUIDE SECOND, which is the order a click already resolves them in (see
	// Airside.Tool.RoadSnapBeatsTheGuide). A Node snap carries the target's stored
	// coordinates VERBATIM, and that exactness is what a merge is built on: the two nodes
	// must be at one position, not at two that round to the same pixel.
	const FVector2D To = Context.Snap.Kind == ERoadSnapKind::Node
		? Context.Snap.Position
		: Context.GuidedCursor();

	// A refused move simply does not happen, so the node stops following the cursor rather
	// than dragging a road shorter than the solver can trim. MoveNode notifies on every
	// successful call, drag frame included (issue #77), so no RebuildMesh here.
	Context.Target->MoveNode(Drag.Owner, To);
}

void FEditTool::OnDragEnd(const FToolContext& Context)
{
	if (!Drag.IsSet() || Context.Target == nullptr)
	{
		return;
	}

	const FEditHandle Dropped = Drag;
	Drag.Clear();

	// DROPPING ON A NODE IS THE MERGE. There is no separate verb and no confirmation, for
	// the reason ERoadSnapKind::Node already gives about a click: "clicking reuses it, which
	// is how a junction is closed". This is that, for a node that already exists.
	//
	// KEEP IS THE NODE THE PLAYER AIMED AT, absorb the one in their hand - so the thing they
	// were pointing to survives. The other way round would move the target instead, which is
	// the opposite of what the gesture says.
	//
	// BEFORE EndInteractiveEdit, so the merge joins the drag's undo step rather than opening
	// one of its own: a drop is one action to the player and must be one press of undo.
	//
	// NODES ONLY. An apron corner dropped on a road node is two unrelated structures meeting
	// at a coordinate, not a thing to fold together.
	if (Dropped.Kind == FEditHandle::EKind::Node
		&& Context.Snap.Kind == ERoadSnapKind::Node
		&& Context.Snap.Node.Index != Dropped.Owner)
	{
		Context.Target->MergeNodes(Context.Snap.Node.Index, Dropped.Owner);
	}

	// EndInteractiveEdit only closes the undo step - it moves nothing, the last OnDrag
	// having already placed the handle and notified for it (#77).
	Context.Target->EndInteractiveEdit(/*bKeep*/ true);
}

void FEditTool::OnDeactivate(const FToolContext& Context)
{
	if (Drag.IsSet() && Context.Target != nullptr)
	{
		Context.Target->EndInteractiveEdit(/*bKeep*/ true);
		Drag.Clear();
	}
}

bool FEditTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	// NODES ONLY. An apron corner's neighbours are its own outline rather than road arms, so
	// the anchor below - built from incident segments and road-node candidates - would be
	// describing something it is not holding.
	if (Network == nullptr || Drag.Kind != FEditHandle::EKind::Node
		|| !Network->GetNodes().IsValidIndex(Drag.Owner))
	{
		return false;
	}

	const FRoadNode& Dragged = Network->GetNodes()[Drag.Owner];

	Out.bFreeStart = true;
	Out.Point = EDragPoint::Centreline;

	// THE WIDEST ARM'S PROFILE. A node's arms may differ, and a guide has to be displaced by
	// the pavement that will actually be drawn - through ResolveProfileFor, the one resolver,
	// so a guide cannot disagree with the thing it is guiding. Widest because that is the
	// edge the player is lining up: a narrow arm tucked inside a wide one displaces nothing
	// the eye can see.
	if (Target != nullptr)
	{
		double Widest = 0.0;
		for (const FRoadSegmentId Arm : Dragged.Incident)
		{
			const FRoadSegment* Segment = Network->GetSegment(Arm);
			const URoadProfile* Profile = Segment != nullptr ? Segment->Profile : nullptr;
			if (Profile != nullptr && Profile->GetTotalWidth() > Widest)
			{
				Widest = Profile->GetTotalWidth();
				Out.HalfWidthLeft = Profile->GetHalfWidthLeft();
				Out.HalfWidthRight = Profile->GetHalfWidthRight();
			}
		}
	}

	// EXACTLY ONE ARM GIVES A DIRECTION TO HOLD - the road this node ends. With two or more
	// no arm is "the" one, and picking whichever is stored first would make the guide change
	// with an edit nobody connected to guides at all: the same rule, and the same reason,
	// FRoadDrawTool's own anchor gives about a junction.
	if (Dragged.Incident.Num() == 1)
	{
		const FRoadNodeId Self = Network->NodeIdAt(Drag.Owner);
		const FRoadNodeId Far = Network->GetOtherEnd(Dragged.Incident[0], Self);
		if (const FRoadNode* Other = Network->GetNode(Far))
		{
			const FVector2D Along = (Dragged.Position - Other->Position).GetSafeNormal();
			if (!Along.IsNearlyZero())
			{
				Out.Reference = Along;
				Out.ReferenceAt = Other->Position;
				Out.ReferenceName = TEXT("this road");
			}
		}
	}

	// THE SHARED CANDIDATE LOOP, excluding the node in hand for the identical reason the
	// chain excludes the one it extends from - see RoadGuideAnchor.
	RoadGuideAnchor::AddNodeCandidates(*Network, Dragged.Position, Drag.Owner, Out);
	return true;
}

void FEditTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	// WHAT A CTRL+CLICK WOULD TAKE, through the one shared answer - see RemoveGesture.
	//
	// FIRST AND ALONE. Everything below describes something CONSTRUCTIVE - points you may
	// grab, a node you would merge into, a line you are squaring to - and none of it is the
	// gesture the player is making while Ctrl is down. FRoadDrawTool hides its ghost under
	// Ctrl for the same reason its comment gives: a preview that offers to build the thing
	// about to be removed is two futures at once.
	if (Context.bRemoveModifier)
	{
		if (IsOfferedHandle(Context))
		{
			RemoveGesture::Describe(Context, Sink);
		}
		return;
	}

	TArray<FEditHandle> Handles;
	GatherHandles(Context, Handles);

	// EVERY GRABBABLE POINT, not only the one under the cursor: "what can I move here" has
	// to be answerable by looking, or the mode is as undiscoverable as the drag it replaces.
	for (const FEditHandle& Handle : Handles)
	{
		FVector2D At;
		if (Handle.PositionIn(*Network, At))
		{
			Sink.Marker(At, EPreviewStyle::Handle);
		}
	}

	// AND THE ONE UNDER THE CURSOR AGAIN, as Hover. Two meanings - "grabbable" and "this
	// one" - so two styles; the overlay draws them at different radii so neither simply
	// overdraws the other, the same arrangement StandPose and Pending already have.
	const FEditHandle Under = NearestHandle(Context, *Network);
	FVector2D HoverAt;
	if (Under.IsSet() && Under.PositionIn(*Network, HoverAt))
	{
		Sink.Marker(HoverAt, EPreviewStyle::Hover);
	}

	// WHAT A DROP WOULD MERGE INTO, and what it would destroy. Snap already means "the
	// gesture would attach to this"; the label is what says the attachment REMOVES a node,
	// which is the one thing a ring cannot convey on its own.
	if (Drag.Kind == FEditHandle::EKind::Node && Context.Snap.Kind == ERoadSnapKind::Node
		&& Context.Snap.Node.Index != Drag.Owner)
	{
		Sink.Marker(Context.Snap.Position, EPreviewStyle::Snap);
		Sink.Label(Context.Snap.Position, TEXT("merge"), EPreviewStyle::Snap);

		// THE ARM BETWEEN THEM IS DOOMED - it collapses, because once the two nodes are one
		// it has no length and no direction. Drawn so the player sees which road disappears
		// BEFORE they let go, rather than afterwards.
		if (Network->GetNodes().IsValidIndex(Drag.Owner))
		{
			const FRoadNode& Held = Network->GetNodes()[Drag.Owner];
			for (const FRoadSegmentId Arm : Held.Incident)
			{
				if (Network->GetOtherEnd(Arm, Network->NodeIdAt(Drag.Owner)) == Context.Snap.Node)
				{
					Sink.Line(Held.Position, Context.Snap.Position, EPreviewStyle::Doomed);
				}
			}
		}
	}

	// SAYING YES TO A GUIDE IS HALF THE WORK. IBuildTool::WantsFreeStartGuides records a tool
	// that described an anchor, had a guide computed for it and drew nothing - which showed
	// the player precisely what having no guide shows them. Airside.Tool.EditModeDragOffersGuides
	// measures this drawing rather than the describing, for that reason.
	if (Drag.IsSet() && Context.Guide.bActive)
	{
		const FVector2D Moving = Context.Guide.Point;
		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			Sink.Line(Moving, Winner.ReferenceAt, EPreviewStyle::Guide);

			// The label at its own line's midpoint, not at the handle: two guides put both
			// labels on one point otherwise, and the plugin has no camera to offset them by
			// a readable number of pixels. FPlotPlaceTool made the same choice for the same
			// reason.
			Sink.Label((Moving + Winner.ReferenceAt) * 0.5, Winner.Description,
				EPreviewStyle::Guide);
		}
	}
}

#undef LOCTEXT_NAMESPACE
