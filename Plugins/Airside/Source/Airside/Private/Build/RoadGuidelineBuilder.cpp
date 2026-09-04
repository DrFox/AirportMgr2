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

	/** A live, hand-edited guideline already covering this segment's Nth declared guideline. */
	FGuidelineEdgeId FindSparedEdge(const URoadNetwork& Network, FRoadSegmentId Segment, int32 Which)
	{
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (Edge.bAlive && !Edge.bDerived &&
				Edge.DerivedFrom == Segment && Edge.DerivedGuidelineIndex == Which)
			{
				FGuidelineEdgeId Id;
				Id.Index = Index;
				Id.Generation = Edge.Generation;
				return Id;
			}
		}
		return FGuidelineEdgeId();
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

	}

	// The orphan sweep used to sit in the block above, before anything was derived. It runs
	// at the END now, because re-resolution below MOVES hand-authored edges off the nodes
	// they were drawn between - a sweep that ran first would leave those behind alive and
	// idle for ever, and the overlay would draw every one of them.
	//
	// Running last is strictly safer: it is the only point at which every detachment this
	// function performs has already happened.

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
			// A guideline the player edited survived the clear pass. Deriving over it would
			// leave TWO guidelines on this segment - the player's, attached to nothing, and
			// a fresh derived one that every turn path and every route would use instead.
			// The edit would appear to have done nothing at all.
			const FGuidelineEdgeId Spared = FindSparedEdge(Network, SegmentId, Which);
			if (Spared.IsSet())
			{
				if (const FGuidelineEdge* SparedEdge = Network.GetGuidelineEdge(Spared))
				{
					// Register the player's OWN endpoints, so the turn paths below attach
					// to their line instead of to one nothing can reach.
					Ends.Add(EndKey(Index, true,  Which), SparedEdge->A);
					Ends.Add(EndKey(Index, false, Which), SparedEdge->B);
					continue;
				}
			}

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

			// Each end records WHICH end it is, so a hand-authored edge can name what it
			// attached to rather than which slot happened to hold it. Handles do not
			// survive a rebuild; this does.
			if (FGuidelineNode* NodeA = Network.GetGuidelineNodeMutable(Edge.A))
			{
				NodeA->Origin.Segment = SegmentId;
				NodeA->Origin.bEndA = true;
				NodeA->Origin.GuidelineIndex = Which;
			}
			if (FGuidelineNode* NodeB = Network.GetGuidelineNodeMutable(Edge.B))
			{
				NodeB->Origin.Segment = SegmentId;
				NodeB->Origin.bEndA = false;
				NodeB->Origin.GuidelineIndex = Which;
			}
			Edge.Control = (AtA + AtB) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(Declared.Class);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = Declared.Direction;
			Edge.Width = Declared.Width;
			Edge.MaxWingspan = Declared.MaxWingspan;
			Edge.DerivedFrom = SegmentId;
			Edge.DerivedGuidelineIndex = Which;
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

					// Arriving at this node along the From arm, then leaving along the To
					// arm. Either arm may be one-way, and a turn that ignores that lands an
					// agent on a node it cannot leave.
					const bool bFromAtA = (FromSegment->A == NodeId);
					const bool bToAtA   = (ToSegment->A == NodeId);
					const EGuidelineDir FromDir = FromProfile->Guidelines[Which].Direction;
					const EGuidelineDir ToDir   = ToProfile->Guidelines[Which].Direction;

					const bool bMayArrive =
						FromDir == EGuidelineDir::Bidirectional ||
						(bFromAtA  && FromDir == EGuidelineDir::BToA) ||
						(!bFromAtA && FromDir == EGuidelineDir::AToB);

					const bool bMayLeave =
						ToDir == EGuidelineDir::Bidirectional ||
						(bToAtA  && ToDir == EGuidelineDir::AToB) ||
						(!bToAtA && ToDir == EGuidelineDir::BToA);

					if (!bMayArrive || !bMayLeave)
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

					// A turn is usable only by what BOTH arms admit - the same reasoning
					// already applied to MaxWingspan below, which this previously
					// contradicted one line up. Where the two arms carry different classes
					// the intersection leaves Emergency alone, which is right: a fire truck
					// may cross between a service road and a taxiway and nothing else may.
					FTrafficMask FromMask = FTrafficMask::Only(FromProfile->Guidelines[Which].Class);
					FromMask.Add(ETraversalClass::Emergency);
					FTrafficMask ToMask = FTrafficMask::Only(ToProfile->Guidelines[Which].Class);
					ToMask.Add(ETraversalClass::Emergency);

					Turn.AllowedTraffic.Bits = static_cast<uint8>(FromMask.Bits & ToMask.Bits);
					Turn.Direction = EGuidelineDir::AToB;
					Turn.Width = FMath::Min(
						FromProfile->Guidelines[Which].Width,
						ToProfile->Guidelines[Which].Width);

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

	// --- Re-resolve hand-authored edges ------------------------------------------------
	//
	// A player's edge survived the clear pass, but the nodes it was drawn between are not
	// the nodes this derivation just made: AddGuidelineNode never deduplicates, so every
	// rebuild produces FRESH coincident nodes and the player's edge would keep pointing at
	// the old ones. It would still draw, and route nothing - breaking on a road edit that
	// had nothing to do with it.
	//
	// So an end that knows what it IS gets re-pointed at whatever now holds that identity.
	// Ends is the builder's own map, keyed exactly this way, and is simply no longer thrown
	// away.
	{
		auto Resolve = [&Ends, &EndKey](const FGuidelineEndRef& Ref, FGuidelineNodeId& Out) -> bool
		{
			if (!Ref.IsSet())
			{
				// No identity recorded - a link between two anchor nodes, whose handles are
				// stable already. Leave the end exactly where it is.
				return true;
			}

			const FGuidelineNodeId* Found =
				Ends.Find(EndKey(Ref.Segment.Index, Ref.bEndA, Ref.GuidelineIndex));
			if (Found == nullptr)
			{
				return false;
			}

			Out = *Found;
			return true;
		};

		TArray<FGuidelineEdgeId> Stranded;
		const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
		for (int32 Index = 0; Index < Edges.Num(); ++Index)
		{
			const FGuidelineEdge& Edge = Edges[Index];
			if (!Edge.bAlive || Edge.bDerived)
			{
				continue;
			}

			FGuidelineEdgeId Id;
			Id.Index = Index;
			Id.Generation = Edge.Generation;

			FGuidelineNodeId NewA = Edge.A;
			FGuidelineNodeId NewB = Edge.B;

			if (!Resolve(Edge.EndRefA, NewA) || !Resolve(Edge.EndRefB, NewB))
			{
				// The road under an end is gone. Kill the link rather than leave one
				// pointing at a road that no longer exists - a route across it would be a
				// route across nothing.
				Stranded.Add(Id);
				continue;
			}

			if (NewA != Edge.A || NewB != Edge.B)
			{
				Network.RelinkGuidelineEdge(Id, NewA, NewB);
			}
		}

		for (const FGuidelineEdgeId Id : Stranded)
		{
			Network.RemoveGuidelineEdge(Id);
		}
	}

	// LAST, for the reason given where this used to live: every detachment above has now
	// happened, so an idle derived node really is idle.
	{
		TArray<FGuidelineNodeId> Orphans;
		const TArray<FGuidelineNode>& Nodes = Network.GetGuidelineNodes();
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if (Nodes[Index].bAlive && Nodes[Index].bDerived && Nodes[Index].Incident.Num() == 0)
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
}
