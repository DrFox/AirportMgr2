#include "Model/TaxiwayStrip.h"

#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

namespace TaxiwayStrip
{
	namespace
	{
		double PointToSegment(const FVector2D& P, const FVector2D& A, const FVector2D& B)
		{
			const double T = RoadGeom::ClosestPointOnSegment(A, B, P);
			return FVector2D::Distance(P, A + (B - A) * T);
		}

		/**
		 * A quadratic's bounding box: it lies inside the hull of A, Control and B, so theirs
		 * bounds it. The cheap early-out before any sampling (final review 6).
		 */
		FBox2D HullBox(const FVector2D& A, const FVector2D& Control, const FVector2D& B, double Pad)
		{
			FBox2D Box(ForceInit);
			Box += A;
			Box += Control;
			Box += B;
			return Box.ExpandBy(Pad);
		}

		FBox2D BoxOf(TConstArrayView<FVector2D> Points)
		{
			FBox2D Box(ForceInit);
			for (const FVector2D& P : Points) { Box += P; }
			return Box;
		}

		/** Least distance between two segments: zero if they cross, else the least of the four
		 *  end-to-segment distances (exact for 2D segments that do not cross). */
		double SegmentToSegment(const FVector2D& A0, const FVector2D& A1, const FVector2D& B0, const FVector2D& B1)
		{
			const auto Side = [](const FVector2D& O, const FVector2D& D, const FVector2D& P)
			{
				return FVector2D::CrossProduct(D - O, P - O);
			};
			if (Side(A0, A1, B0) * Side(A0, A1, B1) < 0.0 && Side(B0, B1, A0) * Side(B0, B1, A1) < 0.0)
			{
				return 0.0;
			}
			return FMath::Min(FMath::Min(PointToSegment(A0, B0, B1), PointToSegment(A1, B0, B1)),
				FMath::Min(PointToSegment(B0, A0, A1), PointToSegment(B1, A0, A1)));
		}
	}

	TArray<FVector2D> FootprintOf(const FSegmentShape& Shape)
	{
		constexpr int32 Samples = GuidelineGeom::DefaultSamples;
		TArray<FVector2D> Centre, Normal;
		Centre.Reserve(Samples + 1);
		Normal.Reserve(Samples + 1);
		for (int32 S = 0; S <= Samples; ++S)
		{
			const double T = static_cast<double>(S) / Samples;
			Centre.Add(GuidelineGeom::Eval(Shape.A, Shape.Control, Shape.B, T));
			Normal.Add(RoadGeom::PerpCCW(GuidelineGeom::Tangent(Shape.A, Shape.Control, Shape.B, T)));
		}

		// THE CLOCKWISE-NORMAL EDGE FORWARD, THE COUNTER-CLOCKWISE ONE BACK: travelling +X with
		// PerpCCW = +Y, (0,-w) -> (L,-w) -> (L,+w) -> (0,+w) is positive shoelace area. The
		// other order is clockwise, and a PolygonArea sign test downstream would read it inside out.
		TArray<FVector2D> Out;
		Out.Reserve(2 * (Samples + 1));
		for (int32 S = 0; S <= Samples; ++S)
		{
			Out.Add(Centre[S] - Normal[S] * Shape.HalfWidth);
		}
		for (int32 S = Samples; S >= 0; --S)
		{
			Out.Add(Centre[S] + Normal[S] * Shape.HalfWidth);
		}
		return Out;
	}

