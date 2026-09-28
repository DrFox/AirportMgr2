#include "Model/TaxiwayStrip.h"

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

	TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint)
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
		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Id.IsSet() || !HasStrip(Network, Id))
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

			// THE CURVE, SAMPLED - not the chord A-B, which on a bend sits a whole sagitta away
			// from the pavement the wing actually follows. GuidelineGeom::Eval is the one
			// quadratic evaluator; DefaultSamples is its own chord-error budget.
			TArray<FVector2D> Centre;
			Centre.Reserve(GuidelineGeom::DefaultSamples + 1);
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
}
