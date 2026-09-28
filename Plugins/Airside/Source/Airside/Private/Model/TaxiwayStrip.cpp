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

	bool IsAircraftOnly(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || Segment->Profile == nullptr)
		{
			return false;
		}

		// BOTH HALVES, because either alone admits the wrong ground. An aircraft line alone
		// would admit a mixed cross-section a truck also drives (a stand opening onto a
		// service road is exactly what the stand tool refuses); no truck line alone would
		// admit a profile with no lines at all, which nothing can taxi on.
		bool bAircraft = false;
		for (const FProfileGuideline& Guideline : Segment->Profile->Guidelines)
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
		return IcaoCode::TaxiwayStripForWidth(Network.GetSegment(Id)->Profile->GetTotalWidth());
	}

	TOptional<FIntrusion> WorstIntrusion(const URoadNetwork& Network, TConstArrayView<FVector2D> Footprint)
	{
		TOptional<FIntrusion> Worst;
		if (Footprint.Num() < 3)
		{
			return Worst;
		}

		// EVERY LIVE SEGMENT, LINEARLY. Callers are a placement readout (once a frame) and stand
		// admission (once per candidate per plan).
		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Id.IsSet() || !HasStrip(Network, Id))
			{
				continue;
			}
			const FRoadSegment& Segment = Segments[Index];
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
			const double Pavement = Segment.Profile->GetTotalWidth();
			const double Strip = IcaoCode::TaxiwayStripForWidth(Pavement);
			const double Depth = Segment.Profile->GetMaxHalfWidth() + Strip - Nearest;
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
