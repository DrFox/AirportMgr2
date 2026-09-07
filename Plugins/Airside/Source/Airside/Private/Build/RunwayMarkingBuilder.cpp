#include "Build/RunwayMarkingBuilder.h"

#include "Build/MarkingGlyphs.h"
#include "Build/MarkingQuads.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayFacts.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

namespace
{
	/**
	 * One runway's frame: Origin at the threshold, Along down the strip, Across to the
	 * reader's RIGHT as a pilot on approach sees it. Unreal is left-handed, so with X
	 * forward and Z up, +Y is to the right - and (-Along.Y, Along.X) is +Y for Along = +X.
	 * The frame is what makes every marking below a rectangle in [along] x [across].
	 */
	struct FRunwayFrame
	{
		FVector2D Origin;
		FVector2D Along;
		FVector2D Across;
		double Length = 0.0;
		double HalfWidth = 0.0;
	};

	/** The far end's frame: the same strip walked the other way. */
	FRunwayFrame Reversed(const FRunwayFrame& Frame)
	{
		FRunwayFrame Out = Frame;
		Out.Origin = Frame.Origin + Frame.Along * Frame.Length;
		Out.Along = -Frame.Along;
		Out.Across = -Frame.Across;
		return Out;
	}

	void Rect(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame,
		double Along0, double Along1, double Across0, double Across1)
	{
		MarkingQuads::AddRect(Out, Z, Frame.Origin, Frame.Along, Frame.Across, Along0, Along1, Across0, Across1);
	}

