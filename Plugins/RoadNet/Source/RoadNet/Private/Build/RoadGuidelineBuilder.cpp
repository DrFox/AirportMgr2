#include "Build/RoadGuidelineBuilder.h"

#include "Build/RoadMeshBuilder.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace
{
	/**
	 * Where a guideline crosses a cut line, as a lerp parameter from the right cut to the
	 * left. Mirrors FRoadProfileBands' convention so the two never disagree about which
	 * way "right to left" runs.
	 */
	double AlphaForOffset(const URoadProfile* Profile, double CentreOffset)
	{
		const double HalfLeft  = Profile ? FMath::Max(Profile->GetHalfWidthLeft(),  0.0) : 0.0;
		const double HalfRight = Profile ? FMath::Max(Profile->GetHalfWidthRight(), 0.0) : 0.0;
		const double Total = HalfLeft + HalfRight;
		if (Total <= 0.0)
		{
			return 0.5;
		}
		return FMath::Clamp((CentreOffset + HalfRight) / Total, 0.0, 1.0);
	}
}

void FRoadGuidelineBuilder::Build(URoadNetwork& Network, const FRoadSolveResult& Solved)
{
	// Clear the previous derivation before regenerating, or Build accumulates.
	//
	// bDerived == false edges are the player's, not ours: regenerating one would discard a
	// deliberate edit and replace it with something indistinguishable from correct. They
	// are left in place, and so are the nodes they still reference - RemoveGuidelineNode
	// takes incident edges with it, so a node is only safe to drop once nothing kept
	// points at it.
	{
		TArray<FGuidelineEdgeId> Doomed;
		const TArray<FGuidelineEdge>& Existing = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Existing.Num(); ++Index)
		{
			if (Existing[Index].bAlive && Existing[Index].bDerived)
			{
				FGuidelineEdgeId Id;
				Id.Index = Index;
				Id.Generation = Existing[Index].Generation;
				Doomed.Add(Id);
			}
		}
		for (const FGuidelineEdgeId Id : Doomed)
		{
			Network.RemoveGuidelineEdge(Id);
		}

		TArray<FGuidelineNodeId> Orphans;
		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].Incident.Num() == 0)
			{
				FGuidelineNodeId Id;
				Id.Index = Index;
				Id.Generation = Nodes[Index].Generation;
				Orphans.Add(Id);
			}
		}
		for (const FGuidelineNodeId Id : Orphans)
		{
			Network.RemoveGuidelineNode(Id);
		}
	}

	// (SegmentIndex, which end, GuidelineIndex) -> the node that segment end terminates on.
	//
	// The junction loop reuses these handles rather than adding coincident nodes of its
	// own. THAT is what connects the graph: this graph shares endpoints by handle, so two
	// coincident-but-distinct nodes would satisfy every position check while leaving the
	// turn paths as separate sticks nothing can route across.
	//
	// Packed into one integer rather than given a key struct, because a local struct needs
	// a GetTypeHash that ADL can find, and hoisting one to file scope for a lookup this
	// small is not worth it.
	auto EndKey = [](int32 SegmentIndex, bool bEndA, int32 GuidelineIndex) -> uint64
	{
		return (static_cast<uint64>(SegmentIndex) << 32)
			 | (static_cast<uint64>(GuidelineIndex) << 1)
			 | (bEndA ? 1ull : 0ull);
	};

	TMap<uint64, FGuidelineNodeId> Ends;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || !Segment.bSolvedA || !Segment.bSolvedB)
		{
			continue;
		}

		FRoadSegmentId SegmentId;
		SegmentId.Index = Index;
		SegmentId.Generation = Segment.Generation;

		const URoadProfile* Profile = Segment.Profile.Get();
		if (Profile == nullptr)
		{
			continue;
		}

		for (int32 Which = 0; Which < Profile->Guidelines.Num(); ++Which)
		{
			const FProfileGuideline& Declared = Profile->Guidelines[Which];
			const double Alpha = AlphaForOffset(Profile, Declared.CentreOffset);

			// End B's cut line is authored from B's point of view, so its left is this
			// segment's right walking A to B - swapped exactly as AddSegment swaps it.
			const FVector2D AtA =
				FRoadMeshBuilder::CutLinePoint(Segment.RightCutA, Segment.LeftCutA, Alpha);
			const FVector2D AtB =
				FRoadMeshBuilder::CutLinePoint(Segment.LeftCutB, Segment.RightCutB, Alpha);

			FGuidelineEdge Edge;
			Edge.A = Network.AddGuidelineNode(AtA);
			Edge.B = Network.AddGuidelineNode(AtB);
			Edge.Control = (AtA + AtB) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(Declared.Class);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = Declared.Direction;
			Edge.Width = Declared.Width;
			Edge.MaxWingspan = Declared.MaxWingspan;
			Edge.DerivedFrom = SegmentId;
			Edge.bDerived = true;

			Ends.Add(EndKey(Index, true,  Which), Edge.A);
			Ends.Add(EndKey(Index, false, Which), Edge.B);

			Network.AddGuidelineEdge(MoveTemp(Edge));
		}
	}

	// Turn paths: one edge per ordered pair of DISTINCT arms at each solved node.
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Pair.Key);
		if (ArmSegments == nullptr || !Pair.Value.bValid)
		{
			continue;
		}

		FRoadNodeId NodeId;
		NodeId.Index = Pair.Key;
		const FRoadNode* Node = Network.GetNodes().IsValidIndex(Pair.Key)
			? &Network.GetNodes()[Pair.Key] : nullptr;
		if (Node == nullptr || !Node->bAlive)
		{
			continue;
		}
		NodeId.Generation = Node->Generation;

		for (int32 From = 0; From < ArmSegments->Num(); ++From)
		{
			for (int32 To = 0; To < ArmSegments->Num(); ++To)
			{
				// No U-turns: a junction does not connect an arm to itself.
				if (From == To)
				{
					continue;
				}

				const FRoadSegmentId FromSeg = (*ArmSegments)[From];
				const FRoadSegmentId ToSeg   = (*ArmSegments)[To];
				const FRoadSegment* FromSegment = Network.GetSegment(FromSeg);
				const FRoadSegment* ToSegment   = Network.GetSegment(ToSeg);
				if (FromSegment == nullptr || ToSegment == nullptr)
				{
					continue;
				}

				const URoadProfile* FromProfile = FromSegment->Profile.Get();
				const URoadProfile* ToProfile   = ToSegment->Profile.Get();
				if (FromProfile == nullptr || ToProfile == nullptr)
				{
					continue;
				}

				const int32 Count = FMath::Min(
					FromProfile->Guidelines.Num(), ToProfile->Guidelines.Num());

				for (int32 Which = 0; Which < Count; ++Which)
				{
					const FGuidelineNodeId* FromEnd =
						Ends.Find(EndKey(FromSeg.Index, FromSegment->A == NodeId, Which));
					const FGuidelineNodeId* ToEnd =
						Ends.Find(EndKey(ToSeg.Index, ToSegment->A == NodeId, Which));
					if (FromEnd == nullptr || ToEnd == nullptr)
					{
						continue;
					}

					const FProfileGuideline& Declared = FromProfile->Guidelines[Which];

					FGuidelineEdge Turn;
					Turn.A = *FromEnd;
					Turn.B = *ToEnd;

					// Both arms' tangent lines meet AT the node, so the single control
					// point they define is the node itself - which is precisely the
					// quadratic case, and why this is not the parent spec's cubic.
					Turn.Control = Node->Position;

					Turn.AllowedTraffic = FTrafficMask::Only(Declared.Class);
					Turn.AllowedTraffic.Add(ETraversalClass::Emergency);
					Turn.Direction = EGuidelineDir::AToB;
					Turn.Width = Declared.Width;

					// 0 means UNLIMITED, so a naive Min would let an unlimited arm widen a
					// limited one - wrong in the direction that puts an oversized aircraft
					// onto a turn that cannot take it. A turn is usable only by what BOTH
					// arms admit.
					const double FromLimit = Declared.MaxWingspan;
					const double ToLimit   = ToProfile->Guidelines[Which].MaxWingspan;
					Turn.MaxWingspan =
						(FromLimit <= 0.0) ? ToLimit :
						(ToLimit   <= 0.0) ? FromLimit :
						FMath::Min(FromLimit, ToLimit);
					Turn.bDerived = true;
					// DerivedFrom stays unset: a turn path belongs to the junction, not to
					// either segment, and that is how the two are told apart.

					Network.AddGuidelineEdge(MoveTemp(Turn));
				}
			}
		}
	}
}
