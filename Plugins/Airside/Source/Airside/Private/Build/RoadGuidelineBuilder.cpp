#include "Build/RoadGuidelineBuilder.h"

#include "AirsideLog.h"
#include "Build/ExitGeometry.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Model/Chassis.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"
#include "Solve/UTurnGeom.h"

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

	/**
	 * The pavement at one junction: its polygon and the ribbons of the arms that meet there.
	 * A vehicle's swept path may use any of it - a truck swinging wide into the other lane, or
	 * across the junction's fillet, is on tarmac - and nothing outside it.
	 */
	struct FJunctionPavement
	{
		TArray<FVector2D> Polygon;
		TArray<TArray<FVector2D>> Ribbons;

		bool Contains(const FVector2D& Point) const
		{
			if (Polygon.Num() >= 3 && RoadGeom::PointInPolygon(Polygon, Point))
			{
				return true;
			}
			for (const TArray<FVector2D>& Ribbon : Ribbons)
			{
				if (RoadGeom::PointInPolygon(Ribbon, Point))
				{
					return true;
				}
			}
			return false;
		}
	};

	/** 10 uu steps to 30 m: finer than any lane, further than any vehicle sweeps. */
	constexpr double ClearanceStep = 10.0;
	constexpr double ClearanceCap = 3000.0;

	/**
	 * Measures what a turn offers a vehicle's body (spec 2026-09-23 §6) on GuidelineGeom::Sample,
	 * so route search judges the line that is driven, not a second evaluation of it. MinRadius
	 * is GuidelineGeom::TightestRadius, the figure the lock warning below uses.
	 * ENFORCED BY: Airside.Build.MeasuredOnFollowerSamples
	 * Clearances march along each sample's normal until the point leaves the pavement.
	 */
	void MeasureTurn(FGuidelineEdge& Turn, const FVector2D& PA, const FVector2D& PB, const FJunctionPavement& Pavement)
	{
		Turn.MinRadius = GuidelineGeom::TightestRadius(PA, Turn.Control, PB);
		const double Cross = FVector2D::CrossProduct(Turn.Control - PA, PB - Turn.Control);
		if (FMath::IsNearlyZero(Cross))
		{
			// Straight through: no inside to a straight line; the lane width gates it.
			Turn.MinRadius = 0.0;
			return;
		}
		TArray<FVector2D> Points;
		GuidelineGeom::Sample(PA, Turn.Control, PB, Points);
		// EVERY SAMPLE, recorded per sample (2026-09-24). A first cut kept only the minimum over
		// the middle third, because a single minimum over all samples was always the in-lane
		// ends (280 uu at every Wide fillet) and the steady-state envelope it was compared with
		// only applies at the apex. VehicleFit now simulates the turn and compares sample by
		// sample, so the ends are judged by what reaches them - usually nothing wide.
		Turn.ClearInnerAt.SetNum(Points.Num());
		Turn.ClearOuterAt.SetNum(Points.Num());
		double Inner = ClearanceCap;
		double Outer = ClearanceCap;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			const FVector2D Tangent = (Points[FMath::Min(Index + 1, Points.Num() - 1)]
				- Points[FMath::Max(Index - 1, 0)]).GetSafeNormal();
			// The centre of a counter-clockwise (positive cross) curve is on its PerpCCW side.
			const FVector2D Inward = RoadGeom::PerpCCW(Tangent) * (Cross > 0.0 ? 1.0 : -1.0);
			auto March = [&](const FVector2D& Dir)
			{
				double D = 0.0;
				while (D < ClearanceCap && Pavement.Contains(Points[Index] + Dir * (D + ClearanceStep)))
				{
					D += ClearanceStep;
				}
				return D;
			};
			const double In = March(Inward);
			const double Out = March(-Inward);
			Turn.ClearInnerAt[Index] = static_cast<float>(In);
			Turn.ClearOuterAt[Index] = static_cast<float>(Out);
			Inner = FMath::Min(Inner, In);
			Outer = FMath::Min(Outer, Out);
		}
		Turn.ClearInner = Inner;
		Turn.ClearOuter = Outer;
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
				// Network.GuidelineEdgeIdAt, not a hand-built handle (#79, #173).
				return Network.GuidelineEdgeIdAt(Index);
			}
		}
		return FGuidelineEdgeId();
	}
}