	/** The stripes of one threshold, from Frame's end. */
	int32 ThresholdStripes(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame, int32 Count)
	{
		using B = FRunwayMarkingBuilder;
		// Symmetric about the centreline, the outermost stripe's outer edge StripeInset
		// inside the pavement edge (ICAO 5.2.4: within 3 m of the edge), evenly spaced
		// between. Even spacing is a simplification of the standard's wider central gap;
		// what the standard fixes is the count, and the count is what is measured.
		const double Reach = Frame.HalfWidth - B::StripeInset;
		const double Gap = Count > 1 ? (2.0 * Reach - Count * B::StripeWidth) / (Count - 1) : 0.0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Outer = -Reach + Index * (B::StripeWidth + Gap);
			Rect(Out, Z, Frame, B::StripeStart, B::StripeStart + B::StripeLength, Outer, Outer + B::StripeWidth);
		}
		return Count;
	}

	/**
	 * A designation at Frame's end: Text's glyphs, DigitHeight tall, centred on the
	 * centreline, their feet DigitGapAfterStripes past the stripes. Returns strokes painted.
	 */
	int32 Designation(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame, const FString& Text)
	{
		using B = FRunwayMarkingBuilder;
		const double Scale = B::DigitHeight / MarkingGlyphs::CellHeight;   // one cell unit in uu
		const double Foot = B::StripeStart + B::StripeLength + B::DigitGapAfterStripes;
		const double TotalWidth = Text.Len() * Scale + (Text.Len() - 1) * B::DigitSpacing * Scale;
		double Left = -TotalWidth * 0.5;
		int32 Strokes = 0;
		for (const TCHAR Character : Text)
		{
			for (const TArray<FVector2D>& Line : MarkingGlyphs::Strokes(Character))
			{
				for (int32 Index = 0; Index + 1 < Line.Num(); ++Index)
				{
					// Each polyline span is a quad StrokeWidth wide, extended half a
					// stroke at both ends so consecutive spans meet square at a corner
					// rather than leaving a notch - which is also what makes a glyph fill
					// its cell exactly (see MarkingGlyphs).
					const FVector2D A = Line[Index];
					const FVector2D C = Line[Index + 1];
					const FVector2D U = (C - A).GetSafeNormal();
					const FVector2D N(-U.Y, U.X);
					constexpr double H = MarkingGlyphs::StrokeWidth * 0.5;
					const FVector2D A2 = A - U * H;
					const FVector2D C2 = C + U * H;
					auto World = [&](const FVector2D& Cell)
					{
						return Frame.Origin + Frame.Along * (Foot + Cell.Y * Scale)
							+ Frame.Across * ((Left + Cell.X * Scale));
					};
					MarkingQuads::AddQuad(Out, Z,
						World(A2 - N * H), World(C2 - N * H), World(C2 + N * H), World(A2 + N * H));
					++Strokes;
				}
			}
			Left += Scale * (1.0 + B::DigitSpacing);
		}
		return Strokes;
	}

	/** The centreline: dashes between the two designations, centred in that span. */
	int32 Centreline(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame)
	{
		using B = FRunwayMarkingBuilder;
		const double Clear = B::StripeStart + B::StripeLength + B::DigitGapAfterStripes + B::DigitHeight + B::DashGapAfterDigits;
		const double Available = Frame.Length - 2.0 * Clear;
		const double Period = B::DashOn + B::DashOff;
		const int32 Count = Available > 0.0 ? FMath::FloorToInt32(Available / Period) : 0;
		// Centred, so the run ends the same distance short of each designation: a
		// centreline that started flush at one end and ran out short of the other would
		// tell a pilot which end the builder started from, which is not a fact about a runway.
		const double Run = Count * Period - B::DashOff;
		const double Start = Clear + (Available - Run) * 0.5;
		const double Half = B::CentrelineWidth(Frame.HalfWidth * 2.0) * 0.5;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double At = Start + Index * Period;
			Rect(Out, Z, Frame, At, At + B::DashOn, -Half, Half);
		}
		return Count;
	}

	/** Would a marking from At to At + Extent cross the strip's midpoint and meet the far end's? */
	bool Fits(const FRunwayFrame& Frame, double At, double Extent)
	{
		return At + Extent <= Frame.Length * 0.5;
	}

	int32 AimingPoint(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame)
	{
		using B = FRunwayMarkingBuilder;
		const double At = Frame.Length < B::AimingPointShortRunway ? B::AimingPointAtShort : B::AimingPointAt;
		const double Len = Frame.HalfWidth * 2.0 < B::AimingPointNarrowRunway ? B::AimingPointLengthNarrow : B::AimingPointLength;
		if (!Fits(Frame, At, Len))
		{
			return 0;
		}
		const double Inner = B::AimingPointGap * 0.5;
		Rect(Out, Z, Frame, At, At + Len, -Inner - B::AimingPointWidth, -Inner);
		Rect(Out, Z, Frame, At, At + Len, Inner, Inner + B::AimingPointWidth);
		return 2;
	}

	int32 TouchdownZone(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame)
	{
		using B = FRunwayMarkingBuilder;
		int32 Painted = 0;
		for (int32 Pair = 0; Pair < 3; ++Pair)
		{
			const double At = B::TouchdownPairsAt[Pair];
			if (!Fits(Frame, At, B::TouchdownStripeLength))
			{
				// Paint what fits (spec §8): a strip too short for the full set omits the
				// pairs that would run into the far end's, rather than overlapping them.
				continue;
			}
			// Pair + 1 stripes a side, the innermost sharing the aiming point's inner
			// edge and the rest stepping outward, a gap between each.
			for (int32 Stripe = 0; Stripe <= Pair; ++Stripe)
			{
				const double Inner = B::AimingPointGap * 0.5 + Stripe * (B::TouchdownStripeWidth + B::TouchdownStripeGap);
				Rect(Out, Z, Frame, At, At + B::TouchdownStripeLength, -Inner - B::TouchdownStripeWidth, -Inner);
				Rect(Out, Z, Frame, At, At + B::TouchdownStripeLength, Inner, Inner + B::TouchdownStripeWidth);
				Painted += 2;
			}
		}
		return Painted;
	}

	int32 SideStripes(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame)
	{
		using B = FRunwayMarkingBuilder;
		Rect(Out, Z, Frame, 0.0, Frame.Length, -Frame.HalfWidth, -Frame.HalfWidth + B::SideStripeWidth);
		Rect(Out, Z, Frame, 0.0, Frame.Length, Frame.HalfWidth - B::SideStripeWidth, Frame.HalfWidth);
		return 2;
	}

	int32 GrassMarkers(FRoadMeshBuffers& Out, double Z, const FRunwayFrame& Frame)
	{
		using B = FRunwayMarkingBuilder;
		int32 Painted = 0;
		// Markers along both edges every GrassMarkerSpacing from the threshold, the last
		// wherever the spacing lands; then a corner square at each of the four corners.
		const int32 PerEdge = FMath::FloorToInt32(Frame.Length / B::GrassMarkerSpacing) + 1;
		for (int32 Index = 0; Index < PerEdge; ++Index)
		{
			const double At = Index * B::GrassMarkerSpacing;
			for (const double Side : { -1.0, 1.0 })
			{
				const double Edge = Side * Frame.HalfWidth;
				const double In = Edge - Side * B::GrassMarker;
				Rect(Out, Z, Frame, At, At + B::GrassMarker, FMath::Min(Edge, In), FMath::Max(Edge, In));
				++Painted;
			}
		}
		for (const double End : { 0.0, Frame.Length - B::GrassCorner })
		{
			for (const double Side : { -1.0, 1.0 })
			{
				const double Edge = Side * Frame.HalfWidth;
				const double In = Edge - Side * B::GrassCorner;
				Rect(Out, Z, Frame, End, End + B::GrassCorner, FMath::Min(Edge, In), FMath::Max(Edge, In));
				++Painted;
			}
		}
		return Painted;
	}
}

