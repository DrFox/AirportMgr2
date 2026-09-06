#include "Tool/HoldShortTool.h"

#include "AirsideLog.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Tool/RoadEditTarget.h"

#define LOCTEXT_NAMESPACE "Airside"

FText FHoldShortTool::GetDisplayName() const
{
	// The SAME string the registry carries - Airside.Tool.BuildSession asserts the two
	// agree, and the editor mode logs an error if its command label disagrees with either.
	return LOCTEXT("HoldShort", "Hold short");
}

FGuidelineNodeId FHoldShortTool::PickNode(const FToolContext& Context) const
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

void FHoldShortTool::OnClick(const FToolContext& Context)
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

	if (Node->HoldShortFor.IsSet())
	{
		// A second click on a flagged node clears it - see the class comment for why this
		// is a toggle rather than a modifier. INDEX_NONE is the facade's "clear".
		Context.Target->SetHoldShort(Picked.Index, INDEX_NONE);
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
			TEXT("Hold short refused at guideline node %d: no runway within one edge"),
			Picked.Index);
		return;
	}

	LastRefusal.Empty();

	// INDICES, because that is what this seam takes: the facade re-derives the
	// generation-checked handles and refuses a dead slot in one place - see
	// IRoadEditTarget::SetHoldShort.
	Context.Target->SetHoldShort(Picked.Index, Runway.Index);
}

void FHoldShortTool::OnCancel(const FToolContext& Context)
{
	// Nothing is ever part-drawn, so the only thing a cancel can take back is the message.
	LastRefusal.Empty();
}

void FHoldShortTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return;
	}

	const URoadNetwork& Network = *Context.Target->GetNetwork();

	// The bars themselves are NOT drawn here. GuidelineOverlay is their only emitter, so
	// they are visible under every tool rather than only this one - the same rule the
	// guideline graph follows, and for the same reason: a bar you can see only while the
	// hold-short tool is selected is a bar you forget you placed.
	if (const FGuidelineNodeId Hover = PickNode(Context); Hover.IsSet())
	{
		if (const FGuidelineNode* Node = Network.GetGuidelineNode(Hover))
		{
			// DOOMED on a node that already carries a bar, not Snap. A style names what
			// THIS click would do, and on a flagged node the click REMOVES the bar - so
			// Snap would promise the exact opposite of what is about to happen.
			Sink.Marker(Node->Position,
				Node->HoldShortFor.IsSet() ? EPreviewStyle::Doomed : EPreviewStyle::Snap);
		}
	}

	if (!LastRefusal.IsEmpty())
	{
		Sink.Label(Context.Cursor, LastRefusal, EPreviewStyle::Refused);
	}
}

#undef LOCTEXT_NAMESPACE
