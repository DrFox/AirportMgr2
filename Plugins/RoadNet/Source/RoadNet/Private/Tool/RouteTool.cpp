#include "Tool/RouteTool.h"

#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Present/RoadNetworkActor.h"
#include "Solve/GuidelineGeom.h"

#define LOCTEXT_NAMESPACE "RoadNet"

namespace
{
	/**
	 * The wingspan to route with: that of the stand the journey STARTS at, or 0.
	 *
	 * "An A320 leaves stand 5 for stand 12" is the query worth asking, and the aircraft in
	 * it is the one the origin stand was designed for. Taking it from the DESTINATION
	 * instead would make every route trivially fit, which is the one answer that can never
	 * be wrong and never be useful.
	 */
	double WingspanAtNode(const URoadNetwork& Network, FGuidelineNodeId Node)
	{
		for (const FEntityInstance& Instance : Network.GetEntities())
		{
			if (!Instance.bAlive || Instance.Definition == nullptr
				|| Instance.Definition->DesignAircraft == nullptr)
			{
				continue;
			}

			if (Instance.PoseNode == Node)
			{
				return Instance.Definition->DesignAircraft->Footprint.Wingspan;
			}

			for (const FResolvedAnchor& Anchor : Instance.ResolvedAnchors)
			{
				if (Anchor.Node == Node)
				{
					return Instance.Definition->DesignAircraft->Footprint.Wingspan;
				}
			}
		}

		return 0.0;
	}

	FString DescribeFailure(ERouteResult Result)
	{
		switch (Result)
		{
		case ERouteResult::NoStart:     return TEXT("no start");
		case ERouteResult::NoGoal:      return TEXT("no destination");
		case ERouteResult::SameNode:    return TEXT("already there");
		case ERouteResult::TooWide:     return TEXT("too wide for this route");
		case ERouteResult::Unreachable: return TEXT("not connected");
		default:                        return FString();
		}
	}
}

FText FRouteTool::GetDisplayName() const
{
	return LOCTEXT("RouteTool", "Route");
}

FGuidelineNodeId FRouteTool::PickNode(const FToolContext& Context) const
{
	if (Context.Target == nullptr || Context.Target->Network == nullptr)
	{
		return FGuidelineNodeId();
	}

	// The same radius everything else snaps with, so "on" means one thing in this tool and
	// in every other.
	return RouteSearch::FindNearestNode(
		*Context.Target->Network, Context.Cursor, Class, Context.SnapRadius);
}

void FRouteTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr || Context.Target->Network == nullptr)
	{
		return;
	}

	const FGuidelineNodeId Picked = PickNode(Context);
	if (!Picked.IsSet())
	{
		// A click on empty apron is not a query. Silently ignored rather than clearing the
		// start, because a near miss while aiming for a node would otherwise throw away
		// the half-built query.
		return;
	}

	if (!bHasStart)
	{
		StartNode = Picked;
		bHasStart = true;
		LastPlan = FRoutePlan();
		return;
	}

	LastPlan = Context.Target->FindRoute(
		StartNode, Picked, Class, WingspanAtNode(*Context.Target->Network, StartNode));

	bHasStart = false;

	if (LastPlan.IsValid())
	{
		// Refused in an editor world, deliberately - the route still draws there. See
		// ARoadNetworkActor::DispatchAgent.
		Context.Target->DispatchAgent(LastPlan, Speed);
	}
}

void FRouteTool::OnCancel(const FToolContext& Context)
{
	// One step back: drop the pending start, and only then the drawn route. Clearing both
	// at once would take away the picture the player is looking at to answer "why did it
	// go that way".
	if (bHasStart)
	{
		bHasStart = false;
		return;
	}

	LastPlan = FRoutePlan();
}

void FRouteTool::OnDeactivate(const FToolContext& Context)
{
	bHasStart = false;
	LastPlan = FRoutePlan();
}

void FRouteTool::DrawGraph(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	const URoadNetwork& Network = *Context.Target->Network;

	for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
	{
		if (!Edge.bAlive || !Edge.AllowedTraffic.Allows(Class))
		{
			continue;
		}

		const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
		const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
		if (A == nullptr || B == nullptr)
		{
			continue;
		}

		// The same sampling the search costed and a follower will walk, so what is drawn
		// is what is driven.
		TArray<FVector2D> Points;
		GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Points);

		for (int32 At = 1; At < Points.Num(); ++At)
		{
			Sink.Line(Points[At - 1], Points[At], EPreviewStyle::Guideline);
		}
	}

	for (const FGuidelineNode& Node : Network.GetGuidelineNodes())
	{
		if (Node.bAlive)
		{
			Sink.Marker(Node.Position, EPreviewStyle::Guideline);
		}
	}
}

void FRouteTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Target->Network == nullptr)
	{
		return;
	}

	DrawGraph(Context, Sink);

	if (bHasStart)
	{
		if (const FGuidelineNode* Node = Context.Target->Network->GetGuidelineNode(StartNode))
		{
			Sink.Marker(Node->Position, EPreviewStyle::Snap);

			// The leg that would be routed if the cursor were clicked now. A straight hint,
			// not a search: running A* every frame to draw a line nobody asked for is work
			// spent on a query that has not been made.
			Sink.Line(Node->Position, Context.Cursor, EPreviewStyle::Pending);
		}
	}

	// What the cursor would pick, so a node that cannot be used simply never lights up.
	if (const FGuidelineNodeId Hover = PickNode(Context); Hover.IsSet())
	{
		if (const FGuidelineNode* Node = Context.Target->Network->GetGuidelineNode(Hover))
		{
			Sink.Marker(Node->Position, EPreviewStyle::Snap);
		}
	}

	if (LastPlan.IsValid())
	{
		for (int32 At = 1; At < LastPlan.Polyline.Num(); ++At)
		{
			Sink.Line(LastPlan.Polyline[At - 1], LastPlan.Polyline[At], EPreviewStyle::Route);
		}

		Sink.Label(LastPlan.Polyline.Last(),
			FString::Printf(TEXT("%.0f m"), LastPlan.Length / 100.0), EPreviewStyle::Route);
	}
	else if (LastPlan.Result != ERouteResult::NoStart)
	{
		// A failed query has no line to draw, so the reason goes where the question was
		// asked. Without it "nothing happened" is indistinguishable from "not connected".
		const FString Why = DescribeFailure(LastPlan.Result);
		if (!Why.IsEmpty())
		{
			Sink.Label(Context.Cursor, Why, EPreviewStyle::Refused);
		}
	}
}

#undef LOCTEXT_NAMESPACE
