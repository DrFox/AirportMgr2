#include "Build/StandTurnOffMarkingBuilder.h"

#include "Build/MarkingQuads.h"
#include "Build/MarkingText.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiwayStrip.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

namespace
{
	/** One taxiway's pavement as a question: its sampled centreline and half-width. */
	struct FPavedTaxiway
	{
		TArray<FVector2D> Centre;
		double HalfWidth = 0.0;
		FBox2D Bounds = FBox2D(ForceInit);
	};

	/**
	 * Every taxiway's pavement - TaxiwayStrip::HasStrip, the one "is this a taxiway" rule - with
	 * its centreline sampled by GuidelineGeom::Sample, the one quadratic sampler. THE WIDER HALF
	 * of an asymmetric profile, TaxiwayStrip::WorstIntrusion's own choice, so the two agree on
	 * where the pavement ends.
	 *
	 * EVERY TAXIWAY, not only the one the lead-in joined: near a junction a sweep can run onto
	 * the neighbouring segment's pavement, and "paint where paved" means any pavement. Linear:
	 * N was 34 segments on M_Test, 2026-09-28 (TaxiwayStrip.cpp's own figure), and this runs
	 * once per Topology rebuild, not per frame.
	 */
	TArray<FPavedTaxiway> PavementOf(const URoadNetwork& Network)
	{
		TArray<FPavedTaxiway> Out;
		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			FVector2D A, B;
			if (!Id.IsSet() || !TaxiwayStrip::HasStrip(Network, Id) || !Network.SegmentEnds(Id, A, B))
			{
				continue;
			}
			FPavedTaxiway Paved;
			GuidelineGeom::Sample(A, Segments[Index].Control, B, Paved.Centre);
			Paved.HalfWidth = Segments[Index].Profile->GetMaxHalfWidth();
			for (const FVector2D& P : Paved.Centre)
			{
				Paved.Bounds += P;
			}
			Paved.Bounds = Paved.Bounds.ExpandBy(Paved.HalfWidth);
			Out.Add(MoveTemp(Paved));
		}
		return Out;
	}

	bool OnPavement(const TArray<FPavedTaxiway>& Pavement, const FVector2D& P)
	{
		for (const FPavedTaxiway& Paved : Pavement)
		{
			if (!Paved.Bounds.IsInside(P))
			{
				continue;
			}
			for (int32 I = 0; I + 1 < Paved.Centre.Num(); ++I)
			{
				const double T = RoadGeom::ClosestPointOnSegment(Paved.Centre[I], Paved.Centre[I + 1], P);
				if (FVector2D::Distance(P, Paved.Centre[I] + (Paved.Centre[I + 1] - Paved.Centre[I]) * T) <= Paved.HalfWidth)
				{
					return true;
				}
			}
		}
		return false;
	}

	/** A link FAnchorLink::Join laid for an aircraft: not a road's own guideline, not a lane. */
	bool IsAircraftLink(const FGuidelineEdge& Edge)
	{
		return Edge.bAlive && !Edge.DerivedFrom.IsSet() && !Edge.StandGeometryOwner.IsSet()
			&& Edge.AllowedTraffic.Allows(ETraversalClass::Aircraft);
	}

	FVector2D PositionOf(const URoadNetwork& Network, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
		return Found != nullptr ? Found->Position : FVector2D::ZeroVector;
	}

	/**
	 * The taxiway guideline at Node - an incident edge a TAXIWAY SEGMENT derived (HasStrip) - or
	 * null. Its segment says how wide the pavement is; its tangent which way the taxiway runs.
	 */
	const FGuidelineEdge* TaxiwayEdgeAt(const URoadNetwork& Network, FGuidelineNodeId Node)
	{
		const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
		if (Found == nullptr)
		{
			return nullptr;
		}
		for (const FGuidelineEdgeId Id : Found->Incident)
		{
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id);
			if (Edge != nullptr && Edge->bAlive && Edge->DerivedFrom.IsSet() && TaxiwayStrip::HasStrip(Network, Edge->DerivedFrom))
			{
				return Edge;
			}
		}
		return nullptr;
	}

	/**
	 * Edge's samples, ordered to start at From - URoadNetwork::SampleGuideline, the graph's one
	 * edge sampler, whose bFromB walk swaps the endpoints rather than reversing an array (final
	 * review 2026-09-29: this used to sample then Algo::Reverse, a second spelling of the walk
	 * the follower takes). ENFORCED BY: Airside.Build.StandTurnOff.FollowsTheDerivedEdges.
	 */
	TArray<FVector2D> SamplesFrom(const URoadNetwork& Network, FGuidelineEdgeId EdgeId, FGuidelineNodeId From)
	{
		TArray<FVector2D> Points;
		const FGuidelineEdge* Edge = Network.GetGuidelineEdge(EdgeId);
		if (Edge != nullptr)
		{
			Network.SampleGuideline(EdgeId, Points, /*bFromB=*/Edge->B == From);
		}
		return Points;
	}

	/**
	 * Paint Points' spans that lie on pavement, LeadInWidth wide, and return the painted run as
	 * a polyline (for the arrow). A span that leaves the pavement is cut where it does, found by
	 * bisection ALONG THE SPAN - a point of the sampled polyline, so the cut paint still lies on
	 * the line the aircraft follows.
	 */
	TArray<FVector2D> PaintPaved(FRoadMeshBuffers& Out, double Z, const TArray<FVector2D>& Points,
		const TArray<FPavedTaxiway>& Pavement, int32 MaterialID, FStandTurnOffCensus& Census)
	{
		TArray<FVector2D> Run;
		const double Half = FStandMarkingBuilder::LeadInWidth * 0.5;
		for (int32 I = 0; I + 1 < Points.Num(); ++I)
		{
			FVector2D P0 = Points[I];
			FVector2D P1 = Points[I + 1];
			const bool bIn0 = OnPavement(Pavement, P0);
			const bool bIn1 = OnPavement(Pavement, P1);
			if (!bIn0 && !bIn1)
			{
				continue;
			}
			if (bIn0 != bIn1)
			{
				// Twenty halvings: a span is at most a few metres, so the cut lands well inside a
				// millimetre of the pavement edge.
				const FVector2D Inside = bIn0 ? P0 : P1;
				const FVector2D Outside = bIn0 ? P1 : P0;
				double Lo = 0.0, Hi = 1.0;
				for (int32 Step = 0; Step < 20; ++Step)
				{
					const double Mid = (Lo + Hi) * 0.5;
					(OnPavement(Pavement, Inside + (Outside - Inside) * Mid) ? Lo : Hi) = Mid;
				}
				const FVector2D Cut = Inside + (Outside - Inside) * Lo;
				(bIn0 ? P1 : P0) = Cut;
			}
			const FVector2D Along = (P1 - P0).GetSafeNormal();
			if (Along.IsNearlyZero())
			{
				continue;
			}
			const FVector2D Across = RoadGeom::PerpCCW(Along) * Half;
			MarkingQuads::AddQuad(Out, Z, P0 - Across, P1 - Across, P1 + Across, P0 + Across, MaterialID);
			Census.LeadInCentres.Add((P0 + P1) * 0.5);
			if (Run.Num() == 0 || !Run.Last().Equals(P0, UE_KINDA_SMALL_NUMBER))
			{
				Run.Add(P0);
			}
			Run.Add(P1);
		}
		return Run;
	}

	/**
	 * A short STRAIGHT arrow from Tail to Tip: a shaft LeadInWidth wide, then a head. The
	 * sample's arrow (samples/standsigns.png) - it replaced a chevron half-way round the sweep.
	 * Quads wound as PaintPaved winds its lead-in (along the direction, across = PerpCCW), so they
	 * face up the same way.
	 */
	void Arrow(FRoadMeshBuffers& Out, double Z, const FVector2D& Tail, const FVector2D& Tip, int32 MaterialID)
	{
		using B = FStandTurnOffMarkingBuilder;
		const FVector2D Dir = (Tip - Tail).GetSafeNormal();
		const FVector2D Across = RoadGeom::PerpCCW(Dir);
		const FVector2D Base = Tip - Dir * FMath::Min(B::ArrowHeadLength, FVector2D::Distance(Tail, Tip));
		const FVector2D Half = Across * (FStandMarkingBuilder::LeadInWidth * 0.5);
		if (FVector2D::Distance(Tail, Base) > UE_KINDA_SMALL_NUMBER)
		{
			MarkingQuads::AddQuad(Out, Z, Tail - Half, Base - Half, Base + Half, Tail + Half, MaterialID);
		}
		// THE HEAD as a quad collapsed at the tip - MarkingQuads has no triangle, and the degenerate
		// half draws nothing.
		const FVector2D HeadHalf = Across * (B::ArrowHeadWidth * 0.5);
		MarkingQuads::AddQuad(Out, Z, Base - HeadHalf, Tip, Tip, Base + HeadHalf, MaterialID);
	}

	/**
	 * One face of the number sign: a black box, then Text in yellow on it, reading along the axis
	 * with its glyph tops toward it. Out is the unit direction from the axis to this face. Returns
	 * the face for the census.
	 */
	FStandSignFace SignFace(FRoadMeshBuffers& Out, double Z, const FVector2D& Mid, const FVector2D& In,
		const FVector2D& Outward, const FString& Text, int32 GuidanceId, int32 BlackId)
	{
		using B = FStandTurnOffMarkingBuilder;
		const double TextLength = MarkingText::Width(Text, B::NumberHeight, B::NumberSpacing);
		const double AlongHalf = TextLength * 0.5 + B::SignBoxPadding;
		const double AcrossHalf = B::NumberHeight * 0.5 + B::SignBoxPadding;
		const FVector2D Centre = Mid + Outward * (B::SignFaceGap + AcrossHalf);

		// THE BOX, below the digits (SignBoxZDrop), wound along In with across = PerpCCW(In) - the
		// lead-in's own winding, so it faces up.
		const FVector2D P0 = Centre - In * AlongHalf;
		const FVector2D P1 = Centre + In * AlongHalf;
		const FVector2D Across = RoadGeom::PerpCCW(In) * AcrossHalf;
		MarkingQuads::AddQuad(Out, Z - B::SignBoxZDrop, P0 - Across, P1 - Across, P1 + Across, P0 + Across, BlackId);

		// THE DIGITS: tops toward the arrow, so the face on the other side reads the other way -
		// one for a pilot on each side (user 2026-09-29). Right = PerpCCW(Up), the pairing the
		// runway designations and this builder have always painted unmirrored.
		FStandSignFace Face;
		Face.Up = -Outward;
		Face.Right = RoadGeom::PerpCCW(Face.Up);
		Face.Origin = Centre + Outward * (B::NumberHeight * 0.5);
		MarkingText::AddString(Out, Z, Face.Origin, Face.Up, Face.Right, B::NumberHeight, B::NumberSpacing, Text, GuidanceId);
		return Face;
	}
}

