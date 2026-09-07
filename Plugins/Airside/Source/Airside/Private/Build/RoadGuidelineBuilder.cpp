#include "Build/RoadGuidelineBuilder.h"

#include "AirsideLog.h"
#include "Build/ExitGeometry.h"
#include "Build/RoadMeshBuilder.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"

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

	// EXIT ARCS. A continuous arm (a runway) is never cut, so its guideline ended ON the
	// node - and a turn path whose control point is the node was then a straight line from
	// the taxiway's cut point into the centreline, meeting it at the taxiway's angle with an
	// instantaneous heading change (samples/runwayexits.png, 2026-09-06). The taxiway-to-
	// taxiway turn at the same node was a proper arc, because BOTH its ends sat back from the
	// node. So: at a MIXED node - at least one continuous arm and at least one that is not -
	// the continuous arm's guideline is split ExitLength before the node and the turns attach
	// there, and the non-continuous arm's guideline ends ExitLength back from the node. The
	// same control-at-the-node quadratic then comes out tangent at both ends. See the runway
	// exit arcs spec (docs/superpowers/specs/2026-09-06-runway-exit-arcs-design.md).
	//
	// Keyed by segment end with guideline index 0: every guideline of one end shares the
	// one set-back, which is a property of the junction, not of the lane.
	TMap<uint64, double> SetBack;
	TSet<uint64> ContinuousEnds;
	// Every non-continuous arm end at a mixed node, and the runway it meets: the
	// runway-holding positions to derive once the ends exist. Keyed like SetBack.
	TMap<uint64, FRoadSegmentId> ProtectedBy;
	// Where a continuous arm's TURNS attach at a mixed node (the split node), as opposed to
	// where its guideline ends (still the node, so the runway's own through-turn is intact).
	TMap<uint64, FGuidelineNodeId> Attach;

	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Pair.Key);
		if (ArmSegments == nullptr || !Pair.Value.bValid
			|| Pair.Value.Arms.Num() != ArmSegments->Num())
		{
			continue;
		}
		const FRoadNode* Node = Network.GetNodes().IsValidIndex(Pair.Key)
			? &Network.GetNodes()[Pair.Key] : nullptr;
		if (Node == nullptr || !Node->bAlive)
		{
			continue;
		}
		FRoadNodeId NodeId;
		NodeId.Index = Pair.Key;
		NodeId.Generation = Node->Generation;

		int32 ContinuousArms = 0;
		double ExitLength = 0.0;
		FRoadSegmentId RunwayHere;
		for (const FRoadSegmentId& ArmSeg : *ArmSegments)
		{
			const FRoadSegment* Arm = Network.GetSegment(ArmSeg);
			const URoadProfile* Profile = Arm ? Network.ProfileFor(*Arm) : nullptr;
			if (Profile != nullptr && Profile->bContinuousThroughJunctions)
			{
				++ContinuousArms;
				if (!RunwayHere.IsSet())
				{
					// Any member of the chain will do - RunwayChain expands it when read.
					RunwayHere = ArmSeg;
				}
				// The runway decides its exits. Two runways crossing a taxiway at one node
				// would disagree only by profile; the longer wins, which is the safer arc.
				ExitLength = FMath::Max(ExitLength, Profile->ExitLength);
			}
		}
		if (ContinuousArms == 0 || ContinuousArms == ArmSegments->Num())
		{
			continue;
		}

		// ONE LENGTH PER NODE, so every arc at it is SYMMETRIC: the same tangent length on
		// the runway side as on the taxiway side. An uneven quadratic - 60 m along the
		// runway, 30 m down a stub taxiway - bunches its curvature at the short end and
		// swings wide of the fillet on the way (samples/runway1.png, 2026-09-07). Decided in
		// ExitGeometry, the one place, because the junction solver sizes the flare fillet
		// from the same length and two copies of the rule would be two arcs.
		const double NodeLength = ExitGeometry::NodeExitLength(Network, Pair.Key, *ArmSegments);
		const FRoadSegment* RunwaySegment = Network.GetSegment(RunwayHere);
		const URoadProfile* RunwayProfile = RunwaySegment ? Network.ProfileFor(*RunwaySegment) : nullptr;
		const double RunwayHalfWidth = RunwayProfile ? RunwayProfile->GetTotalWidth() * 0.5 : 0.0;
		const FVector2D RunwayAxis = Network.GetOutgoingTangent(RunwayHere, NodeId);

		for (int32 ArmIndex = 0; ArmIndex < ArmSegments->Num(); ++ArmIndex)
		{
			const FRoadSegmentId ArmSeg = (*ArmSegments)[ArmIndex];
			const FRoadSegment* Arm = Network.GetSegment(ArmSeg);
			const URoadProfile* Profile = Arm ? Network.ProfileFor(*Arm) : nullptr;
			if (Arm == nullptr || Profile == nullptr)
			{
				continue;
			}
			const bool bContinuous = Profile->bContinuousThroughJunctions;
			const bool bEndA = (Arm->A == NodeId);
			if (!bContinuous)
			{
				ProtectedBy.Add(EndKey(ArmSeg.Index, bEndA, 0), RunwayHere);
			}
			if (NodeLength <= 0.0)
			{
				// Arcs off (a profile authored without an exit length): the ends stay at
				// their cut lines, and the holding positions recorded above are all this
				// node derives.
				continue;
			}

			double Length = NodeLength;
			if (!bContinuous)
			{
				// NEVER ON THE RUNWAY SLAB, whatever the clamp says: the end sits at least
				// where the taxiway's far edge clears the strip, so the holding position on
				// it is off the asphalt. The floor used to be the pavement CUT, and the first
				// clamp - measured on the chord between cuts - found nothing left of a 55 m
				// stub and put the end a metre short of the junction inside the slab
				// (samples/holdlines.png). The cut is no longer the measure because the
				// flare fillet now follows the arc and can push the cut far down a shallow
				// exit; the strip's own width is what the position must clear.
				const double TaxiwayHalfWidth = Profile->GetTotalWidth() * 0.5;
				const FVector2D Axis = Network.GetOutgoingTangent(ArmSeg, NodeId);
				const double AxisAngle = FMath::Acos(FMath::Clamp(FMath::Abs(FVector2D::DotProduct(Axis, RunwayAxis)), 0.0, 1.0));
				Length = FMath::Max(Length, ExitGeometry::TaxiwayEndFloor(RunwayHalfWidth, TaxiwayHalfWidth, AxisAngle));

				// AND NEVER INSIDE THE FLARE: at least the pavement cut, which is where the
				// ribbon begins at the taxiway's own width. Between the runway and that cut
				// the flare fillet (PR #58) widens the pavement, and a holding position there
				// is painted the taxiway's width across pavement that is wider - a bar that
				// stops short of the edge (reported 2026-09-07). The cut is a FLOOR here,
				// never the clamp it once was: a floor cannot put the end inside the slab,
				// which is what the first cut-based clamp did to a 55 m stub. A stub shorter
				// than its own cut is left at the slab floor and said so - its pavement is
				// already degenerate (the mesh builder refuses crossed cuts).
				const double Cut = bEndA ? Arm->TrimA : Arm->TrimB;
				const FRoadNode* Far = Network.GetNode(bEndA ? Arm->B : Arm->A);
				const FRoadNode* Near = Network.GetNode(NodeId);
				const double ArmLength = (Far && Near) ? FVector2D::Distance(Far->Position, Near->Position) : 0.0;
				if (Cut > Length && Cut < ArmLength - (bEndA ? Arm->TrimB : Arm->TrimA))
				{
					Length = Cut;
				}
				else if (Cut > Length)
				{
					UE_LOG(LogAirside, Warning,
						TEXT("Taxiway segment %d is shorter than its own cut at the runway (%.0f uu of %.0f): ")
						TEXT("its holding position stays at %.0f, inside the flare"),
						ArmSeg.Index, ArmLength, Cut, Length);
				}
			}
			if (Length <= 0.0)
			{
				continue;
			}
			const uint64 Key = EndKey(ArmSeg.Index, bEndA, 0);
			SetBack.Add(Key, Length);
			if (bContinuous)
			{
				ContinuousEnds.Add(Key);
			}
		}
	}

	// Splits a derived edge Length from one of its ends, exactly (de Casteljau; a runway's
	// derived guideline is a straight chord, so the pieces are straight too). Both pieces
	// keep the original's identity - DerivedFrom and index - which FindSparedEdge and the
	// route search already tolerate, because FAnchorLink::Build has split taxiways for
	// stand lead-ins this way since before there were exits. Returns the split node and
	// hands back the id of the piece on the FAR side, so the other end can be split next.
	auto SplitFromEnd = [&Network](FGuidelineEdgeId EdgeId, bool bFromA, double Length,
		FGuidelineEdgeId& OutRest) -> FGuidelineNodeId
	{
		OutRest = EdgeId;
		const FGuidelineEdge* Found = Network.GetGuidelineEdge(EdgeId);
		if (Found == nullptr)
		{
			return FGuidelineNodeId();
		}
		const FGuidelineEdge Original = *Found;
		const FVector2D PA = Network.GetGuidelineNode(Original.A)->Position;
		const FVector2D PB = Network.GetGuidelineNode(Original.B)->Position;
		const double Chord = FVector2D::Distance(PA, PB);
		if (Chord <= Length || Chord <= 0.0)
		{
			return FGuidelineNodeId();
		}
		const double T = bFromA ? Length / Chord : 1.0 - Length / Chord;

		FVector2D Mid, ControlLeft, ControlRight;
		GuidelineGeom::Split(PA, Original.Control, PB, T, Mid, ControlLeft, ControlRight);
		const FGuidelineNodeId Split = Network.AddGuidelineNode(Mid, /*bDerived=*/true);

		FGuidelineEdge Left = Original;
		Left.B = Split;
		Left.Control = ControlLeft;
		FGuidelineEdge Right = Original;
		Right.A = Split;
		Right.Control = ControlRight;

		Network.RemoveGuidelineEdge(EdgeId);
		const FGuidelineEdgeId LeftId = Network.AddGuidelineEdge(MoveTemp(Left));
		const FGuidelineEdgeId RightId = Network.AddGuidelineEdge(MoveTemp(Right));
		OutRest = bFromA ? RightId : LeftId;
		return Split;
	};

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

		// ProfileFor, NOT Segment.Profile - the THIRD reader to learn this. The solver and the
		// mesh builder were pinned to the accessor when a reloaded level came back invisible;
		// this builder was written afterwards and read the raw pointer, so the same reload
		// came back PAVED but not ROUTABLE: every taxiway drawn, no centreline under any of
		// them, every stand lead-in joining nothing, every arrival refused (M_Starter,
		// 2026-09-06). The mesh hid the loss, which is why it survived two milestones.
		const URoadProfile* Profile = Network.ProfileFor(Segment);
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
			FVector2D AtA =
				FRoadMeshBuilder::CutLinePoint(Segment.RightCutA, Segment.LeftCutA, Alpha);
			FVector2D AtB =
				FRoadMeshBuilder::CutLinePoint(Segment.LeftCutB, Segment.RightCutB, Alpha);

			// A taxiway meeting a runway ends where its exit arc begins - ExitLength back
			// from the node along its own tangent - not at its pavement cut. The end node
			// keeps its Origin, so a holding-position mark keyed by this end lands on the arc's
			// start. The continuous arm is NOT moved here: its guideline still ends on the
			// node and is split below instead, so the runway stays one line through.
			const uint64 KeyA = EndKey(Index, true, 0);
			const uint64 KeyB = EndKey(Index, false, 0);
			if (const double* Back = SetBack.Find(KeyA); Back && !ContinuousEnds.Contains(KeyA))
			{
				if (const FRoadNode* NodeA = Network.GetNode(Segment.A))
				{
					AtA = NodeA->Position
						+ Network.GetOutgoingTangent(SegmentId, Segment.A).GetSafeNormal() * (*Back);
				}
			}
			if (const double* Back = SetBack.Find(KeyB); Back && !ContinuousEnds.Contains(KeyB))
			{
				if (const FRoadNode* NodeB = Network.GetNode(Segment.B))
				{
					AtB = NodeB->Position
						+ Network.GetOutgoingTangent(SegmentId, Segment.B).GetSafeNormal() * (*Back);
				}
			}

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

			FGuidelineEdgeId EdgeId = Network.AddGuidelineEdge(MoveTemp(Edge));

			// The runway side of the arc: split this half ExitLength from each mixed end and
			// let the turns attach at the split. End A first, then B on whatever piece is
			// now adjacent to B - two exits on one half must not split the same piece twice.
			if (const double* Back = SetBack.Find(KeyA); Back && ContinuousEnds.Contains(KeyA))
			{
				FGuidelineEdgeId Rest;
				const FGuidelineNodeId Split = SplitFromEnd(EdgeId, /*bFromA=*/true, *Back, Rest);
				if (Split.IsSet())
				{
					Attach.Add(EndKey(Index, true, Which), Split);
					EdgeId = Rest;
				}
			}
			if (const double* Back = SetBack.Find(KeyB); Back && ContinuousEnds.Contains(KeyB))
			{
				FGuidelineEdgeId Rest;
				const FGuidelineNodeId Split = SplitFromEnd(EdgeId, /*bFromA=*/false, *Back, Rest);
				if (Split.IsSet())
				{
					Attach.Add(EndKey(Index, false, Which), Split);
				}
			}
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

				// Through the accessor for the same reason as the segment loop: a junction
				// between two reloaded taxiways skipped its turn paths entirely.
				const URoadProfile* FromProfile = Network.ProfileFor(*FromSegment);
				const URoadProfile* ToProfile   = Network.ProfileFor(*ToSegment);
				if (FromProfile == nullptr || ToProfile == nullptr)
				{
					continue;
				}

				const int32 Count = FMath::Min(
					FromProfile->Guidelines.Num(), ToProfile->Guidelines.Num());

				for (int32 Which = 0; Which < Count; ++Which)
				{
					// A turn between a continuous arm and one that is not attaches to the
					// continuous arm at its SPLIT node, ExitLength up the centreline, so the
					// quadratic through the node is an arc. Two continuous arms (the runway's
					// own halves) still meet at their node-ends: that is the zero-length
					// through-turn that keeps the runway one line.
					const bool bFromContinuous =
						ContinuousEnds.Contains(EndKey(FromSeg.Index, FromSegment->A == NodeId, 0));
					const bool bToContinuous =
						ContinuousEnds.Contains(EndKey(ToSeg.Index, ToSegment->A == NodeId, 0));
					const FGuidelineNodeId* FromEnd = nullptr;
					const FGuidelineNodeId* ToEnd = nullptr;
					if (bFromContinuous && !bToContinuous)
					{
						FromEnd = Attach.Find(EndKey(FromSeg.Index, FromSegment->A == NodeId, Which));
					}
					if (bToContinuous && !bFromContinuous)
					{
						ToEnd = Attach.Find(EndKey(ToSeg.Index, ToSegment->A == NodeId, Which));
					}
					if (FromEnd == nullptr)
					{
						FromEnd = Ends.Find(EndKey(FromSeg.Index, FromSegment->A == NodeId, Which));
					}
					if (ToEnd == nullptr)
					{
						ToEnd = Ends.Find(EndKey(ToSeg.Index, ToSegment->A == NodeId, Which));
					}
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

	// --- Re-apply holding-position marks ------------------------------------------------------
	//
	// The flag lives on a node and every derived node above is FRESH, so a bar the player
	// placed would vanish on the next road edit. The mark is stored by the same identity
	// a hand-authored edge stores its ends by, and resolved through the same Ends map -
	// one source (the mark), one cache (the flag), rebuilt together. Spec 2026-09-06 §6.
	{
		// The network owns this invariant, not the builder - it merely knows WHEN to ask.
		// Pruning first also means the loop below cannot re-apply a mark whose runway has
		// been deleted, which would put a bar on a node protecting nothing.
		Network.PruneHoldingPositionMarks();

		// CLEAR BEFORE DERIVING AND RE-APPLYING, because not every flagged node is fresh.
		// Most derived nodes are made anew above and start unflagged, but a node the sweep
		// SPARED, and every Origin-less node (an entity's pose or anchor - see
		// FGuidelineNode::Origin), lives on with whatever it last had.
		//
		// Two rules, because the two kinds of node have DIFFERENT sources of truth:
		//   - Origin set: the derivation (runway kind) or the MARK (intermediate kind) is
		//     the source and the node is its cache, so it is cleared and rewritten below -
		//     EXCEPT when this pass derived nothing for that end. Ends is fully populated by
		//     now, so a missing EndKey is the "unsolved end" the re-apply loop deliberately
		//     skips; clearing and then not rewriting would take the player's position away
		//     over a transient derivation failure. Leave the cache alone and let the next
		//     successful solve refresh it.
		//   - Origin unset: no mark is ever stored, so the NODE is the source. A runway kind
		//     there is cleared only when it names something that is no longer a live runway
		//     - the one case the prune cannot reach; an intermediate kind is left alone.
		{
			const TArray<FGuidelineNode>& Live = Network.GetGuidelineNodes();
			for (int32 Index = 0; Index < Live.Num(); ++Index)
			{
				if (!Live[Index].bAlive || Live[Index].HoldingPosition == EHoldingPositionKind::None)
				{
					continue;
				}
				const FGuidelineEndRef& Origin = Live[Index].Origin;
				bool bClear = false;
				if (Origin.IsSet())
				{
					bClear = Ends.Find(EndKey(Origin.Segment.Index, Origin.bEndA, Origin.GuidelineIndex)) != nullptr;
				}
				else
				{
					bClear = Live[Index].HoldingPosition == EHoldingPositionKind::Runway
						&& !Network.IsRunwaySegment(Live[Index].HoldingPositionFor);
				}
				if (!bClear)
				{
					continue;
				}
				if (FGuidelineNode* Node = Network.GetGuidelineNodeMutable(Network.GuidelineNodeIdAt(Index)))
				{
					Node->HoldingPosition = EHoldingPositionKind::None;
					Node->HoldingPositionFor = FRoadSegmentId();
				}
			}
		}

		// RUNWAY-HOLDING POSITIONS ARE DERIVED: every taxiway end at a runway, on every
		// guideline of that end, protecting the strip it meets. Infrastructure, not a
		// choice - a real one is painted at every such junction whether ATC ever says
		// "hold short" there or not (spec 2026-09-07). Independent of ExitLength: with the
		// arcs off the end sits at its cut line and is a holding position all the same.
		for (const TPair<uint64, FRoadSegmentId>& Pair : ProtectedBy)
		{
			const int32 SegmentIndex = static_cast<int32>(Pair.Key >> 32);
			const bool bEndA = (Pair.Key & 1ull) != 0;
			const FRoadSegment* Arm = Segments.IsValidIndex(SegmentIndex) ? &Segments[SegmentIndex] : nullptr;
			const URoadProfile* Profile = Arm ? Network.ProfileFor(*Arm) : nullptr;
			if (Profile == nullptr)
			{
				continue;
			}
			for (int32 Which = 0; Which < Profile->Guidelines.Num(); ++Which)
			{
				const FGuidelineNodeId* End = Ends.Find(EndKey(SegmentIndex, bEndA, Which));
				FGuidelineNode* Node = End ? Network.GetGuidelineNodeMutable(*End) : nullptr;
				if (Node == nullptr)
				{
					continue;
				}
				Node->HoldingPosition = EHoldingPositionKind::Runway;
				Node->HoldingPositionFor = Pair.Value;
			}
		}

		// INTERMEDIATE positions are the player's, re-applied from their marks.
		for (const FHoldingPositionMark& Mark : Network.GetHoldingPositionMarks())
		{
			const FGuidelineNodeId* Found = Ends.Find(
				EndKey(Mark.At.Segment.Index, Mark.At.bEndA, Mark.At.GuidelineIndex));
			if (Found == nullptr)
			{
				// The segment is alive (prune said so) but derived nothing this pass - an
				// unsolved end, or a profile that lost the guideline the mark named. Leave
				// the mark: the next successful solve puts the position back, which is
				// kinder than deleting a player's work over a transient derivation failure.
				//
				// The clear above skips this same case for the same reason - the two tests
				// are the same lookup in the same map, so a flag is never cleared here only
				// to be left unwritten there.
				continue;
			}
			FGuidelineNode* Node = Network.GetGuidelineNodeMutable(*Found);
			// A mark on an end that has since become a runway end is out-ranked by the
			// derivation: the junction decides, and the stale mark is harmless.
			if (Node != nullptr && Node->HoldingPosition != EHoldingPositionKind::Runway)
			{
				Node->HoldingPosition = EHoldingPositionKind::Intermediate;
				Node->HoldingPositionFor = FRoadSegmentId();
			}
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

	// THE CENSUS. This builder logged nothing for the whole of its life, and the first
	// "none of the routes are there in PIE" report (2026-09-06) could not be read off the
	// log at all: the mesh census said 16 segments and the traffic said "no route to a
	// stand", and everything between the two was a guess. One line per build, so the next
	// such report is answered by a grep.
	{
		int32 NodesAlive = 0, HoldingPosition = 0, EdgesAlive = 0, Authored = 0, TurnPaths = 0;
		for (const FGuidelineNode& Node : Network.GetGuidelineNodes())
		{
			NodesAlive += Node.bAlive ? 1 : 0;
			HoldingPosition += (Node.bAlive && Node.HoldingPosition != EHoldingPositionKind::None) ? 1 : 0;
		}
		for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
		{
			if (!Edge.bAlive) { continue; }
			++EdgesAlive;
			Authored += Edge.bDerived ? 0 : 1;
			TurnPaths += (Edge.bDerived && !Edge.DerivedFrom.IsSet()) ? 1 : 0;
		}
		UE_LOG(LogAirside, Log,
			TEXT("Guidelines: %d nodes (%d holding-position), %d edges (%d hand-authored, %d turn paths), %d holding-position mark(s) on file"),
			NodesAlive, HoldingPosition, EdgesAlive, Authored, TurnPaths, Network.GetHoldingPositionMarks().Num());
	}
}
