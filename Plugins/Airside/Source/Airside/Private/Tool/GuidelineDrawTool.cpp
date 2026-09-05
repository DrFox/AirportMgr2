#include "Tool/GuidelineDrawTool.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** Nearest ALIVE guideline node within Radius, whatever it is incident to. */
	FGuidelineNodeId NearestAnyNode(const URoadNetwork& Network, const FVector2D& At, double Radius)
	{
		FGuidelineNodeId Best;
		double BestDistance = Radius;

		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (!Nodes[Index].bAlive)
			{
				continue;
			}

			const double Distance = FVector2D::Distance(Nodes[Index].Position, At);
			if (Distance <= BestDistance)
			{
				BestDistance = Distance;
				Best.Index = Index;
				Best.Generation = Nodes[Index].Generation;
			}
		}
		return Best;
	}

	/**
	 * Nearest HAND-AUTHORED edge within Radius, measured on its chord.
	 *
	 * Derived edges are not offered: removing one is undone by the very next rebuild, so
	 * obeying the click would look exactly like ignoring it.
	 */
	FGuidelineEdgeId NearestHandEdge(const URoadNetwork& Network, const FVector2D& At, double Radius)
	{
		FGuidelineEdgeId Best;
		double BestDistance = Radius;

		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || Edge.bDerived)
			{
				continue;
			}

			const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
			const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}

			const double T = RoadGeom::ClosestPointOnSegment(A->Position, B->Position, At);
			const double Distance =
				FVector2D::Distance(FMath::Lerp(A->Position, B->Position, T), At);

			if (Distance <= BestDistance)
			{
				BestDistance = Distance;
				Best.Index = Index;
				Best.Generation = Edge.Generation;
			}
		}
		return Best;
	}
}

#define LOCTEXT_NAMESPACE "Airside"

FText FGuidelineDrawTool::GetDisplayName() const
{
	return LOCTEXT("GuidelineDrawTool", "Guidelines");
}

EGuidelineLink FGuidelineDrawTool::Validate(const URoadNetwork& Network,
	FGuidelineNodeId From, FGuidelineNodeId To)
{
	if (Network.GetGuidelineNode(From) == nullptr || Network.GetGuidelineNode(To) == nullptr)
	{
		return EGuidelineLink::NoStart;
	}

	if (From == To)
	{
		return EGuidelineLink::SameNode;
	}

	// Asked of the START node's incidence rather than by scanning every edge: the answer is
	// the same and the work is proportional to one node's degree.
	const FGuidelineNode* Start = Network.GetGuidelineNode(From);
	for (const FGuidelineEdgeId& Incident : Start->Incident)
	{
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Incident);
		if (Edge == nullptr || !Edge->bAlive)
		{
			continue;
		}
		if (Edge->A == To || Edge->B == To)
		{
			return EGuidelineLink::AlreadyJoined;
		}
	}

	return EGuidelineLink::Valid;
}

const TCHAR* FGuidelineDrawTool::Describe(EGuidelineLink Result)
{
	switch (Result)
	{
	case EGuidelineLink::Valid:         return TEXT("");
	case EGuidelineLink::NoStart:       return TEXT("nothing to link here");
	case EGuidelineLink::SameNode:      return TEXT("same node");
	case EGuidelineLink::AlreadyJoined: return TEXT("already joined");
	default:                            return TEXT("refused");
	}
}

FGuidelineNodeId FGuidelineDrawTool::PickNode(const FToolContext& Context) const
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return FGuidelineNodeId();
	}
	return NearestAnyNode(*Context.Target->GetNetwork(), Context.Cursor, Context.SnapRadius);
}

void FGuidelineDrawTool::OnClick(const FToolContext& Context)
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return;
	}

	// Ctrl: take a link away. Handled before the start/finish states so it never leaves a
	// half-drawn link behind when the player only meant to delete something.
	if (Context.bRemoveModifier)
	{
		const FGuidelineEdgeId Doomed =
			NearestHandEdge(*Context.Target->GetNetwork(), Context.Cursor, Context.SnapRadius);
		if (Doomed.IsSet())
		{
			Context.Target->DisconnectGuideline(Doomed.Index);
		}
		bHasStart = false;
		return;
	}

	const FGuidelineNodeId Picked = PickNode(Context);
	if (!Picked.IsSet())
	{
		return;
	}

	if (!bHasStart)
	{
		StartNode = Picked;
		bHasStart = true;
		return;
	}

	// Refusals leave the START in place rather than dropping it. Clearing it would make a
	// mis-aimed second click cost the first one too.
	if (Validate(*Context.Target->GetNetwork(), StartNode, Picked) != EGuidelineLink::Valid)
	{
		return;
	}

	Context.Target->ConnectGuidelines(StartNode.Index, Picked.Index);
	bHasStart = false;
}

void FGuidelineDrawTool::OnCancel(const FToolContext& Context)
{
	bHasStart = false;
}

void FGuidelineDrawTool::OnDeactivate(const FToolContext& Context)
{
	bHasStart = false;
}

void FGuidelineDrawTool::BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const
{
	if (Context.Target == nullptr || Context.Target->GetNetwork() == nullptr)
	{
		return;
	}

	const URoadNetwork& Network = *Context.Target->GetNetwork();

	// Removal reads differently from drawing, so it previews differently: the link that
	// would go, marked as doomed, and nothing about starting a new one.
	if (Context.bRemoveModifier)
	{
		const FGuidelineEdgeId Doomed = NearestHandEdge(Network, Context.Cursor, Context.SnapRadius);
		if (const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Doomed))
		{
			const FGuidelineNode* A = Network.GetGuidelineNode(Edge->A);
			const FGuidelineNode* B = Network.GetGuidelineNode(Edge->B);
			if (A != nullptr && B != nullptr)
			{
				Sink.Line(A->Position, B->Position, EPreviewStyle::Doomed);
			}
		}
		return;
	}

	const FGuidelineNodeId Hover = PickNode(Context);

	if (bHasStart)
	{
		if (const FGuidelineNode* Start = Network.GetGuidelineNode(StartNode))
		{
			Sink.Marker(Start->Position, EPreviewStyle::Snap);

			// To the node it would ATTACH to when there is one, so the line ends where the
			// link will, not where the mouse happens to be inside the pick radius.
			const FGuidelineNode* End = Network.GetGuidelineNode(Hover);
			const FVector2D To = (End != nullptr) ? End->Position : Context.Cursor;
			Sink.Line(Start->Position, To, EPreviewStyle::Pending);

			if (Hover.IsSet())
			{
				const EGuidelineLink Judgement = Validate(Network, StartNode, Hover);
				if (Judgement != EGuidelineLink::Valid)
				{
					Sink.Label(Context.Cursor, Describe(Judgement), EPreviewStyle::Refused);
				}
			}
		}
	}

	if (Hover.IsSet())
	{
		if (const FGuidelineNode* Node = Network.GetGuidelineNode(Hover))
		{
			Sink.Marker(Node->Position, EPreviewStyle::Snap);
		}
	}
}

#undef LOCTEXT_NAMESPACE