int32 FStandTurnOffMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
	FStandTurnOffCensus* Census, const FStandPaintIds& PaintIds)
{
	FStandTurnOffCensus Local;
	FStandTurnOffCensus& C = Census != nullptr ? *Census : Local;
	const int32 Id = PaintIds[EStandPaint::Guidance];
	const int32 BlackId = PaintIds[EStandPaint::SignBackground];
	const TArray<FPavedTaxiway> Pavement = PavementOf(Network);
	if (Pavement.Num() == 0)
	{
		return 0;
	}

	int32 Painted = 0;
	for (const FEntityInstance& Stand : Network.GetEntities())
	{
		if (!Stand.bAlive || !Stand.IsStand() || !Stand.PoseNode.IsSet())
		{
			continue;
		}
		const FGuidelineNode* Pose = Network.GetGuidelineNode(Stand.PoseNode);
		if (Pose == nullptr)
		{
			continue;
		}

		// ONE TURN-OFF PER LEAD-IN: a taxi-through stand has two (FAnchorLink::Gather's forward
		// ray), and each is signed with the stand's one number.
		for (const FGuidelineEdgeId LeadId : Pose->Incident)
		{
			const FGuidelineEdge* Lead = Network.GetGuidelineEdge(LeadId);
			if (Lead == nullptr || !IsAircraftLink(*Lead))
			{
				continue;
			}
			const FGuidelineNodeId LeadEnd = Lead->A == Stand.PoseNode ? Lead->B : Lead->A;
			const FGuidelineNode* End = Network.GetGuidelineNode(LeadEnd);
			if (End == nullptr)
			{
				continue;
			}

			// THE SWEEPS are the CURVED links leaving the lead end (Join's Control = Corner). A
			// straight link there is someone else's lead-in welded to the same node, not ours.
			TArray<FGuidelineEdgeId> Sweeps;
			for (const FGuidelineEdgeId Id2 : End->Incident)
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Id2);
				if (Id2 != LeadId && Edge != nullptr && IsAircraftLink(*Edge)
					&& !GuidelineGeom::IsStraight(PositionOf(Network, Edge->A), Edge->Control, PositionOf(Network, Edge->B)))
				{
					Sweeps.Add(Id2);
				}
			}
			const auto TaxiEndOf = [&Network, LeadEnd](FGuidelineEdgeId Sweep)
			{
				const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Sweep);
				return Edge->A == LeadEnd ? Edge->B : Edge->A;
			};

			// THE CORNER: where the lead-in's line meets the taxiway centreline - every sweep's
			// control (Join sets Control = Corner on both), or with no sweep (no room) the lead end,
			// which IS the taxiway node. Which sweep is read makes no difference any more: the sign
			// sits on the lead-in's axis, not on a sweep (user 2026-09-29), so no side is chosen.
			FGuidelineNodeId TurnNode = LeadEnd;
			FVector2D Corner = End->Position;
			if (Sweeps.Num() > 0)
			{
				TurnNode = TaxiEndOf(Sweeps[0]);
				Corner = Network.GetGuidelineEdge(Sweeps[0])->Control;
			}
			const FGuidelineEdge* Taxiway = TaxiwayEdgeAt(Network, TurnNode);
			if (Taxiway == nullptr)
			{
				// A lead-in to a hand-drawn line, a turn path or a runway: no taxiway pavement to
				// sign. The stand keeps its number; it is shown in the inspector regardless.
				continue;
			}
			++C.TurnOffs;
			++Painted;

			// THE LEAD-IN, WHERE PAVED - every sweep and the lead itself, from the taxiway end
			// inward. On a close taxiway the lead end can sit on the pavement.
			for (const FGuidelineEdgeId Sweep : Sweeps)
			{
				PaintPaved(Out, Z, SamplesFrom(Network, Sweep, TaxiEndOf(Sweep)), Pavement, Id, C);
			}
			PaintPaved(Out, Z, SamplesFrom(Network, LeadId, LeadEnd), Pavement, Id, C);

			// THE SIGN, ON THE LEAD-IN'S OWN AXIS (user 2026-09-29, samples/standsigns.png): from
			// the corner on the centreline toward the stand, a short straight arrow just clear of
			// the centreline paint, between two black boxes carrying the number. It must fit on the
			// pavement: the axis reaches the pavement edge HalfWidth / |In . Normal| from the corner.
			const FVector2D In = (Pose->Position - Corner).GetSafeNormal();
			if (In.IsNearlyZero())
			{
				continue;
			}
			const FVector2D TA = PositionOf(Network, Taxiway->A);
			const FVector2D TB = PositionOf(Network, Taxiway->B);
			const FVector2D Tangent = GuidelineGeom::Tangent(TA, Taxiway->Control, TB, Taxiway->B == TurnNode ? 1.0 : 0.0);
			const FVector2D Normal = RoadGeom::PerpCCW(Tangent.GetSafeNormal());
			const FRoadSegment* TaxiSeg = Network.GetSegment(Taxiway->DerivedFrom);
			const URoadProfile* TaxiProfile = TaxiSeg != nullptr ? Network.ProfileFor(*TaxiSeg) : nullptr;
			if (TaxiProfile == nullptr)
			{
				continue;
			}
			const double Reach = TaxiProfile->GetMaxHalfWidth() / FMath::Max(FMath::Abs(FVector2D::DotProduct(In, Normal)), 0.5);
			const double Length = FMath::Min(ArrowLength, Reach - ArrowStartFromCentreline - ArrowEdgeMargin);
			if (Length < ArrowHeadLength)
			{
				// Too narrow a taxiway for the sign: the lead-in is painted, the sign is not.
				continue;
			}
			const FVector2D Tail = Corner + In * ArrowStartFromCentreline;
			const FVector2D Tip = Tail + In * Length;
			Arrow(Out, Z, Tail, Tip, Id);
			++C.Arrows;
			C.ArrowTails.Add(Tail);
			C.ArrowTips.Add(Tip);

			if (Stand.StandNumber <= 0)
			{
				continue;
			}
			const FString Text = FString::FromInt(Stand.StandNumber);
			const FVector2D Mid = (Tail + Tip) * 0.5;
			const FVector2D Side = RoadGeom::PerpCCW(In);
			for (const double Sign : { 1.0, -1.0 })
			{
				C.SignFaces.Add(SignFace(Out, Z, Mid, In, Side * Sign, Text, Id, BlackId));
			}
			++C.Numbers;
			C.NumbersPainted.Add(Stand.StandNumber);
			C.NumberOrigins.Add(Mid);
		}
	}
	return Painted;
}