int32 FRunwayMarkingBuilder::ThresholdStripeCount(double TotalWidth)
{
	// ICAO Annex 14 Table 5-1 (5.2.4.5): stripes by runway width. Nearest width wins,
	// as the admission's code table does, so a profile a little off one of the five
	// standard widths is not painted as a different class of runway.
	struct FRow { double Width; int32 Stripes; };
	static const FRow Rows[] = { { 1800.0, 4 }, { 2300.0, 6 }, { 3000.0, 8 }, { 4500.0, 12 }, { 6000.0, 16 } };
	const FRow* Nearest = &Rows[0];
	for (const FRow& Row : Rows)
	{
		if (FMath::Abs(Row.Width - TotalWidth) < FMath::Abs(Nearest->Width - TotalWidth))
		{
			Nearest = &Row;
		}
	}
	return Nearest->Stripes;
}

double FRunwayMarkingBuilder::CentrelineWidth(double TotalWidth)
{
	return TotalWidth >= WideRunway ? CentrelineWidthWide : CentrelineWidthNarrow;
}

int32 FRunwayMarkingBuilder::Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out,
	FRunwayMarkingCensus* Census)
{
	FRunwayMarkingCensus Local;
	FRunwayMarkingCensus& C = Census != nullptr ? *Census : Local;
	C = FRunwayMarkingCensus();

	// Each runway ONCE however many segments its exits have cut it into: the first live
	// runway segment met seeds the chain, and every member of the chain is then done.
	TSet<int32> Visited;
	const TArray<FRoadSegment>& Segments = Network.GetSegments();
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FRoadSegment& Segment = Segments[Index];
		if (!Segment.bAlive || Visited.Contains(Index))
		{
			continue;
		}
		FRoadSegmentId Seed;
		Seed.Index = Index;
		Seed.Generation = Segment.Generation;
		if (!Network.IsRunwaySegment(Seed))
		{
			continue;
		}
		for (const FRoadSegmentId& Member : Network.RunwayChain(Seed))
		{
			Visited.Add(Member.Index);
		}

		// The frame from the extent query, asked from the seed's own A node so the point
		// is certainly on the strip; the threshold it reports is the end nearest that
		// node and the direction runs away from it. Which end that is does not matter:
		// every marking is painted from both ends, or neither.
		FRunwayFrame Frame;
		const FRoadNode* A = Network.GetNode(Segment.A);
		if (A == nullptr
			|| !Network.RunwayExtentAt(A->Position, Frame.Origin, Frame.Along, Frame.Length)
			|| Frame.Length <= 0.0)
		{
			continue;
		}
		Frame.Across = FVector2D(-Frame.Along.Y, Frame.Along.X);
		const URoadProfile* Profile = Network.ProfileFor(Segment);
		Frame.HalfWidth = Profile != nullptr ? Profile->GetTotalWidth() * 0.5 : 0.0;
		if (Frame.HalfWidth <= 0.0)
		{
			continue;
		}
		const FRunwayFrame Far = Reversed(Frame);
		const FRunwayFacts Facts = Network.RunwayFactsFor(Seed);
		++C.Runways;

		if (Facts.Surface == ERunwaySurface::Grass)
		{
			// No pavement, no pavement markings: a grass strip carries edge markers only,
			// and its designator lives on a board, not on the ground.
			C.GrassMarkers += GrassMarkers(Out, Z, Frame);
			continue;
		}

		const int32 Stripes = ThresholdStripeCount(Frame.HalfWidth * 2.0);
		const int32 Near = RunwayDesignator::Designate(Frame.Along);
		const FRunwayFrame* Ends[] = { &Frame, &Far };
		for (const FRunwayFrame* End : Ends)
		{
			C.ThresholdStripes += ThresholdStripes(Out, Z, *End, Stripes);
			C.DesignatorStrokes += Designation(Out, Z, *End,
				RunwayDesignator::ToText(End == &Frame ? Near : RunwayDesignator::Reciprocal(Near)));
			if (Facts.Approach >= ERunwayApproach::NonPrecision)
			{
				C.AimingPointBars += AimingPoint(Out, Z, *End);
			}
			if (Facts.Approach == ERunwayApproach::Precision)
			{
				C.TouchdownStripes += TouchdownZone(Out, Z, *End);
			}
		}
		C.CentrelineDashes += Centreline(Out, Z, Frame);
		if (Facts.Approach == ERunwayApproach::Precision)
		{
			C.SideStripes += SideStripes(Out, Z, Frame);
		}
	}
	return C.Runways;
}
