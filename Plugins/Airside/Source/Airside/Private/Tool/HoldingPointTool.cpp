#include "Tool/HoldingPointTool.h"

#include "AirsideLog.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Tool/RoadEditTarget.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FHoldingPointTool::GetDisplayName() const
{
	// The SAME string the registry carries - Airside.Tool.BuildSession asserts the two
	// agree, and the editor mode logs an error if its command label disagrees with either.
	return LOCTEXT("HoldingPosition", "Holding point");
}

FGuidelineNodeId FHoldingPointTool::PickNode(const FToolContext& Context) const
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return FGuidelineNodeId();
	}

	// AIRCRAFT, and the same radius everything else snaps with, so "on" means one thing in
	// this tool and in every other. A hold bar is aircraft infrastructure: a node no
	// aircraft edge admits is not a bar candidate at all, so it never lights up rather than
	// lighting up and then refusing.
	return RouteSearch::FindNearestNode(
		*Context.Target->GetNetwork(), Context.Cursor, ETraversalClass::Aircraft, Context.SnapRadius);
}

void FHoldingPointTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return;
	}

	const URoadNetwork& Network = *Context.Target->GetNetwork();

	const FGuidelineNodeId Picked = PickNode(Context);
	if (!Picked.IsSet())
	{
		// A click on empty pavement is not a gesture. Silently ignored, and the previous
		// refusal is deliberately left standing: a near miss while aiming for the node that
		// was refused should not wipe the reason it was refused.
		return;
	}

	const FGuidelineNode* Node = Network.GetGuidelineNode(Picked);
	if (Node == nullptr)
	{
		return;
	}

	if (Node->HoldingPositionFor.IsSet())
	{
		// A second click on a flagged node clears it - see the class comment for why this
		// is a toggle rather than a modifier. INDEX_NONE is the facade's "clear".
		//
		// The return is HONOURED rather than discarded: the facade refuses a dead slot, and
		// swallowing that would leave the player clicking a bar that will not go away with
		// nothing on screen to say why. The reason itself stays in the log, because it names
		// slot indices that mean nothing to a player.
		if (!Context.Target->SetIntermediateHoldingPosition(Picked.Index, INDEX_NONE))
		{
			LastRefusal = TEXT("The facade refused; see the log");
			return;
		}
		LastRefusal.Empty();
		return;
	}

	// Asked of the network, not worked out here: RunwayNearGuidelineNode is a fact about
	// the graph, and one that the graph's own tests already pin.
	const FRoadSegmentId Runway = Network.RunwayNearGuidelineNode(Picked);
	if (!Runway.IsSet())
	{
		LastRefusal = TEXT("No runway within one edge of this node");

		// Logged as well as shown. "I clicked and nothing happened" is the report this
		// project gets, and a line in AirportMgr.log settles it without a screenshot.
		UE_LOG(LogAirside, Log,
			TEXT("Holding point refused at guideline node %d: no runway within one edge"),
			Picked.Index);
		return;
	}

	// INDICES, because that is what this seam takes: the facade re-derives the
	// generation-checked handles and refuses a dead slot in one place - see
	// IRoadEditTarget::SetIntermediateHoldingPosition. Honoured, for the reason given on the clear above.
	if (!Context.Target->SetIntermediateHoldingPosition(Picked.Index, Runway.Index))
	{
		LastRefusal = TEXT("The facade refused; see the log");
		return;
	}

	LastRefusal.Empty();
}

void FHoldingPointTool::OnCancel(const FToolContext& Context)
{
	// Nothing is ever part-drawn, so the only thing a cancel can take back is the message.
	LastRefusal.Empty();
}

void FHoldingPointTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return;
	}

	const URoadNetwork& Network = *Context.Target->GetNetwork();

	// The bars themselves are NOT drawn here. GuidelineOverlay is their only emitter, so
	// they are visible under every tool rather than only this one - the same rule the
	// guideline graph follows, and for the same reason: a bar you can see only while the
	// holding-position tool is selected is a bar you forget you placed.
	if (const FGuidelineNodeId Hover = PickNode(Context); Hover.IsSet())
	{
		if (const FGuidelineNode* Node = Network.GetGuidelineNode(Hover))
		{
			// DOOMED on a node that already carries a bar, not Snap. A style names what
			// THIS click would do, and on a flagged node the click REMOVES the bar - so
			// Snap would promise the exact opposite of what is about to happen.
			Sink.Marker(Node->Position,
				Node->HoldingPositionFor.IsSet() ? EPreviewStyle::Doomed : EPreviewStyle::Snap);
		}
	}

	if (!LastRefusal.IsEmpty())
	{
		Sink.Label(Context.Cursor, LastRefusal, EPreviewStyle::Refused);
	}
}

#undef LOCTEXT_NAMESPACE
