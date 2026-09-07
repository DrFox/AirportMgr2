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
	if (Node->HoldingPosition == EHoldingPositionKind::Runway)
	{
		// Not the player's. A runway-holding position is derived at every taxiway end on a
		// runway (spec 2026-09-07); clearing one would come back on the next rebuild and
		// setting one is what the builder already did. Said on screen AND in the log: "I
		// clicked and nothing happened" is the report this project gets.
		LastRefusal = TEXT("Runway holding positions are derived from the runway");
		UE_LOG(LogAirside, Log,
			TEXT("Holding point refused at guideline node %d: a runway-holding position is derived, not placed"),
			Picked.Index);
		return;
	}
	// A TOGGLE - see the class comment for why this is not a modifier. The return is
	// HONOURED rather than discarded: the facade refuses a dead slot, and swallowing that
	// would leave the player clicking a position that will not change with nothing on
	// screen to say why. The reason itself stays in the log, because it names slot indices
	// that mean nothing to a player.
	const bool bSet = Node->HoldingPosition == EHoldingPositionKind::None;
	if (!Context.Target->SetIntermediateHoldingPosition(Picked.Index, bSet))
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
			// Refused on a runway-holding position - the click will do nothing but say so.
			Sink.Marker(Node->Position,
				Node->HoldingPosition == EHoldingPositionKind::Runway ? EPreviewStyle::Refused :
				Node->HoldingPosition == EHoldingPositionKind::Intermediate ? EPreviewStyle::Doomed :
				EPreviewStyle::Snap);
		}
	}

	if (!LastRefusal.IsEmpty())
	{
		Sink.Label(Context.Cursor, LastRefusal, EPreviewStyle::Refused);
	}
}

#undef LOCTEXT_NAMESPACE