void FRoadGuidelineBuilder::Build(URoadNetwork& Network, const FRoadSolveResult& Solved,
	const FRoadDesignVehicles& DesignVehicles)
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
				Doomed.Add(Network.GuidelineEdgeIdAt(Index));
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
		const FRoadNodeId NodeId = Network.NodeIdAt(Pair.Key);

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
				// Acute, folded from AngleBetween rather than acos(|dot|) - Check-Architecture rule 18.
				const double AxisCrossing = RoadGeom::AngleBetween(Axis, RunwayAxis);
				const double AxisAngle = FMath::Min(AxisCrossing, UE_DOUBLE_PI - AxisCrossing);
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
		const FVector2D PA = Network.GetGuidelineNode(Found->A)->Position;
		const FVector2D PB = Network.GetGuidelineNode(Found->B)->Position;
		const double Chord = FVector2D::Distance(PA, PB);
		if (Chord <= Length || Chord <= 0.0)
		{
			return FGuidelineNodeId();
		}
		const double T = bFromA ? Length / Chord : 1.0 - Length / Chord;

		// T is strictly inside (0,1) - Chord > Length was just checked - so this is never a
		// snap to an existing endpoint; WeldTolerance is 0 rather than the LeadInWeldTolerance
		// AnchorLink and StandLaneBuild use for their own, unrelated proximity joins.
		FGuidelineNodeId Split;
		FGuidelineEdgeId Head, Tail;
		if (!Network.SplitGuidelineEdge(EdgeId, T, /*WeldTolerance=*/0.0, Split, Head, Tail))
		{
			return FGuidelineNodeId();
		}
		OutRest = bFromA ? Tail : Head;
		return Split;
	};

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || !Segment.bSolvedA || !Segment.bSolvedB)
		{
			continue;
		}

		const FRoadSegmentId SegmentId = Network.SegmentIdAt(Index);

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
			// A SPARED LANE IGNORES THE DRIVE SIDE (review of 2026-09-23, left as is): the
			// player's hand-edited edge keeps the geometry they gave it, so after a flip the other
			// lane is derived onto the positions it still occupies - two coincident lanes, running
			// opposite ways. Anything that lets a player edit a road LANE in place must re-apply
			// the drive side to it; connectors drawn between lanes re-resolve by identity and
			// survive a flip (Airside.Build.TwoWay.SparedEdgeSurvivesFlip).
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
			// THE DRIVE SIDE IS APPLIED HERE AND NOWHERE ELSE (spec 2026-09-23 §2). Profiles
			// are authored for right-hand traffic and OffsetFor mirrors them; Direction stays
			// tied to A/B, so the lane that ran A->B on the right now runs A->B on the left.
			// ENFORCED BY: Airside.Build.TwoWay.RouteKeepsSide
			const double Alpha = AlphaForOffset(Profile, Declared.OffsetFor(Network.GetDriveSide()));

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
			// survive a rebuild; this does. One whole FGuidelineEndRef per node through
			// SetGuidelineNodeOrigin (#191), not three field writes through a raw pointer -
			// a caller stopping between them used to be able to leave GuidelineIndex from
			// the slot's previous life sitting beside a fresh Segment/bEndA.
			FGuidelineEndRef OriginA;
			OriginA.Segment = SegmentId;
			OriginA.bEndA = true;
			OriginA.GuidelineIndex = Which;
			Network.SetGuidelineNodeOrigin(Edge.A, OriginA);

			FGuidelineEndRef OriginB;
			OriginB.Segment = SegmentId;
			OriginB.bEndA = false;
			OriginB.GuidelineIndex = Which;
			Network.SetGuidelineNodeOrigin(Edge.B, OriginB);
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
	int32 Balloons = 0;
	int32 Tapers = 0;
	int32 BendArcs = 0;
	for (const TPair<int32, FJunctionResult>& Pair : Solved.NodeResults)
	{
		const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(Pair.Key);
		if (ArmSegments == nullptr || !Pair.Value.bValid)
		{
			continue;
		}

		const FRoadNode* Node = Network.GetNodes().IsValidIndex(Pair.Key)
			? &Network.GetNodes()[Pair.Key] : nullptr;
		if (Node == nullptr || !Node->bAlive)
		{
			continue;
		}
		// Network.NodeIdAt, not a hand-built handle (#79, #173).
		const FRoadNodeId NodeId = Network.NodeIdAt(Pair.Key);

		// The tarmac a turn here may sweep: the junction polygon (its boundary minus the fan
		// centre SolveBoundary appends) and every arm's ribbon, from its four cut points. End
		// B's cut is authored from B's side, so its "left" is the A->B right - the quad still
		// closes, and PointInPolygon takes either winding.
		FJunctionPavement Pavement;
		if (Pair.Value.Boundary.Num() > 3)
		{
			Pavement.Polygon = Pair.Value.Boundary;
			Pavement.Polygon.Pop();
		}
		for (const FRoadSegmentId& ArmSeg : *ArmSegments)
		{
			if (const FRoadSegment* Arm = Network.GetSegment(ArmSeg))
			{
				Pavement.Ribbons.Add({ Arm->LeftCutA, Arm->RightCutA, Arm->LeftCutB, Arm->RightCutB });
			}
		}

		// A DEAD END TURNS VEHICLES ROUND (spec 2026-09-23 §4, ruled: an edge, no mesh). One-way
		// lanes would otherwise strand anything that drove into a stub. A bidirectional arm (a
		// taxiway, or a one-lane road) is skipped: its one line already runs both ways, and a
		// balloon there would be a change nobody asked for.
		//
		// SIZED FOR THE LARGEST RIGID SERVICE VEHICLE ON EVERY TIER - DesignVehicles.Default, NOT
		// the tier's own design vehicle the fillets use. By user ruling (2026-09-25, "smaller,
		// reverse later"): a balloon the rig can be driven round without folding its trailer
		// reaches ~24 m past the road end (measured), and the rig will turn at a road end with a
		// three-point turn once reversing exists (step 2). Until then it is refused at every dead
		// end on its lock. Laid over grass: the balloon reaches ~4x the lock radius past the road
		// end (UTurnGeom.h).
		// ENFORCED BY: Airside.Build.DesignVehicle.WideDeadEndRefusesRigUntilReversing
		// ENFORCED BY: Airside.Build.TwoWay.DeadEnd, Airside.Solve.UTurnBalloon
		if (ArmSegments->Num() == 1)
		{
			const FRoadSegmentId ArmSeg = (*ArmSegments)[0];
			const FRoadSegment* Arm = Network.GetSegment(ArmSeg);
			const URoadProfile* Profile = Arm ? Network.ProfileFor(*Arm) : nullptr;
			if (Arm == nullptr || Profile == nullptr)
			{
				continue;
			}
			const bool bAtA = (Arm->A == NodeId);
			int32 InWhich = INDEX_NONE;
			int32 OutWhich = INDEX_NONE;
			for (int32 Which = 0; Which < Profile->Guidelines.Num(); ++Which)
			{
				const EGuidelineDir Dir = Profile->Guidelines[Which].Direction;
				// The same arrive/leave reading the turn loop below applies per arm.
				if ((bAtA && Dir == EGuidelineDir::BToA) || (!bAtA && Dir == EGuidelineDir::AToB))
				{
					InWhich = Which;
				}
				if ((bAtA && Dir == EGuidelineDir::AToB) || (!bAtA && Dir == EGuidelineDir::BToA))
				{
					OutWhich = Which;
				}
			}
			const FGuidelineNodeId* InEnd = InWhich != INDEX_NONE ? Ends.Find(EndKey(ArmSeg.Index, bAtA, InWhich)) : nullptr;
			const FGuidelineNodeId* OutEnd = OutWhich != INDEX_NONE ? Ends.Find(EndKey(ArmSeg.Index, bAtA, OutWhich)) : nullptr;
			if (InEnd == nullptr || OutEnd == nullptr)
			{
				continue;
			}

			const FVector2D InAt = Network.GetGuidelineNode(*InEnd)->Position;
			const FVector2D OutAt = Network.GetGuidelineNode(*OutEnd)->Position;
			// Out of the road, past the dead end.
			const FVector2D Axis = -Network.GetOutgoingTangent(ArmSeg, NodeId).GetSafeNormal();
			const TArray<UTurnGeom::FPiece> Pieces =
				UTurnGeom::Balloon(InAt, OutAt, Axis, DesignVehicles.Default.Chassis.TightestFollowableRadius());
			if (Pieces.Num() == 0)
			{
				UE_LOG(LogAirside, Warning, TEXT("Dead end at (%.0f,%.0f): no U-turn laid - its lane ends coincide"),
					Node->Position.X, Node->Position.Y);
				continue;
			}

			const FProfileGuideline& InLine = Profile->Guidelines[InWhich];
			const FProfileGuideline& OutLine = Profile->Guidelines[OutWhich];
			FTrafficMask Mask = FTrafficMask::Only(InLine.Class);
			Mask.Add(OutLine.Class);
			Mask.Add(ETraversalClass::Emergency);

			FGuidelineNodeId Prev = *InEnd;
			for (int32 Index = 0; Index < Pieces.Num(); ++Index)
			{
				const bool bLast = Index == Pieces.Num() - 1;
				const FGuidelineNodeId Next = bLast ? *OutEnd : Network.AddGuidelineNode(Pieces[Index].End);
				FGuidelineEdge Loop;
				Loop.A = Prev;
				Loop.B = Next;
				Loop.Control = Pieces[Index].Control;
				Loop.AllowedTraffic = Mask;
				Loop.Direction = EGuidelineDir::AToB;
				Loop.Width = FMath::Min(InLine.Width, OutLine.Width);
				// The LOCK is checked on a balloon; its clearances stay unmeasured (-1), because
				// it lies over grass by ruling and has no pavement edge to measure to.
				Loop.MinRadius = GuidelineGeom::TightestRadius(
					Network.GetGuidelineNode(Prev)->Position, Loop.Control, Pieces[Index].End);
				Loop.bDerived = true;
				// DerivedFrom stays unset, like a turn path: the balloon belongs to the node.
				Network.AddGuidelineEdge(MoveTemp(Loop));
				Prev = Next;
			}
			++Balloons;
			continue;
		}

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

				// EVERY ARRIVING LANE TO EVERY LEAVING LANE, subject to class. Until 2026-09-23
				// this paired guideline N with guideline N over the smaller count - right while
				// every road was one centreline, wrong the day a road had two lanes: offsets are
				// relative to each segment's own A->B, arms meet at mixed ends, so index 0 is the
				// arriving lane on one arm and the leaving lane on the next, and a one-lane arm
				// meeting a two-lane arm lost a lane outright. With two one-way lanes per arm
				// this emits exactly one turn per arm pair; a bidirectional taxiway still emits
				// one, because its single guideline both arrives and leaves.
				// ENFORCED BY: Airside.Build.TwoWay.TJunctionTurns, .MixedLaneCounts; Check-Architecture rule 16
				for (int32 FromWhich = 0; FromWhich < FromProfile->Guidelines.Num(); ++FromWhich)
				for (int32 ToWhich = 0; ToWhich < ToProfile->Guidelines.Num(); ++ToWhich)
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
						FromEnd = Attach.Find(EndKey(FromSeg.Index, FromSegment->A == NodeId, FromWhich));
					}
					if (bToContinuous && !bFromContinuous)
					{
						ToEnd = Attach.Find(EndKey(ToSeg.Index, ToSegment->A == NodeId, ToWhich));
					}
					if (FromEnd == nullptr)
					{
						FromEnd = Ends.Find(EndKey(FromSeg.Index, FromSegment->A == NodeId, FromWhich));
					}
					if (ToEnd == nullptr)
					{
						ToEnd = Ends.Find(EndKey(ToSeg.Index, ToSegment->A == NodeId, ToWhich));
					}
					if (FromEnd == nullptr || ToEnd == nullptr)
					{
						continue;
					}

					// Arriving at this node along the From arm, then leaving along the To
					// arm. Either arm may be one-way, and a turn that ignores that lands an
					// agent on a node it cannot leave.
					// FProfileGuideline's one reading of Direction at a node, which the solver's bend
					// widening traces the same turns by.
					const bool bMayArrive = FromProfile->Guidelines[FromWhich].ArrivesAt(FromSegment->A == NodeId);
					const bool bMayLeave = ToProfile->Guidelines[ToWhich].LeavesFrom(ToSegment->A == NodeId);

					if (!bMayArrive || !bMayLeave)
					{
						continue;
					}

					const FProfileGuideline& Declared = FromProfile->Guidelines[FromWhich];
					const FProfileGuideline& ToDeclared = ToProfile->Guidelines[ToWhich];

					FGuidelineEdge Turn;
					Turn.A = *FromEnd;
					Turn.B = *ToEnd;

					// Both arms' tangent lines meet AT the node, so the single control
					// point they define is the node itself - which is precisely the
					// quadratic case, and why this is not the parent spec's cubic.
					//
					// FOR CENTRELINE GUIDELINES ONLY (2026-09-23). An offset lane's tangent
					// line misses the node, and a node-controlled quadratic would leave and
					// join the lanes at an angle. So the control is where the two LANE lines
					// cross - and exactly the node, bitwise, when both are centred, so no
					// taxiway turn moves (Airside.Build.TwoWay.TaxiwayControlUnchanged).
					Turn.Control = Node->Position;
					const double FromOffset = Declared.OffsetFor(Network.GetDriveSide());
					const double ToOffset = ToDeclared.OffsetFor(Network.GetDriveSide());
					if (FromOffset != 0.0 || ToOffset != 0.0)
					{
						const FVector2D PA = Network.GetGuidelineNode(Turn.A)->Position;
						const FVector2D PB = Network.GetGuidelineNode(Turn.B)->Position;
						FRay2D Arrive;
						Arrive.Origin = PA;
						Arrive.Dir = -Network.GetOutgoingTangent(FromSeg, NodeId).GetSafeNormal();
						FRay2D Leave;
						Leave.Origin = PB;
						Leave.Dir = Network.GetOutgoingTangent(ToSeg, NodeId).GetSafeNormal();
						FVector2D Crossing;
						const bool bAhead = RoadGeom::LineIntersect(Arrive, Leave, Crossing)
							&& FVector2D::DotProduct(Crossing - PA, Arrive.Dir) > 0.0
							&& FVector2D::DotProduct(PB - Crossing, Leave.Dir) > 0.0;
						// Straight through (parallel lanes), or a crossing behind an end: the
						// chord midpoint, a straight line that never loops back.
						Turn.Control = bAhead ? Crossing : (PA + PB) * 0.5;
					}

					// A BEND'S LANES FOLLOW ITS PAVEMENT (user ruling 2026-09-25). At a two-arm bend the
					// quadratic above runs cut to cut with its control on the lane lines' crossing -
					// one quadratic across the corner's whole sweep, which delivers only cos(sweep/2) of
					// the arc it approximates (0.707 at a right angle: 726 uu on a Narrow inner lane
					// whose arc is 1026) and bulges off it by 62 uu at the apex. The cut is the inner
					// fillet's tangent point (or, where the solver widened the inside, further out along
					// the arm), so each lane line touches a circle about that fillet's centre at the
					// lane's own distance from the inner edge - the lane concentric with the pavement's
					// inside. So the turn is that arc, laid in pieces (GuidelineGeom::BendLane), each
					// sampled once like any edge.
					//
					// CONCENTRIC WITH THE INNER EDGE, NOT THE OUTER, and not by preference: the two
					// fillets have the SAME radius, so their centres sit a road width apart on each axis
					// and no circle is concentric with both. About the outer fillet's centre the lanes
					// would use the outside of the bend - and turn at 306 / 606 uu on Narrow, 186 / 636
					// on Wide, below both design vehicles' locks, where they turn at 726 / 938 and
					// 853 / 1171 today (measured 2026-09-25). The ruling forbids a tighter turn.
					// SINCE 2026-09-25 THE OUTER EDGE FOLLOWS: the solver lays the outside as the arc about
					// this same centre at the inner radius plus the road width (SmoothBend,
					// RoadNetworkSolver.cpp), so both edges and every lane now share the one centre.
					//
					// ONLY WHERE THE CONSTRUCTION HOLDS: two arms, a service-road lane on both (a
					// taxiway's corner is authored for aircraft - PreferredFilletRadius - and stays as it
					// was), and both lane ends on the same circle and at its tangent points. Arms of two
					// widths put their lanes at different distances from the inner edge, so no one
					// circle touches both: those RAMP onto the wider arm's circle (below). 33d6f49b kept
					// the one quadratic there - "only one node around the corner", the user's variant (a).
					// ENFORCED BY: Airside.Build.BendLanes.ConcentricWithPavement, .NeverTighter, .EveryTierIsSmooth
					TArray<GuidelineGeom::FArcPiece> BendArc;
					if (ArmSegments->Num() == 2 && Pair.Value.Corners.Num() == 2
						&& Declared.Class == ETraversalClass::GroundVehicle && ToDeclared.Class == ETraversalClass::GroundVehicle)
					{
						const RoadGeom::FFillet* Inside = nullptr;
						for (const RoadGeom::FFillet& Corner : Pair.Value.Corners)
						{
							if (Corner.bValid && !Corner.bStraightThrough && Corner.Theta < UE_DOUBLE_PI && Corner.Radius > 0.0)
							{
								Inside = &Corner;
							}
						}
						if (Inside != nullptr)
						{
							const FVector2D PA = Network.GetGuidelineNode(Turn.A)->Position;
							const FVector2D PB = Network.GetGuidelineNode(Turn.B)->Position;
							const FVector2D ArriveDir = -Network.GetOutgoingTangent(FromSeg, NodeId).GetSafeNormal();
							const FVector2D LeaveDir = Network.GetOutgoingTangent(ToSeg, NodeId).GetSafeNormal();
							// GuidelineGeom::BendLane refuses what the construction cannot hold: lanes at
							// two distances from the edge, a tangent point behind a lane end, a turn not
							// round this corner. A widened bend's lane ends sit back from their tangent
							// points (the solver cut the arms back to hold the widening) and are joined
							// to the arc by straight leads.
							GuidelineGeom::BendLane(PA, ArriveDir, PB, LeaveDir, Inside->Centre, BendArc);
							// A WIDTH STEP RUNS AT THE WIDER WIDTH (2026-09-25): the narrower arm's lane end is
							// off the circle the wider arm's touches, so it ramps onto it along the bend's own
							// ramp clock - the one the solver laid both edges on (FBendOuter::Ramp), so the lanes
							// stay evenly spaced between them through the taper (GuidelineGeom::RampedBendLane).
							const double FromWidth = FromProfile->GetTotalWidth();
							const double ToWidth = ToProfile->GetTotalWidth();
							const FBendOuter* Bend = Solved.BendOuters.FindByPredicate(
								[&Pair](const FBendOuter& Candidate) { return Candidate.NodeIndex == Pair.Key && Candidate.bApplied; });
							const int32 FromArm = ArmSegments->IndexOfByKey(FromSeg);
							const int32 ToArm = ArmSegments->IndexOfByKey(ToSeg);
							if (BendArc.Num() == 0 && FromWidth != ToWidth && Bend != nullptr && FromArm != INDEX_NONE && ToArm != INDEX_NONE)
							{
								// The lanes' circle: the wider arm's lane line's distance from the centre.
								const FVector2D Wide = FromWidth > ToWidth ? PA : PB;
								const FVector2D WideDir = FromWidth > ToWidth ? ArriveDir : LeaveDir;
								GuidelineGeom::FRampedBend Ramped;
								Ramped.Centre = Inside->Centre;
								Ramped.Radius = FMath::Abs(FVector2D::CrossProduct(Inside->Centre - Wide, WideDir));
								Ramped.RefRadius = Bend->RefRadius;
								Ramped.LIn = Bend->Ramp[FromArm];
								Ramped.LOut = Bend->Ramp[ToArm];
								GuidelineGeom::RampedBendLane(PA, ArriveDir, PB, LeaveDir, Ramped, BendArc);
							}
						}
					}

					// A WIDTH TAPER IS DRIVEN ON AN S (user ruling 2026-09-25). At a straight-through
					// node of two arms whose lanes sit at different offsets, the solver has inset
					// both cuts (FRoadNetworkSolver's WidthTaperLength) and the lane ends face each
					// other across the taper, parallel and a lane-offset step apart. One quadratic
					// cannot join two parallel lines - the chord above was a straight diagonal with a
					// kink at each end, and with the cuts at the node it was a 90 degree jog. So the
					// turn is TWO edges meeting at the midpoint, GuidelineGeom's lane change: each a
					// symmetric quadratic of tangent s deflecting by b, with tan(b/2) = across/along
					// and 2 s sin b = across, so the pair runs exactly from end to end and is tangent
					// to both lanes and to each other. Each is sampled once by GuidelineGeom like any
					// edge, and measured like any turn - its MinRadius is what route search gates on.
					// Only at a TWO-arm node: a through road's width change inside a larger junction
					// keeps the chord, and is not this ruling's.
					// THE TRIGGER IS "THE LANES ARE OFFSET", NOT "THE WIDTHS DIFFER" - intended: what
					// the S removes is a lateral step in the LINE, whatever made it. So a same-width
					// profile change whose lanes sit at other offsets tapers too, and so does a
					// one-lane bidirectional road meeting a two-lane one (its centreline steps a
					// quarter-width onto each lane).
					// ENFORCED BY: Airside.Build.WidthTaper.SameWidthOffsetLanes, Airside.Build.WidthTaper.OneLaneMeetsTwo
					// ENFORCED BY: Airside.Build.WidthTaper.Drivable, Airside.Build.WidthTaper.LanesContinuous
					bool bTaperS = false;
					FVector2D TaperMid = FVector2D::ZeroVector;
					FVector2D TaperControlOut = FVector2D::ZeroVector;
					FVector2D TaperFrom = FVector2D::ZeroVector;
					FVector2D TaperTo = FVector2D::ZeroVector;
					FVector2D TaperTravel = FVector2D::ZeroVector;
					if (ArmSegments->Num() == 2
						&& RoadGeom::IsStraightThrough(RoadGeom::AngleBetween(
							Network.GetOutgoingTangent(FromSeg, NodeId), Network.GetOutgoingTangent(ToSeg, NodeId))))
					{
						// GuidelineGeom's ONE lane-change construction, the same the solver sized
						// the inset with (LaneChangeLength). Along <= 1 uu (a taper capped to
						// nothing) leaves the chord - see the spec's Open list.
						const FVector2D Travel = -Network.GetOutgoingTangent(FromSeg, NodeId).GetSafeNormal();
						FVector2D ControlIn, Mid, ControlOut;
						if (GuidelineGeom::LaneChange(Network.GetGuidelineNode(Turn.A)->Position,
							Network.GetGuidelineNode(Turn.B)->Position, Travel, ControlIn, Mid, ControlOut))
						{
							TaperMid = Mid;
							TaperFrom = Network.GetGuidelineNode(Turn.A)->Position;
							TaperTo = Network.GetGuidelineNode(Turn.B)->Position;
							TaperTravel = Travel;
							Turn.Control = ControlIn;
							TaperControlOut = ControlOut;
							bTaperS = true;
						}
					}

					// A turn is usable only by what BOTH arms admit - the same reasoning
					// already applied to MaxWingspan below, which this previously
					// contradicted one line up. Where the two arms carry different classes
					// the intersection leaves Emergency alone, which is right: a fire truck
					// may cross between a service road and a taxiway and nothing else may.
					FTrafficMask FromMask = FTrafficMask::Only(Declared.Class);
					FromMask.Add(ETraversalClass::Emergency);
					FTrafficMask ToMask = FTrafficMask::Only(ToDeclared.Class);
					ToMask.Add(ETraversalClass::Emergency);

					Turn.AllowedTraffic.Bits = static_cast<uint8>(FromMask.Bits & ToMask.Bits);
					Turn.Direction = EGuidelineDir::AToB;
					Turn.Width = FMath::Min(
						Declared.Width,
						ToDeclared.Width);

					// 0 means UNLIMITED, so a naive Min would let an unlimited arm widen a
					// limited one - wrong in the direction that puts an oversized aircraft
					// onto a turn that cannot take it. A turn is usable only by what BOTH
					// arms admit.
					const double FromLimit = Declared.MaxWingspan;
					const double ToLimit   = ToDeclared.MaxWingspan;
					Turn.MaxWingspan =
						(FromLimit <= 0.0) ? ToLimit :
						(ToLimit   <= 0.0) ? FromLimit :
						FMath::Min(FromLimit, ToLimit);
					// THE S IS TWO PIECES meeting at a node of their own, and a bend's arc one per
					// BendArcPieceSweep of its turn; everything below - the measure, the lock warning,
					// the add - is done to each piece alike.
					TArray<FGuidelineEdge, TInlineAllocator<4>> Pieces;
					if (BendArc.Num() > 0)
					{
						// THE BEND'S ARC, piece by piece; its last End is Turn.B's position verbatim,
						// and the last piece joins that node BY HANDLE.
						const FGuidelineNodeId Last = Turn.B;
						FGuidelineNodeId Prev = Turn.A;
						for (int32 Index = 0; Index < BendArc.Num(); ++Index)
						{
							FGuidelineEdge Piece = Turn;
							Piece.A = Prev;
							Piece.B = Index + 1 == BendArc.Num() ? Last : Network.AddGuidelineNode(BendArc[Index].End);
							Piece.Control = BendArc[Index].Control;
							Pieces.Add(Piece);
							Prev = Piece.B;
						}
						++BendArcs;
					}
					else if (bTaperS)
					{
						const FGuidelineNodeId Mid = Network.AddGuidelineNode(TaperMid);
						FGuidelineEdge Second = Turn;
						Turn.B = Mid;
						Second.A = Mid;
						Second.Control = TaperControlOut;
						Pieces.Add(Turn);
						Pieces.Add(Second);
						++Tapers;
					}
					else
					{
						Pieces.Add(Turn);
					}
					for (FGuidelineEdge& Piece : Pieces)
					{
						// ENFORCED BY: Airside.Build.TurnClearance
						MeasureTurn(Piece, Network.GetGuidelineNode(Piece.A)->Position,
							Network.GetGuidelineNode(Piece.B)->Position, Pavement);
						Piece.bDerived = true;
						// DerivedFrom stays unset: a turn path belongs to the junction, not to
						// either segment, and that is how the two are told apart.

						// WHAT THIS CORNER ACTUALLY HANDS A DRIVER, measured on the curve that is
						// about to be added rather than inferred from the fillet that shaped it.
						// Those are not the same number and the gap between them is where three
						// sessions went:
						//
						//   the profile asks for a fillet;
						//   FRoadNetworkSolver::SolveNodeCuts SCALES IT DOWN to fit the arms,
						//     which is bounded by how long the player drew the segment;
						//   and the quadratic laid across the survivors is tighter again by
						//     1/sqrt(2) at a right angle.
						//
						// So a fillet that clears a truck's steering lock on paper routinely does
						// not clear it here, and NO PROFILE VALUE CAN FIX IT once the arms are
						// short - raising the request only gives the solver more to scale away.
						// Reported from play 2026-09-15 as a truck that crabbed a corner, missed
						// the junction and reversed; the route log said R=120 uu where the vehicle
						// needed 699.
						//
						// A WARNING AND NOT A REFUSAL - HERE. Since 2026-09-24 route search DOES refuse
						// this corner to any vehicle whose lock it beats (VehicleFit): the edge is still
						// laid, so the graph stays connected and other vehicles still use it, but the
						// truck that crabbed through it before is now sent another way or told "no road
						// wide enough". Deliberate - crabbing was the defect this warning was written for.
						//
						// (Original reasoning, still true of the edge itself:) The corner is still the best this junction
						// can do, and refusing to lay it would leave the network disconnected -
						// which is worse than a slow corner and much harder to diagnose. What the
						// player can act on is the segment length, so that is what this names.
						{
							// ISSUE #190: DesignVehicles is the caller's, resolved once for the whole
							// rebuild - see this function's own comment - not re-resolved per ordered
							// arm pair the way this warning used to.
							//
							// THE LESS DEMANDING OF THE TWO ARMS' DESIGN VEHICLES: the corner's fillet
							// is the smaller of the arms' (FJunctionSolver), so a Wide road meeting a
							// Narrow one is laid for the bowser, and warning there against the rig
							// would name a shortfall nobody designed that corner to meet.
							const FChassis& FromDesign = DesignVehicles.For(FromProfile);
							const FChassis& ToDesign = DesignVehicles.For(ToProfile);
							const FChassis& LargestServiceVehicle =
								FromDesign.TightestFollowableRadius() <= ToDesign.TightestFollowableRadius() ? FromDesign : ToDesign;
							const double Needed = LargestServiceVehicle.TightestFollowableRadius();

							// A TAPER IS JUDGED BY WHAT SIZED IT, not by the corner rule above (review
							// of 5660420c): the solver sized the S for the WIDER arm's design vehicle
							// (FRoadNetworkSolver's WidthTaperLength) and then capped the inset by the
							// segment's slack - so a short segment gets a tighter S, silently, and the
							// "right-angle corner" text below names the wrong fix. Said here with the
							// vehicle, the length the S needed and the segment length that holds it.
							// ENFORCED BY: Airside.Build.WidthTaper.CappedTaperWarns
							if (bTaperS)
							{
								const FChassis& Sizing = FromProfile->GetTotalWidth() >= ToProfile->GetTotalWidth() ? FromDesign : ToDesign;
								const double SizingRadius = Sizing.TightestFollowableRadius();
								const double Delivered = GuidelineGeom::TightestRadius(
									Network.GetGuidelineNode(Piece.A)->Position, Piece.Control,
									Network.GetGuidelineNode(Piece.B)->Position);
								// Once per S: both pieces are the same curve mirrored.
								if (SizingRadius > 0.0 && Delivered < SizingRadius && &Piece == &Pieces[0])
								{
									const double Shift = FMath::Abs(FVector2D::CrossProduct(TaperTravel, TaperTo - TaperFrom));
									const double Have = FVector2D::DotProduct(TaperTo - TaperFrom, TaperTravel);
									const double NeedLength = GuidelineGeom::LaneChangeLength(SizingRadius, Shift);
									UE_LOG(LogAirside, Warning,
										TEXT("Width taper at (%.0f,%.0f): its S-curve radius is %.0f uu, but the vehicle "
										     "it is sized for needs %.0f (wheelbase %.0f, lock %.0f deg). The lanes step "
										     "%.0f uu across a %.0f uu taper that needs %.0f: a segment here is too short "
										     "to hold its half. Draw each segment at the width change at least %.1f m "
										     "long, plus its far junction's cut-back."),
										Node->Position.X, Node->Position.Y, Delivered, SizingRadius,
										Sizing.Wheelbase(), Sizing.Ground.MaxSteerDegrees, Shift, Have, NeedLength,
										0.5 * NeedLength / FRoadNetworkSolver::SlackShare / 100.0);
								}
							}
							// A BEND'S ARC WARNS ONCE, from its first piece, on its tightest piece's radius
							// and the cut-back its lane ends sit at (Turn.Control, the lane lines' crossing
							// - each piece's own control is only its slice of the arc).
							else if (Needed > 0.0 && (BendArc.Num() == 0 || &Piece == &Pieces[0]))
							{
								double Delivered = GuidelineGeom::TightestRadius(
									Network.GetGuidelineNode(Piece.A)->Position,
									Piece.Control,
									Network.GetGuidelineNode(Piece.B)->Position);
								for (const FGuidelineEdge& Other : Pieces)
								{
									if (BendArc.Num() > 0)
									{
										Delivered = FMath::Min(Delivered, GuidelineGeom::TightestRadius(
											Network.GetGuidelineNode(Other.A)->Position, Other.Control,
											Network.GetGuidelineNode(Other.B)->Position));
									}
								}
								if (Delivered < Needed)
								{
									UE_LOG(LogAirside, Warning,
										TEXT("Junction at (%.0f,%.0f): turn path radius %.0f uu, but the "
										     "largest service vehicle needs %.0f (wheelbase %.0f, lock "
										     "%.0f deg). The arms are cut back %.0f uu; a right-angle "
										     "corner needs about %.0f, so the segments meeting here are "
										     "too short. Draw them longer."),
										Node->Position.X, Node->Position.Y, Delivered, Needed,
										LargestServiceVehicle.Wheelbase(), LargestServiceVehicle.Ground.MaxSteerDegrees,
										FVector2D::Distance(
											Network.GetGuidelineNode(Piece.A)->Position, BendArc.Num() > 0 ? Turn.Control : Piece.Control),
										Needed * UE_DOUBLE_SQRT_2);
								}
							}
						}

						Network.AddGuidelineEdge(MoveTemp(Piece));
					}
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

			const FGuidelineEdgeId Id = Network.GuidelineEdgeIdAt(Index);

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
				// Kind and For together through SetGuidelineNodeHoldingPosition (#191): the
				// two fields must agree ("Runway iff HoldingPositionFor is set") and a raw
				// pointer let this clear one without the other.
				Network.SetGuidelineNodeHoldingPosition(
					Network.GuidelineNodeIdAt(Index), EHoldingPositionKind::None, FRoadSegmentId());
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
				if (End == nullptr)
				{
					continue;
				}
				// SetGuidelineNodeHoldingPosition silently declines a dead node, same as the
				// null check this replaced (#191).
				Network.SetGuidelineNodeHoldingPosition(*End, EHoldingPositionKind::Runway, Pair.Value);
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
			const FGuidelineNode* Node = Network.GetGuidelineNode(*Found);
			// A mark on an end that has since become a runway end is out-ranked by the
			// derivation: the junction decides, and the stale mark is harmless.
			if (Node != nullptr && Node->HoldingPosition != EHoldingPositionKind::Runway)
			{
				Network.SetGuidelineNodeHoldingPosition(*Found, EHoldingPositionKind::Intermediate, FRoadSegmentId());
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
				Orphans.Add(Network.GuidelineNodeIdAt(Index));
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

		// ARM ENDS THAT LEAD NOWHERE FOR THEIR OWN CLASS. A service road drawn up to a
		// taxiway's side, or up to a runway, makes a junction with turn paths for everybody
		// EXCEPT the class that drew it - because a turn's mask is the INTERSECTION of the
		// two arms' masks, and there is no second arm of its own class to intersect with.
		//
		// COUNTED AND NOT REFUSED, the same treatment the road tool gives a half-drawn
		// crossing: the player may be about to draw the far side, and a crossing is drawn by
		// clicking ON the thing being crossed and then chaining onward - so a tool that
		// refused the first of those two clicks would make a crossing impossible to draw at
		// all. This line is what turns "the truck says no route" into "look at the junction
		// you left open".
		int32 DeadEnds = 0;
		for (const FGuidelineNode& Node : Network.GetGuidelineNodes())
		{
			if (!Node.bAlive || !Node.bDerived || Node.Incident.Num() != 1) { continue; }

			// Only a SEGMENT's own guideline end counts. A turn path with one end is
			// impossible, and an anchor lead-in ending at a stand is not a dead end - it is
			// the entire point of the stand.
			const FGuidelineEdge* Only = Network.GetGuidelineEdge(Node.Incident[0]);
			DeadEnds += (Only != nullptr && Only->DerivedFrom.IsSet()) ? 1 : 0;
		}

		UE_LOG(LogAirside, Log,
			TEXT("Guidelines: %d nodes (%d holding-position), %d edges (%d hand-authored, %d turn paths), "
				 "%d holding-position mark(s) on file, %d arm end(s) with no through path for their class, "
				 "%d dead-end U-turn(s), %d width-taper S lane(s), %d bend lane(s) on the pavement's arc, drive %s"),
			NodesAlive, HoldingPosition, EdgesAlive, Authored, TurnPaths,
			Network.GetHoldingPositionMarks().Num(), DeadEnds, Balloons, Tapers, BendArcs,
			Network.GetDriveSide() == EDriveSide::Left ? TEXT("left") : TEXT("right"));
	}
}
