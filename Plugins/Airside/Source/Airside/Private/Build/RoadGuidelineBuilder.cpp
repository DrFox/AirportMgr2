#include "Build/RoadGuidelineBuilder.h"

#include "AirsideLog.h"
#include "Build/ExitGeometry.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Build/TurnShape.h"
#include "Model/Chassis.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Model/VehicleFit.h"
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
	uint64 EndKey(int32 SegmentIndex, bool bEndA, int32 GuidelineIndex)
	{
		return (static_cast<uint64>(SegmentIndex) << 32)
			 | (static_cast<uint64>(GuidelineIndex) << 1)
			 | (bEndA ? 1ull : 0ull);
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

	/**
	 * Splits a derived edge Length from one of its ends, exactly (de Casteljau; a runway's
	 * derived guideline is a straight chord, so the pieces are straight too). Both pieces
	 * keep the original's identity - DerivedFrom and index - which FindSparedEdge and the
	 * route search already tolerate, because FAnchorLink::Build has split taxiways for
	 * stand lead-ins this way since before there were exits. Returns the split node and
	 * hands back the id of the piece on the FAR side, so the other end can be split next.
	 */
	FGuidelineNodeId SplitFromEnd(URoadNetwork& Network, FGuidelineEdgeId EdgeId, bool bFromA, double Length,
		FGuidelineEdgeId& OutRest)
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
	}

	/**
	 * EXIT ARCS. A continuous arm (a runway) is never cut, so its guideline ended ON the
	 * node - and a turn path whose control point is the node was then a straight line from
	 * the taxiway's cut point into the centreline, meeting it at the taxiway's angle with an
	 * instantaneous heading change (samples/runwayexits.png, 2026-09-06). The taxiway-to-
	 * taxiway turn at the same node was a proper arc, because BOTH its ends sat back from the
	 * node. So: at a MIXED node - at least one continuous arm and at least one that is not -
	 * the continuous arm's guideline is split ExitLength before the node and the turns attach
	 * there, and the non-continuous arm's guideline ends ExitLength back from the node. The
	 * same control-at-the-node quadratic then comes out tangent at both ends. See the runway
	 * exit arcs spec (docs/superpowers/specs/2026-09-06-runway-exit-arcs-design.md).
	 *
	 * Fills SetBack (how far back from the node each mixed end's guideline should stop),
	 * ContinuousEnds (which of those ends belong to the continuous arm, so it splits rather
	 * than moves off the node), and ProtectedBy (every non-continuous end's holding runway,
	 * for the holding-position re-apply pass to turn into a mark).
	 */
	void ComputeExitSetBacks(const URoadNetwork& Network, const FRoadSolveResult& Solved,
		TMap<uint64, double>& SetBack, TSet<uint64>& ContinuousEnds, TMap<uint64, FRoadSegmentId>& ProtectedBy)
	{
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
	}

	/**
	 * One derived guideline edge per (segment, declared guideline): the segment pass. A
	 * hand-authored edge already covering one is spared (FindSparedEdge) and its endpoints
	 * registered instead, so turn paths still attach to what the player drew. SetBack and
	 * ContinuousEnds (ComputeExitSetBacks) move a mixed end's guideline back from the node
	 * for its exit arc, or split it and record the split in Attach for a continuous arm.
	 *
	 * Fills Ends (every segment end's node, keyed by EndKey - the handle the turn-path pass
	 * and the re-resolve/hold-mark passes below all share) and Attach.
	 */
	void DeriveSegmentGuidelines(URoadNetwork& Network, const TArray<FRoadSegment>& Segments,
		const TMap<uint64, double>& SetBack, const TSet<uint64>& ContinuousEnds,
		TMap<uint64, FGuidelineNodeId>& Ends, TMap<uint64, FGuidelineNodeId>& Attach)
	{
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
					const FGuidelineNodeId Split = SplitFromEnd(Network, EdgeId, /*bFromA=*/true, *Back, Rest);
					if (Split.IsSet())
					{
						Attach.Add(EndKey(Index, true, Which), Split);
						EdgeId = Rest;
					}
				}
				if (const double* Back = SetBack.Find(KeyB); Back && ContinuousEnds.Contains(KeyB))
				{
					FGuidelineEdgeId Rest;
					const FGuidelineNodeId Split = SplitFromEnd(Network, EdgeId, /*bFromA=*/false, *Back, Rest);
					if (Split.IsSet())
					{
						Attach.Add(EndKey(Index, false, Which), Split);
					}
				}
			}
		}
	}

	/**
	 * A DEAD END TURNS VEHICLES ROUND (spec 2026-09-23 §4, ruled: an edge, no mesh). One-way
	 * lanes would otherwise strand anything that drove into a stub. A bidirectional arm (a
	 * taxiway, or a one-lane road) is skipped: its one line already runs both ways, and a
	 * balloon there would be a change nobody asked for.
	 *
	 * SIZED FOR THE LARGEST RIGID SERVICE VEHICLE ON EVERY TIER - DesignVehicles.Default, NOT
	 * the tier's own design vehicle the fillets use. By user ruling (2026-09-25, "smaller,
	 * reverse later"): a balloon the rig can be driven round without folding its trailer
	 * reaches ~24 m past the road end (measured). The rig turns round with a HAMMERHEAD instead -
	 * a reverse turn into a stub (FReverseTurn, ruled 2026-09-26, amending the three-point turn
	 * the 2025 ruling expected) - so it is still refused at every balloon, on its trailer
	 * folding, and that is the answer rather than a gap. Laid over grass: the balloon reaches
	 * ~4x the lock radius past the road end (UTurnGeom.h).
	 * ENFORCED BY: Airside.Build.DesignVehicle.WideDeadEndRefusesRig
	 * ENFORCED BY: Airside.Build.TwoWay.DeadEnd, Airside.Solve.UTurnBalloon
	 *
	 * Returns whether a balloon was actually added, so the caller's census count only ever
	 * reflects one that is really there.
	 */
	bool TryBuildDeadEndBalloon(URoadNetwork& Network, const FRoadNode* Node, FRoadNodeId NodeId,
		FRoadSegmentId ArmSeg, const FRoadDesignVehicles& DesignVehicles, const TMap<uint64, FGuidelineNodeId>& Ends)
	{
		const FRoadSegment* Arm = Network.GetSegment(ArmSeg);
		const URoadProfile* Profile = Arm ? Network.ProfileFor(*Arm) : nullptr;
		if (Arm == nullptr || Profile == nullptr)
		{
			return false;
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
			return false;
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
			return false;
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
		return true;
	}

	/**
	 * THE FILLET A REVERSE TURN IS LAID WITH, uu, before it is clamped to the room the two arms
	 * give. 15 m: the radius a lorry driver backs a 13.6 m trailer round into a dock, and the
	 * radius Airside.Solve.TowReverse.ArcTrackedWithinTolerance holds the rig to within 7 uu on
	 * (2026-09-26). Tighter is the solver's to refuse, not this constant's to forbid.
	 */
	constexpr double ReverseFilletRadius = 1500.0;

	/** The least fillet a reverse turn is laid with when the arms leave less room; below it, not laid. */
	constexpr double MinReverseFilletRadius = 600.0;

	/** uu of straight retrace the stopped vehicle's trailer axle is given beyond its chain, before the fillet. */
	constexpr double ReversePullPastMargin = 200.0;

	/** uu kept between the reversed vehicle's rear overhang and the end of the bay's lane. */
	constexpr double ReverseEndMargin = 50.0;

	/** Why a record failed: gone for good (drop it), or not laid THIS rebuild (keep it, an edit may fix it). */
	enum class EReverseTurnFail : uint8 { None, Gone, NotLaid };

	/** The segment of Node whose other end is Far, or unset. */
	FRoadSegmentId ArmTowards(const URoadNetwork& Network, const FRoadNode& Node, FRoadNodeId NodeId, FRoadNodeId Far)
	{
		for (const FRoadSegmentId& Seg : Node.Incident)
		{
			if (Network.GetOtherEnd(Seg, NodeId) == Far)
			{
				return Seg;
			}
		}
		return FRoadSegmentId();
	}

	/** Whether Segment's lane Which has a derived edge bowed more than 1 uu off its chord. */
	bool LaneIsCurved(const URoadNetwork& Network, FRoadSegmentId Segment, int32 Which)
	{
		for (const FGuidelineEdge& Edge : Network.GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.bDerived && Edge.DerivedFrom == Segment && Edge.DerivedGuidelineIndex == Which)
			{
				const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
				const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
				if (A != nullptr && B != nullptr && FVector2D::Distance(Edge.Control, (A->Position + B->Position) * 0.5) > 1.0)
				{
					return true;
				}
			}
		}
		return false;
	}

	/**
	 * LAYS ONE REVERSE TURN (spec 2026-09-26 §3; FReverseTurn's header says what, this says how):
	 * a chain of bReverseLeg edges from the pull-past lane end, back along that lane, round a
	 * fillet onto the bay arm's lane towards Node, on to the bay's end; then one forward exit
	 * edge from there back up that lane to its end at Node. Returns the bay-end node, or unset
	 * with OutWhy and OutFail saying why.
	 *
	 * THE PULL-PAST IS CHECKED, NOT ASSUMED: the arm pulled past along must hold the design
	 * vehicle's straight chain (steered axle to rearmost axle) plus ReversePullPastMargin before
	 * the fillet starts, or the stopped vehicle's trailer is still in the junction's corner when
	 * the reverse arms and TowReverse refuses it off the line. The design vehicle is the pull-past
	 * arm's tier's (FRoadDesignVehicles::VehicleFor), the one its corners are laid for.
	 */
	FGuidelineNodeId LayReverseTurn(URoadNetwork& Network, const FReverseTurn& Turn, const FRoadDesignVehicles& DesignVehicles,
		const TMap<uint64, FGuidelineNodeId>& Ends, FString& OutWhy, EReverseTurnFail& OutFail)
	{
		OutFail = EReverseTurnFail::Gone;
		const FRoadNode* Node = Network.GetNode(Turn.Node);
		if (Node == nullptr || !Node->bAlive)
		{
			OutWhy = TEXT("its junction has gone");
			return FGuidelineNodeId();
		}
		const FRoadSegmentId SegA = ArmTowards(Network, *Node, Turn.Node, Turn.FromFar);
		const FRoadSegmentId SegB = ArmTowards(Network, *Node, Turn.Node, Turn.IntoFar);
		const FRoadSegment* ArmA = Network.GetSegment(SegA);
		const FRoadSegment* ArmB = Network.GetSegment(SegB);
		if (ArmA == nullptr || ArmB == nullptr || SegA == SegB)
		{
			OutWhy = TEXT("an arm it names has gone");
			return FGuidelineNodeId();
		}

		OutFail = EReverseTurnFail::NotLaid;
		const URoadProfile* ProfileA = Network.ProfileFor(*ArmA);
		const URoadProfile* ProfileB = Network.ProfileFor(*ArmB);
		if (ProfileA == nullptr || ProfileB == nullptr)
		{
			OutWhy = TEXT("an arm has no profile");
			return FGuidelineNodeId();
		}
		// A: the lane LEAVING Node (the vehicle pulls past along it). B: the lane ARRIVING at Node
		// (the vehicle backs in on it and drives out along it) - TryBuildDeadEndBalloon's reading.
		const bool bNodeIsA_A = ArmA->A == Turn.Node;
		const bool bNodeIsA_B = ArmB->A == Turn.Node;
		int32 WhichA = INDEX_NONE;
		int32 WhichB = INDEX_NONE;
		for (int32 Which = 0; Which < ProfileA->Guidelines.Num(); ++Which)
		{
			const EGuidelineDir Dir = ProfileA->Guidelines[Which].Direction;
			if ((bNodeIsA_A && Dir == EGuidelineDir::AToB) || (!bNodeIsA_A && Dir == EGuidelineDir::BToA))
			{
				WhichA = Which;
			}
		}
		for (int32 Which = 0; Which < ProfileB->Guidelines.Num(); ++Which)
		{
			const EGuidelineDir Dir = ProfileB->Guidelines[Which].Direction;
			if ((bNodeIsA_B && Dir == EGuidelineDir::BToA) || (!bNodeIsA_B && Dir == EGuidelineDir::AToB))
			{
				WhichB = Which;
			}
		}
		const FGuidelineNodeId* StopEnd = WhichA != INDEX_NONE ? Ends.Find(EndKey(SegA.Index, !bNodeIsA_A, WhichA)) : nullptr;
		const FGuidelineNodeId* ANearEnd = WhichA != INDEX_NONE ? Ends.Find(EndKey(SegA.Index, bNodeIsA_A, WhichA)) : nullptr;
		const FGuidelineNodeId* BNearEnd = WhichB != INDEX_NONE ? Ends.Find(EndKey(SegB.Index, bNodeIsA_B, WhichB)) : nullptr;
		const FGuidelineNodeId* BFarEnd = WhichB != INDEX_NONE ? Ends.Find(EndKey(SegB.Index, !bNodeIsA_B, WhichB)) : nullptr;
		if (StopEnd == nullptr || ANearEnd == nullptr || BNearEnd == nullptr || BFarEnd == nullptr)
		{
			OutWhy = TEXT("an arm has no one-way lane each way (a bidirectional road backs along its own line)");
			return FGuidelineNodeId();
		}
		// STRAIGHT ARMS ONLY: the retrace and the bay run are laid as straight lines between lane
		// ends, so a bowed lane would put the reverse leg off the lane the vehicle actually drove.
		if (LaneIsCurved(Network, SegA, WhichA) || LaneIsCurved(Network, SegB, WhichB))
		{
			OutWhy = TEXT("an arm is curved - reverse turns are laid on straight arms");
			return FGuidelineNodeId();
		}

		const FVector2D Stop = Network.GetGuidelineNode(*StopEnd)->Position;
		const FVector2D ANear = Network.GetGuidelineNode(*ANearEnd)->Position;
		const FVector2D BNear = Network.GetGuidelineNode(*BNearEnd)->Position;
		const FVector2D BFar = Network.GetGuidelineNode(*BFarEnd)->Position;
		const FVector2D DirA = (ANear - Stop).GetSafeNormal();   // reverse travel along the pull-past lane
		const FVector2D DirB = (BFar - BNear).GetSafeNormal();   // reverse travel into the bay

		const FVehicle& Design = DesignVehicles.VehicleFor(ProfileA);
		const double Chain = VehicleFit::ChainLength(Design);
		const double Overhang = Design.Tow.Num() > 0 ? Design.Tow.Last().BodyRear : FMath::Abs(Design.BodyRearX);
		const FVector2D End = BFar - DirB * (Overhang + ReverseEndMargin);
		// THE BAY HOLDS THE WHOLE VEHICLE: backed in, its trailer axle on End, its steered axle a
		// chain's length back up the bay - and that must still be on the bay's own lane, short of
		// the lane end at the junction, or the exit (which runs up that lane) does not pass under
		// the cab. Found on the M_RigTest yard (2026-09-26): a 25 m bay left the rig's cab 179 uu
		// out on the junction's turn path and the drive-out folded the trailer.
		const double BayRun = FVector2D::DotProduct(End - BNear, DirB);
		if (BayRun < Chain + ReversePullPastMargin)
		{
			OutWhy = FString::Printf(TEXT("the bay holds %.0f uu from its junction to its end; the design vehicle needs %.0f (chain %.0f + %.0f) to back in whole"),
				BayRun, Chain + ReversePullPastMargin, Chain, ReversePullPastMargin);
			return FGuidelineNodeId();
		}

		const FProfileGuideline& LineA = ProfileA->Guidelines[WhichA];
		const FProfileGuideline& LineB = ProfileB->Guidelines[WhichB];
		FTrafficMask Mask = FTrafficMask::Only(LineA.Class);
		Mask.Add(LineB.Class);
		Mask.Add(ETraversalClass::Emergency);
		const double Width = FMath::Min(LineA.Width, LineB.Width);
		auto AddEdge = [&](FGuidelineNodeId From, FGuidelineNodeId To, const FVector2D& Control, bool bReverse)
		{
			FGuidelineEdge Edge;
			Edge.A = From;
			Edge.B = To;
			Edge.Control = Control;
			Edge.AllowedTraffic = Mask;
			Edge.Direction = EGuidelineDir::AToB;
			Edge.Width = Width;
			// UNMEASURED ON PURPOSE: a reverse is judged over its whole length by TowReverse in
			// VehicleFit::JudgePlan, and VehicleFit::Judge says nothing for a bReverseLeg edge.
			Edge.MinRadius = 0.0;
			Edge.bDerived = true;
			Edge.bReverseLeg = bReverse;
			Network.AddGuidelineEdge(MoveTemp(Edge));
		};

		const double Cross = FVector2D::CrossProduct(DirA, DirB);
		const double Dot = FVector2D::DotProduct(DirA, DirB);
		const double PullPast = FVector2D::Distance(Stop, ANear);
		FGuidelineNodeId EndNode;
		if (FMath::Abs(Cross) < FMath::Sin(FMath::DegreesToRadians(1.0)) && Dot > 0.0)
		{
			// OPPOSITE ARMS: a straight bay. The two lanes must be ONE line - the bay's lane
			// towards Node continuing the pull-past lane - or the "straight" reverse would jog.
			const double Offset = FMath::Abs(FVector2D::CrossProduct(DirA, BNear - Stop));
			if (Offset > 1.0)
			{
				OutWhy = FString::Printf(TEXT("the arms are opposite but their lanes are %.0f uu out of line"), Offset);
				return FGuidelineNodeId();
			}
			if (PullPast < Chain + ReversePullPastMargin)
			{
				OutWhy = FString::Printf(TEXT("pull-past %.0f uu < %.0f (the design vehicle's chain %.0f + %.0f)"),
					PullPast, Chain + ReversePullPastMargin, Chain, ReversePullPastMargin);
				return FGuidelineNodeId();
			}
			EndNode = Network.AddGuidelineNode(End);
			AddEdge(*StopEnd, EndNode, (Stop + End) * 0.5, /*bReverse=*/true);
		}
		else
		{
			// A TURN: the two lane lines meet at Corner; the fillet's tangent points sit Tangent
			// either side of it, R tan(theta/2), clamped so the pull-past before it still holds
			// the chain and the bay after it still gives the trailer half a chain to settle.
			const double Denominator = Cross;
			if (FMath::Abs(Denominator) < UE_KINDA_SMALL_NUMBER)
			{
				OutWhy = TEXT("the arms' lanes are parallel and point apart");
				return FGuidelineNodeId();
			}
			const double AlongA = FVector2D::CrossProduct(BNear - Stop, DirB) / Denominator;
			const FVector2D Corner = Stop + DirA * AlongA;
			const double Theta = RoadGeom::AngleBetween(DirA, DirB);
			const double HalfTan = FMath::Tan(Theta * 0.5);
			const double RoomA = AlongA - Chain - ReversePullPastMargin;
			const double RoomB = FVector2D::DotProduct(End - Corner, DirB) - 0.5 * Chain;
			const double Radius = FMath::Min(ReverseFilletRadius, FMath::Min(RoomA, RoomB) / FMath::Max(HalfTan, UE_KINDA_SMALL_NUMBER));
			if (Radius < MinReverseFilletRadius)
			{
				OutWhy = FString::Printf(TEXT("no room for a fillet: R %.0f uu < %.0f (pull-past room %.0f, bay room %.0f uu, chain %.0f)"),
					Radius, MinReverseFilletRadius, RoomA, RoomB, Chain);
				return FGuidelineNodeId();
			}
			const double Tangent = Radius * HalfTan;
			const FVector2D T1 = Corner - DirA * Tangent;
			const FVector2D T2 = Corner + DirB * Tangent;
			const FVector2D Normal = Cross > 0.0 ? FVector2D(-DirA.Y, DirA.X) : FVector2D(DirA.Y, -DirA.X);
			const FVector2D Centre = T1 + Normal * Radius;
			TArray<GuidelineGeom::FArcPiece> Pieces;
			if (!GuidelineGeom::Arc(T1, DirA, T2, DirB, Centre, GuidelineGeom::BendArcPieceSweep, Pieces) || Pieces.Num() == 0)
			{
				OutWhy = TEXT("the fillet did not lay");
				return FGuidelineNodeId();
			}
			const FGuidelineNodeId T1Node = Network.AddGuidelineNode(T1);
			AddEdge(*StopEnd, T1Node, (Stop + T1) * 0.5, true);
			FGuidelineNodeId Prev = T1Node;
			for (const GuidelineGeom::FArcPiece& Piece : Pieces)
			{
				const FGuidelineNodeId Next = Network.AddGuidelineNode(Piece.End);
				AddEdge(Prev, Next, Piece.Control, true);
				Prev = Next;
			}
			EndNode = Network.AddGuidelineNode(End);
			AddEdge(Prev, EndNode, (Pieces.Last().End + End) * 0.5, true);
		}

		// THE EXIT: forward from the bay end back up the lane it backed in on, to that lane's end
		// at Node, where the junction's own turn paths take it on.
		AddEdge(EndNode, *BNearEnd, (End + BNear) * 0.5, /*bReverse=*/false);
		OutFail = EReverseTurnFail::None;
		return EndNode;
	}

	/**
	 * EVERY REVERSE TURN, laid; a record whose junction or arm has gone is dropped (logged), one
	 * that merely could not be laid this time is kept (logged) - an edit may make room for it.
	 * Returns how many were laid.
	 */
	int32 BuildReverseTurns(URoadNetwork& Network, const FRoadDesignVehicles& DesignVehicles,
		const TMap<uint64, FGuidelineNodeId>& Ends)
	{
		TArray<FGuidelineNodeId> TurnEnds;
		TArray<int32> Gone;
		int32 Laid = 0;
		const TArray<FReverseTurn> Turns = Network.GetReverseTurns();
		for (int32 Index = 0; Index < Turns.Num(); ++Index)
		{
			FString Why;
			EReverseTurnFail Fail = EReverseTurnFail::None;
			const FGuidelineNodeId End = LayReverseTurn(Network, Turns[Index], DesignVehicles, Ends, Why, Fail);
			TurnEnds.Add(End);
			if (End.IsSet())
			{
				++Laid;
				continue;
			}
			const FRoadNode* Node = Network.GetNode(Turns[Index].Node);
			const FVector2D At = Node != nullptr ? Node->Position : FVector2D::ZeroVector;
			UE_LOG(LogAirside, Warning, TEXT("Reverse turn %d at (%.0f,%.0f) %s: %s"), Index, At.X, At.Y,
				Fail == EReverseTurnFail::Gone ? TEXT("dropped") : TEXT("not laid"), *Why);
			if (Fail == EReverseTurnFail::Gone)
			{
				Gone.Add(Index);
			}
		}
		Network.SetReverseTurnEnds(MoveTemp(TurnEnds));
		for (int32 K = Gone.Num() - 1; K >= 0; --K)
		{
			Network.RemoveReverseTurnAt(Gone[K]);
		}
		return Laid;
	}

	/**
	 * --- Re-resolve hand-authored edges ---
	 *
	 * A player's edge survived the clear pass, but the nodes it was drawn between are not
	 * the nodes this derivation just made: AddGuidelineNode never deduplicates, so every
	 * rebuild produces FRESH coincident nodes and the player's edge would keep pointing at
	 * the old ones. It would still draw, and route nothing - breaking on a road edit that
	 * had nothing to do with it.
	 *
	 * So an end that knows what it IS gets re-pointed at whatever now holds that identity.
	 * Ends is the builder's own map, keyed exactly this way, and is simply no longer thrown
	 * away.
	 */
	void ReResolveAuthoredEdges(URoadNetwork& Network, const TMap<uint64, FGuidelineNodeId>& Ends)
	{
		auto Resolve = [&Ends](const FGuidelineEndRef& Ref, FGuidelineNodeId& Out) -> bool
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

	/**
	 * --- Re-apply holding-position marks ---
	 *
	 * The flag lives on a node and every derived node above is FRESH, so a bar the player
	 * placed would vanish on the next road edit. The mark is stored by the same identity
	 * a hand-authored edge stores its ends by, and resolved through the same Ends map -
	 * one source (the mark), one cache (the flag), rebuilt together. Spec 2026-09-06 §6.
	 */
	void ReapplyHoldingPositionMarks(URoadNetwork& Network, const TArray<FRoadSegment>& Segments,
		const TMap<uint64, FGuidelineNodeId>& Ends, const TMap<uint64, FRoadSegmentId>& ProtectedBy)
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

	TMap<uint64, FGuidelineNodeId> Ends;

	const TArray<FRoadSegment>& Segments = Network.GetSegments();

	// EXIT ARCS at MIXED nodes (a runway meeting a taxiway): ComputeExitSetBacks below.
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

	ComputeExitSetBacks(Network, Solved, SetBack, ContinuousEnds, ProtectedBy);

	DeriveSegmentGuidelines(Network, Segments, SetBack, ContinuousEnds, Ends, Attach);

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

		// A DEAD END TURNS VEHICLES ROUND, via TryBuildDeadEndBalloon - see its comment. A
		// bidirectional arm (a taxiway, or a one-lane road) is skipped: its one line already
		// runs both ways, and a balloon there would be a change nobody asked for.
		if (ArmSegments->Num() == 1)
		{
			if (TryBuildDeadEndBalloon(Network, Node, NodeId, (*ArmSegments)[0], DesignVehicles, Ends))
			{
				++Balloons;
			}
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
					// DerivedFrom's counterpart for a turn path (issue #324) - see its own
					// comment. Set once, on this shared template, so every piece the ETurnShape
					// switch below copies it into (a BendArc's interior pieces included) carries
					// it, letting FAnchorLink::Join re-measure a split piece of ANY of them.
					Turn.AtJunction = NodeId;

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

					// THE TURN'S SHAPE, DECIDED ONCE (issue #305): a bend's arc concentric with the
					// pavement's inner edge, a width taper's S, or the plain chord - see
					// ClassifyTurn's own comment (Build/TurnShape.h) for the construction and the
					// rulings behind each. It also sets Turn.Control for the TaperS case (its own
					// ControlIn), exactly as the inline construction this replaced did.
					const FTurnGeometry TurnGeom = ClassifyTurn(Network, Solved, Pair.Value, Pair.Key, NodeId,
						*ArmSegments, FromSeg, ToSeg, FromProfile, ToProfile, Declared.Class, ToDeclared.Class, Turn);

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
					switch (TurnGeom.Shape)
					{
					case ETurnShape::BendArc:
					{
						// THE BEND'S ARC, piece by piece; its last End is Turn.B's position verbatim,
						// and the last piece joins that node BY HANDLE.
						const FGuidelineNodeId Last = Turn.B;
						FGuidelineNodeId Prev = Turn.A;
						for (int32 Index = 0; Index < TurnGeom.BendArc.Num(); ++Index)
						{
							FGuidelineEdge Piece = Turn;
							Piece.A = Prev;
							Piece.B = Index + 1 == TurnGeom.BendArc.Num() ? Last : Network.AddGuidelineNode(TurnGeom.BendArc[Index].End);
							Piece.Control = TurnGeom.BendArc[Index].Control;
							Pieces.Add(Piece);
							Prev = Piece.B;
						}
						++BendArcs;
						break;
					}
					case ETurnShape::TaperS:
					{
						const FGuidelineNodeId Mid = Network.AddGuidelineNode(TurnGeom.TaperMid);
						FGuidelineEdge Second = Turn;
						Turn.B = Mid;
						Second.A = Mid;
						Second.Control = TurnGeom.TaperControlOut;
						Pieces.Add(Turn);
						Pieces.Add(Second);
						++Tapers;
						break;
					}
					case ETurnShape::Chord:
					default:
						Pieces.Add(Turn);
						break;
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
							if (TurnGeom.Shape == ETurnShape::TaperS)
							{
								const FChassis& Sizing = FromProfile->GetTotalWidth() >= ToProfile->GetTotalWidth() ? FromDesign : ToDesign;
								const double SizingRadius = Sizing.TightestFollowableRadius();
								const double Delivered = GuidelineGeom::TightestRadius(
									Network.GetGuidelineNode(Piece.A)->Position, Piece.Control,
									Network.GetGuidelineNode(Piece.B)->Position);
								// Once per S: both pieces are the same curve mirrored.
								if (SizingRadius > 0.0 && Delivered < SizingRadius && &Piece == &Pieces[0])
								{
									const double Shift = FMath::Abs(FVector2D::CrossProduct(TurnGeom.TaperTravel, TurnGeom.TaperTo - TurnGeom.TaperFrom));
									const double Have = FVector2D::DotProduct(TurnGeom.TaperTo - TurnGeom.TaperFrom, TurnGeom.TaperTravel);
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
							else if (Needed > 0.0 && (TurnGeom.Shape != ETurnShape::BendArc || &Piece == &Pieces[0]))
							{
								double Delivered = GuidelineGeom::TightestRadius(
									Network.GetGuidelineNode(Piece.A)->Position,
									Piece.Control,
									Network.GetGuidelineNode(Piece.B)->Position);
								for (const FGuidelineEdge& Other : Pieces)
								{
									if (TurnGeom.Shape == ETurnShape::BendArc)
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
											Network.GetGuidelineNode(Piece.A)->Position, TurnGeom.Shape == ETurnShape::BendArc ? Turn.Control : Piece.Control),
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

	// Re-resolve hand-authored edges onto this rebuild's fresh nodes (ReResolveAuthoredEdges),
	// then re-apply holding-position marks onto them (ReapplyHoldingPositionMarks) - see each
	// function's comment. Order matters: marks are re-applied by the same Ends-keyed identity
	// the re-resolve pass just used, onto nodes that pass has already made current.
	// REVERSE TURNS, onto this rebuild's lane ends, before the orphan sweep below - their edges
	// are what keep their new nodes from being swept.
	const int32 ReverseTurnsLaid = BuildReverseTurns(Network, DesignVehicles, Ends);

	ReResolveAuthoredEdges(Network, Ends);
	ReapplyHoldingPositionMarks(Network, Segments, Ends, ProtectedBy);

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
		// Its own line, and only when there are any: the census line above is grepped by tests.
		if (Network.GetReverseTurns().Num() > 0)
		{
			UE_LOG(LogAirside, Log, TEXT("Reverse turns: %d laid of %d on file"), ReverseTurnsLaid, Network.GetReverseTurns().Num());
		}
	}
}

void FRoadGuidelineBuilder::MeasureSplitHalf(URoadNetwork& Network, FGuidelineEdgeId Half,
	const FRoadSolveResult& Solved, FRoadNodeId JunctionNode)
{
	const FGuidelineEdge* Existing = Network.GetGuidelineEdge(Half);
	if (Existing == nullptr || Existing->DerivedFrom.IsSet() || !JunctionNode.IsSet())
	{
		// A lane's own split half (DerivedFrom set) carries no MeasureTurn contract at all -
		// its Width gates it, same as before this function existed - and an unresolved node
		// means the caller had nothing to measure against (see this function's own header
		// comment on when that is legitimate).
		return;
	}

	const FJunctionResult* Result = Solved.NodeResults.Find(JunctionNode.Index);
	const TArray<FRoadSegmentId>* ArmSegments = Solved.NodeArmSegments.Find(JunctionNode.Index);
	if (Result == nullptr || ArmSegments == nullptr || !Result->bValid)
	{
		return;
	}

	// THE SAME PAVEMENT CONSTRUCTION Build's own per-node loop uses above - the junction's
	// boundary polygon (minus the fan centre SolveBoundary appends) plus every arm's ribbon.
	FJunctionPavement Pavement;
	if (Result->Boundary.Num() > 3)
	{
		Pavement.Polygon = Result->Boundary;
		Pavement.Polygon.Pop();
	}
	for (const FRoadSegmentId& ArmSeg : *ArmSegments)
	{
		if (const FRoadSegment* Arm = Network.GetSegment(ArmSeg))
		{
			Pavement.Ribbons.Add({ Arm->LeftCutA, Arm->RightCutA, Arm->LeftCutB, Arm->RightCutB });
		}
	}

	const FGuidelineNode* A = Network.GetGuidelineNode(Existing->A);
	const FGuidelineNode* B = Network.GetGuidelineNode(Existing->B);
	if (A == nullptr || B == nullptr)
	{
		return;
	}

	// A SCRATCH COPY, MEASURED IN PLACE, then written back through the one narrow mutator
	// Model/ exposes for this whole fact (issue #324) - MeasureTurn takes an FGuidelineEdge&
	// to mutate exactly as Build's own per-piece loop above does, and this is Build/'s own
	// static, so there is no reason to duplicate its body for a five-field write instead.
	FGuidelineEdge Scratch = *Existing;
	MeasureTurn(Scratch, A->Position, B->Position, Pavement);
	// RETURN DISCARDED: false only means Half is dead, which Existing already ruled out above
	// and nothing between there and here removes an edge to make happen.
	Network.SetGuidelineEdgeMeasurement(Half, Scratch.MinRadius, Scratch.ClearInner, Scratch.ClearOuter,
		MoveTemp(Scratch.ClearInnerAt), MoveTemp(Scratch.ClearOuterAt));
}