	bool ShapeOf(const URoadNetwork& Network, FRoadSegmentId Id, FSegmentShape& Out)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		// THROUGH ProfileFor - see IsAircraftOnly.
		const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
		FVector2D A, B;
		if (Profile == nullptr || !Network.SegmentEnds(Id, A, B))
		{
			return false;
		}
		Out.A = A;
		Out.Control = Segment->Control;
		Out.B = B;
		Out.HalfWidth = Profile->GetMaxHalfWidth();
		return true;
	}

	bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		// THROUGH ProfileFor, the one accessor that repairs a reloaded segment's null Profile - read
		// raw, a saved map lost every strip laid on the fallback profile (found 2026-09-29).
		const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
		if (Profile == nullptr)
		{
			return false;
		}

		// BOTH HALVES, because either alone admits the wrong ground. An aircraft line alone
		// would admit a mixed cross-section a truck also drives (a stand opening onto a
		// service road is exactly what the stand tool refuses); no truck line alone would
		// admit a profile with no lines at all, which nothing can taxi on.
		bool bAircraft = false;
		for (const FProfileGuideline& Guideline : Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return false;
			}
			bAircraft |= Guideline.Class == ETraversalClass::Aircraft;
		}
		return bAircraft;
	}

	bool HasStrip(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		return IsAircraftOnly(Network, Id) && !Network.IsRunwaySegment(Id);
	}

	double StripWidthOf(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		if (!HasStrip(Network, Id))
		{
			return 0.0;
		}
		// THROUGH ProfileFor - see IsAircraftOnly; HasStrip has already proved it non-null.
		return IcaoCode::TaxiwayStripForWidth(Network.ProfileFor(*Network.GetSegment(Id))->GetTotalWidth());
	}

	TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint,
		TConstArrayView<FRoadSegmentId> Exempt)
	{
		TOptional<FIntrusion> Worst;
		if (Footprint.Num() < 3)
		{
			return Worst;
		}

		// EVERY LIVE SEGMENT, LINEARLY. Callers are a placement readout (once a frame) and stand
		// admission (once per candidate per plan). N was 34 on M_Test, 2026-09-28 (its
		// "LogRoadMesh: Rebuilt" line), times 16 samples times a stand's four edges - a few
		// thousand segment tests per call. A spatial index is the day a map runs to thousands.
		const FBox2D FootprintBox = BoxOf(Footprint);
		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Id.IsSet() || Exempt.Contains(Id) || !HasStrip(Network, Id))
			{
				continue;
			}
			const FRoadSegment& Segment = Segments[Index];
			// THROUGH ProfileFor - see IsAircraftOnly; HasStrip has already proved it non-null.
			const URoadProfile* Profile = Network.ProfileFor(Segment);
			FVector2D A, B;
			if (!Network.SegmentEnds(Id, A, B))
			{
				continue;
			}

			// FAR AWAY, CHEAPLY: the curve's hull box grown by its whole reach misses the
			// footprint's box, so nothing below could find it nearer (final review 6).
			const double Reach = Profile->GetMaxHalfWidth() + IcaoCode::TaxiwayStripForWidth(Profile->GetTotalWidth());
			if (!HullBox(A, Segment.Control, B, Reach + ToleranceUu).Intersect(FootprintBox))
			{
				continue;
			}

			// THE CURVE, SAMPLED - not the chord A-B, which on a bend sits a whole sagitta away
			// from the pavement the wing actually follows. GuidelineGeom::Eval is the one
			// quadratic evaluator; DefaultSamples is its own chord-error budget. Inline storage:
			// no allocation per segment per call.
			TArray<FVector2D, TInlineAllocator<GuidelineGeom::DefaultSamples + 1>> Centre;
			for (int32 S = 0; S <= GuidelineGeom::DefaultSamples; ++S)
			{
				Centre.Add(GuidelineGeom::Eval(A, Segment.Control, B,
					static_cast<double>(S) / GuidelineGeom::DefaultSamples));
			}

			double Nearest = DBL_MAX;
			for (const FVector2D& P : Centre)
			{
				if (RoadGeom::PointInPolygon(Footprint, P)) { Nearest = 0.0; break; }
			}
			for (int32 C = 0; C + 1 < Centre.Num() && Nearest > 0.0; ++C)
			{
				for (int32 E = 0; E < Footprint.Num(); ++E)
				{
					Nearest = FMath::Min(Nearest, SegmentToSegment(Centre[C], Centre[C + 1],
						Footprint[E], Footprint[(E + 1) % Footprint.Num()]));
				}
			}

			// THE WIDER HALF, so an asymmetric profile is judged on its generous side rather
			// than leaving a sliver on the narrow one uncounted.
			const double Pavement = Profile->GetTotalWidth();
			const double Strip = IcaoCode::TaxiwayStripForWidth(Pavement);
			const double Depth = Profile->GetMaxHalfWidth() + Strip - Nearest;
			if (Depth > ToleranceUu && (!Worst.IsSet() || Depth > Worst->Depth))
			{
				FIntrusion Found;
				Found.Taxiway = Id;
				Found.Letter = IcaoCode::TaxiwayLetterForWidth(Pavement);
				Found.Required = Strip;
				Found.Depth = Depth;
				Worst = Found;
			}
		}
		return Worst;
	}

	namespace
	{
		double DegreesBetween(const FVector2D& A, const FVector2D& B)
		{
			return FMath::RadiansToDegrees(RoadGeom::AngleBetween(A, B));
		}

		/** The pavement letter of a live strip-bearing segment, for the refusal text. */
		const TCHAR* LetterOf(const URoadNetwork& Network, FRoadSegmentId Id)
		{
			const FRoadSegment* Segment = Network.GetSegment(Id);
			const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
			return IcaoCode::ToLetter(IcaoCode::TaxiwayLetterForWidth(Profile != nullptr ? Profile->GetTotalWidth() : 0.0));
		}

		TArray<FVector2D> CentreOf(const FSegmentShape& Shape)
		{
			TArray<FVector2D> Centre;
			Centre.Reserve(GuidelineGeom::DefaultSamples + 1);
			for (int32 S = 0; S <= GuidelineGeom::DefaultSamples; ++S)
			{
				Centre.Add(GuidelineGeom::Eval(Shape.A, Shape.Control, Shape.B,
					static_cast<double>(S) / GuidelineGeom::DefaultSamples));
			}
			return Centre;
		}

		/**
		 * FootprintOf's polygon as its per-sample quads. PolygonsOverlap is a CONVEX test, and a
		 * curved footprint is not convex: judged whole, a bent taxiway's strip would claim its
		 * own inside-of-the-bend as if it were the hull. Each quad between two samples is convex
		 * for any bend GuidelineGeom's radii allow.
		 */
		TArray<TArray<FVector2D>> QuadsOf(const TArray<FVector2D>& Footprint)
		{
			const int32 N = Footprint.Num() / 2;   // samples along each edge
			TArray<TArray<FVector2D>> Quads;
			Quads.Reserve(N - 1);
			for (int32 K = 0; K + 1 < N; ++K)
			{
				Quads.Add({ Footprint[K], Footprint[K + 1], Footprint[2 * N - 2 - K], Footprint[2 * N - 1 - K] });
			}
			return Quads;
		}

		bool QuadsOverlap(const TArray<TArray<FVector2D>>& A, const TArray<TArray<FVector2D>>& B)
		{
			for (const TArray<FVector2D>& QA : A)
			{
				for (const TArray<FVector2D>& QB : B)
				{
					if (RoadGeom::PolygonsOverlap(QA, QB, ToleranceUu)) { return true; }
				}
			}
			return false;
		}

		/** Does the new centreline cross this segment's centreline, sampled both? */
		bool CentrelinesCross(const TArray<FVector2D>& New, const FSegmentShape& Other)
		{
			const TArray<FVector2D> Theirs = CentreOf(Other);
			for (int32 I = 0; I + 1 < New.Num(); ++I)
			{
				for (int32 J = 0; J + 1 < Theirs.Num(); ++J)
				{
					if (RoadGeom::SegmentsCross(New[I], New[I + 1], Theirs[J], Theirs[J + 1])) { return true; }
				}
			}
			return false;
		}
	}

	bool MeetsAtAllowedAngle(double Degrees)
	{
		return Degrees >= MeetMinDegrees;
	}

	FString MeetingRefusal(const URoadNetwork& Network, FRoadSegmentId Taxiway, double Degrees)
	{
		return FString::Printf(
			TEXT("meets a Code %s taxiway at %d degrees, inside its clearance strip - join within 30 degrees of square"),
			LetterOf(Network, Taxiway), FMath::RoundToInt(Degrees));
	}

	FStripVerdict JudgeExisting(const URoadNetwork& Network, FRoadSegmentId Id, TConstArrayView<FRoadSegmentId> Ignore)
	{
		FSegmentShape Shape;
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || !ShapeOf(Network, Id, Shape))
		{
			return FStripVerdict();
		}
		FSegmentEnd AtA;
		AtA.Node = Segment->A;
		AtA.At = Shape.A;
		FSegmentEnd AtB;
		AtB.Node = Segment->B;
		AtB.At = Shape.B;
		TArray<FRoadSegmentId> Skip(Ignore.GetData(), Ignore.Num());
		Skip.AddUnique(Id);
		return JudgeSegment(Network, Shape, HasStrip(Network, Id), AtA, AtB, Skip);
	}

	FStripVerdict JudgeSegment(const URoadNetwork& Network, const FSegmentShape& Shape,
		bool bIsTaxiway, const FSegmentEnd& AtA, const FSegmentEnd& AtB,
		TConstArrayView<FRoadSegmentId> Ignore)
	{
		FStripVerdict Verdict;

		// 1. WHAT IT MEETS. Exempt = the strip-bearing taxiways it joins at an allowed angle,
		// whose strip it may therefore cross (spec: "network that MEETS the taxiway may pass
		// through its strip"). Met = every segment at either end, strip or not - a new taxiway's
		// own strip cannot "contain" the road it is being joined to.
		TArray<FRoadSegmentId> Exempt(Ignore.GetData(), Ignore.Num());
		TArray<FRoadSegmentId> Met;
		const auto Meet = [&](const FSegmentEnd& End, const FVector2D& Out) -> bool
		{
			if (End.Node.IsSet())
			{
				const FRoadNode* Node = Network.GetNode(End.Node);
				if (Node == nullptr) { return true; }
				for (const FRoadSegmentId Arm : Node->Incident)
				{
					Met.AddUnique(Arm);
					if (Ignore.Contains(Arm) || !HasStrip(Network, Arm)) { continue; }
					// PER ARM (Review Focus 2): at a node where two taxiways join, a road square
					// to one can be 15 degrees off the other and run along ITS strip. At least
					// MeetMinDegrees from every arm - see its header for why there is no
					// separate straight-on band (a dead end's bend heads away from its arm).
					const double Deg = DegreesBetween(Out, Network.GetOutgoingTangent(Arm, End.Node));
					if (MeetsAtAllowedAngle(Deg))
					{
						Exempt.AddUnique(Arm);
						continue;
					}
					Verdict.bRefused = true;
					Verdict.Text = MeetingRefusal(Network, Arm, Deg);
					return false;
				}
			}
			else if (End.Segment.IsSet())
			{
				Met.AddUnique(End.Segment);
				FSegmentShape Theirs;
				if (Ignore.Contains(End.Segment) || !HasStrip(Network, End.Segment) || !ShapeOf(Network, End.Segment, Theirs))
				{
					return true;
				}
				// THE TWO ARMS THE SPLIT WILL MAKE, not the curve's tangent (final review 4):
				// URoadNetwork::SplitSegment replaces the segment with two STRAIGHT halves meeting
				// at At, so the commit's ConnectNodes judges those chords - on a bend ~30 degrees
				// off the tangent - and judging the tangent here let the preview approve a join
				// the commit then refused after splitting. On a straight segment the two agree.
				const double Deg = FMath::Min(DegreesBetween(Out, Theirs.A - End.At), DegreesBetween(Out, Theirs.B - End.At));
				if (MeetsAtAllowedAngle(Deg))
				{
					Exempt.AddUnique(End.Segment);
					return true;
				}
				Verdict.bRefused = true;
				Verdict.Text = MeetingRefusal(Network, End.Segment, Deg);
				return false;
			}
			return true;
		};
		// Each end's direction pointing AWAY from that end along the new segment - the same sense
		// GetOutgoingTangent gives the existing arm, so square is 90 and straight on is 180.
		if (!Meet(AtA, GuidelineGeom::Tangent(Shape.A, Shape.Control, Shape.B, 0.0))
			|| !Meet(AtB, -GuidelineGeom::Tangent(Shape.A, Shape.Control, Shape.B, 1.0)))
		{
			return Verdict;
		}

		// 1b. A TAXIWAY IS A CHAIN, NOT A SEGMENT. The spec exempts what "meets the TAXIWAY",
		// and one taxiway is often several pieces - every split and snapped junction adds one -
		// so exempting only the met pieces refused a square join 10 m from the next piece's end
		// (found 2026-09-29: a moved T-junction refused by its own neighbour piece). Walk on
		// from every exempt piece through each node holding exactly two strip-bearing arms that
		// run on within 30 degrees of straight. NOT round a corner: past one the next leg is a
		// different line, and a road square to the first leg runs alongside the second.
		// Seeded from Ignore too - a replaced piece (a moved node's own arm, a heal's stub) is
		// the same taxiway as the one it becomes.
		//
		// ONLY NEAR THE JOIN (final review 1): a piece is walked onto only while some part of its
		// centreline lies within its own reach (half-width + strip) plus the new segment's half of
		// either end. A ring of gentle bends passes the per-node test all the way round, and an
		// unbounded walk exempted a road from the ring's FAR side, which it crosses unjoined.
		const auto NearTheJoin = [&Network, &Shape, &AtA, &AtB](FRoadSegmentId Id)
		{
			FSegmentShape Theirs;
			const FRoadSegment* Segment = Network.GetSegment(Id);
			const URoadProfile* Profile = Segment != nullptr ? Network.ProfileFor(*Segment) : nullptr;
			if (Profile == nullptr || !ShapeOf(Network, Id, Theirs)) { return false; }
			const double Reach = Theirs.HalfWidth + IcaoCode::TaxiwayStripForWidth(Profile->GetTotalWidth()) + Shape.HalfWidth;
			const TArray<FVector2D> Centre = CentreOf(Theirs);
			for (const FVector2D& End : { AtA.At, AtB.At })
			{
				for (int32 C = 0; C + 1 < Centre.Num(); ++C)
				{
					if (PointToSegment(End, Centre[C], Centre[C + 1]) <= Reach) { return true; }
				}
			}
			return false;
		};
		for (int32 Seed = 0; Seed < Exempt.Num(); ++Seed)
		{
			const FRoadSegmentId From = Exempt[Seed];
			const FRoadSegment* Piece = Network.GetSegment(From);
			if (Piece == nullptr || !HasStrip(Network, From)) { continue; }
			for (const FRoadNodeId Through : { Piece->A, Piece->B })
			{
				const FRoadNode* Node = Network.GetNode(Through);
				if (Node == nullptr) { continue; }
				FRoadSegmentId Next;
				int32 Arms = 0;
				for (const FRoadSegmentId Arm : Node->Incident)
				{
					if (!HasStrip(Network, Arm)) { continue; }
					++Arms;
					if (Arm != From) { Next = Arm; }
				}
				if (Arms != 2 || !Next.IsSet() || Exempt.Contains(Next)) { continue; }
				if (DegreesBetween(Network.GetOutgoingTangent(From, Through), Network.GetOutgoingTangent(Next, Through))
					>= ChainStraightMinDegrees && NearTheJoin(Next))
				{
					Exempt.Add(Next);   // walked on from in its own turn, by this loop
				}
			}
		}

		// 2. ITS PAVEMENT in any other taxiway's strip.
		const TArray<FVector2D> Footprint = FootprintOf(Shape);
		if (const TOptional<FIntrusion> In = WorstIntrusion(Network, Footprint, Exempt))
		{
			Verdict.bRefused = true;
			Verdict.Depth = In->Depth;
			FSegmentShape Theirs;
			if (ShapeOf(Network, In->Taxiway, Theirs) && CentrelinesCross(CentreOf(Shape), Theirs))
			{
				Verdict.Text = FString::Printf(
					TEXT("crosses a Code %s taxiway without a junction - end it on the taxiway, square to it"),
					IcaoCode::ToLetter(In->Letter));
			}
			else
			{
				Verdict.Text = FString::Printf(
					TEXT("inside a Code %s taxiway's clearance strip by %.1f m - end it on the taxiway at a junction, or move it %.1f m away"),
					IcaoCode::ToLetter(In->Letter), In->Depth / 100.0, In->Depth / 100.0);
			}
			return Verdict;
		}

		if (!bIsTaxiway)
		{
			return Verdict;
		}

		// 3. A NEW TAXIWAY'S OWN STRIP, looking back (plan ruling 5): one-way checking would lay
		// an F 40 m from a B because the B's 9 m strip is clear, while the F's 34.5 m one
		// swallows the B's pavement.
		FSegmentShape StripShape = Shape;
		StripShape.HalfWidth = Shape.HalfWidth + IcaoCode::TaxiwayStripForWidth(2.0 * Shape.HalfWidth);
		const TArray<TArray<FVector2D>> StripQuads = QuadsOf(FootprintOf(StripShape));
		const FBox2D StripBox = HullBox(StripShape.A, StripShape.Control, StripShape.B, StripShape.HalfWidth + ToleranceUu);

		// EVERY LIVE ENTITY AND SEGMENT, LINEARLY - WorstIntrusion's own reasoning and N (34
		// segments on M_Test, 2026-09-28), times 16 x 16 quad pairs for any that survive the
		// box test. PAID TWICE A FRAME by the taxiway tool (FRoadChainingState::BuildPreview's
		// readout and FRoadDrawTool::Tick's ghost colour both ask WhySegmentRefused, 2026-09-29),
		// so the box early-outs below are what keep a far entity or segment to four compares.
		const TArray<FEntityInstance>& Entities = Network.GetEntities();
		for (int32 Index = 0; Index < Entities.Num(); ++Index)
		{
			const FEntityInstance& Entity = Entities[Index];
			// STANDS AND DEPOTS ALIKE - both are ground a taxiing wing must not sweep; named apart
			// below because the player fixes them differently. A plotted outline is convex (a
			// stand box, a depot rectangle) - PolygonsOverlap's contract.
			if (!Entity.bAlive || !(Entity.IsStand() || Entity.IsDepot()) || !Entity.IsPlotted()) { continue; }
			if (!BoxOf(Entity.Outline).Intersect(StripBox)) { continue; }
			for (const TArray<FVector2D>& Quad : StripQuads)
			{
				if (RoadGeom::PolygonsOverlap(Quad, Entity.Outline, ToleranceUu))
				{
					Verdict.bRefused = true;
					Verdict.Text = Entity.IsStand()
						? FString::Printf(TEXT("its clearance strip would contain stand %d"), Index)
						: FString(TEXT("its clearance strip would contain a fuel depot"));
					return Verdict;
				}
			}
		}

		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			// RUNWAYS ARE OUT OF SCOPE (plan ruling 2) - their own strip rules, not this one.
			if (!Id.IsSet() || Exempt.Contains(Id) || Met.Contains(Id) || Network.IsRunwaySegment(Id)) { continue; }
			FSegmentShape Theirs;
			if (!ShapeOf(Network, Id, Theirs)) { continue; }
			if (!HullBox(Theirs.A, Theirs.Control, Theirs.B, Theirs.HalfWidth).Intersect(StripBox)) { continue; }
			if (QuadsOverlap(StripQuads, QuadsOf(FootprintOf(Theirs))))
			{
				Verdict.bRefused = true;
				Verdict.Text = IsAircraftOnly(Network, Id)
					? FString(TEXT("its clearance strip would contain a taxiway"))
					: FString(TEXT("its clearance strip would contain a service road"));
				return Verdict;
			}
		}
		return Verdict;
	}
}
