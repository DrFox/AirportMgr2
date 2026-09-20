#include "Tool/EditTool.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadNode.h"
#include "Profiles/RoadProfile.h"
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

void FEditTool::OnDragBegin(const FToolContext& Context)
{
	if (Context.Target == nullptr || Context.Snap.Kind != ERoadSnapKind::Node)
	{
		return;
	}

	// ONLY A HANDLE THIS TOOL OFFERED. The snap chain finds every live node; the lit tool
	// decides which of them this mode may touch. A drag that ignored the filter would move
	// a taxiway node while the player had the Runway button lit and was looking only at
	// thresholds - the handles drawn on screen would not be the things that move.
	TArray<int32> Handles;
	GatherNodeHandles(Context, Handles);
	if (!Handles.Contains(Context.Snap.Node.Index))
	{
		return;
	}

	DragNode = Context.Snap.Node.Index;

	// One undo step for the whole drag, not one per frame.
	Context.Target->BeginInteractiveEdit(TEXT("move node"));
	UE_LOG(LogAirside, Log, TEXT("Edit: grabbed node %d"), DragNode);
}

void FEditTool::OnDrag(const FToolContext& Context)
{
	if (DragNode == INDEX_NONE || Context.Target == nullptr)
	{
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
	Context.Target->MoveNode(DragNode, To);
}

void FEditTool::OnDragEnd(const FToolContext& Context)
{
	if (DragNode == INDEX_NONE || Context.Target == nullptr)
	{
		return;
	}

	DragNode = INDEX_NONE;

	// EndInteractiveEdit only closes the undo step - it moves nothing, the last OnDrag
	// having already placed the node and notified for it (#77).
	Context.Target->EndInteractiveEdit(/*bKeep*/ true);
}

void FEditTool::OnDeactivate(const FToolContext& Context)
{
	if (DragNode != INDEX_NONE && Context.Target != nullptr)
	{
		Context.Target->EndInteractiveEdit(/*bKeep*/ true);
		DragNode = INDEX_NONE;
	}
}

bool FEditTool::DescribeGuideAnchor(const URoadNetwork* Network, IRoadEditTarget* Target,
	FGuideAnchor& Out) const
{
	if (Network == nullptr || DragNode == INDEX_NONE || !Network->GetNodes().IsValidIndex(DragNode))
	{
		return false;
	}

	const FRoadNode& Dragged = Network->GetNodes()[DragNode];

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
		const FRoadNodeId Self = Network->NodeIdAt(DragNode);
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
	RoadGuideAnchor::AddNodeCandidates(*Network, Dragged.Position, DragNode, Out);
	return true;
}

void FEditTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork* Network = Context.Network();
	if (Network == nullptr)
	{
		return;
	}

	TArray<int32> Handles;
	GatherNodeHandles(Context, Handles);

	// EVERY GRABBABLE POINT, not only the one under the cursor: "what can I move here" has
	// to be answerable by looking, or the mode is as undiscoverable as the drag it replaces.
	for (const int32 Index : Handles)
	{
		Sink.Marker(Network->GetNodes()[Index].Position, EPreviewStyle::Handle);
	}

	// AND THE ONE UNDER THE CURSOR AGAIN, as Hover. Two meanings - "grabbable" and "this
	// one" - so two styles; the overlay draws them at different radii so neither simply
	// overdraws the other, the same arrangement StandPose and Pending already have.
	if (Context.Snap.Kind == ERoadSnapKind::Node && Handles.Contains(Context.Snap.Node.Index))
	{
		Sink.Marker(Context.Snap.Position, EPreviewStyle::Hover);
	}

	// SAYING YES TO A GUIDE IS HALF THE WORK. IBuildTool::WantsFreeStartGuides records a tool
	// that described an anchor, had a guide computed for it and drew nothing - which showed
	// the player precisely what having no guide shows them. Airside.Tool.EditModeDragOffersGuides
	// measures this drawing rather than the describing, for that reason.
	if (DragNode != INDEX_NONE && Context.Guide.bActive)
	{
		const FVector2D Moving = Context.Guide.Point;
		for (const SnapGuide::FCandidate& Winner : Context.Guide.Winners)
		{
			Sink.Line(Moving, Winner.ReferenceAt, EPreviewStyle::Guide);

			// The label at its own line's midpoint, not at the node: two guides put both
			// labels on one point otherwise, and the plugin has no camera to offset them by
			// a readable number of pixels. FPlotPlaceTool made the same choice for the same
			// reason.
			Sink.Label((Moving + Winner.ReferenceAt) * 0.5, Winner.Description,
				EPreviewStyle::Guide);
		}
	}
}

#undef LOCTEXT_NAMESPACE
