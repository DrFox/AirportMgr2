#include "Solve/GuidelineGeom.h"

#include "Solve/RoadGeom.h"

namespace GuidelineGeom
{
	FVector2D Eval(const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T)
	{
		const double U = 1.0 - T;
		return (U * U) * A + (2.0 * U * T) * Control + (T * T) * B;
	}

	FVector2D Tangent(const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T)
	{
		// d/dt of (1-t)^2 A + 2(1-t)t C + t^2 B, with the constant factor of 2 dropped -
		// only the direction is wanted.
		const double Clamped = FMath::Clamp(T, 0.0, 1.0);
		const FVector2D Derivative = (Control - A) * (1.0 - Clamped) + (B - Control) * Clamped;

		const FVector2D Unit = Derivative.GetSafeNormal();
		if (!Unit.IsNearlyZero())
		{
			return Unit;
		}

		// Degenerate: the control point sits on an end, so the derivative vanishes there.
		// The chord is the only direction left that means anything.
		const FVector2D Chord = (B - A).GetSafeNormal();
		return Chord.IsNearlyZero() ? FVector2D(1.0, 0.0) : Chord;
	}

	bool IsStraight(const FVector2D& A, const FVector2D& Control, const FVector2D& B)
	{
		// Scaled to the guideline's own length: a fixed tolerance is either meaningless on
		// a 3km runway or wrong on a 2m link. UE_DOUBLE_KINDA_SMALL_NUMBER of the span is
		// far below anything the builder produces deliberately.
		const FVector2D Span = B - A;
		const double Scale = FMath::Max(Span.Size(), 1.0);
		return FVector2D::Distance(Control, (A + B) * 0.5) <= Scale * 1e-6;
	}

	void Sample(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B,
		TArray<FVector2D>& OutPoints, int32 Samples)
	{
		// A straight guideline needs no interior points, and giving it some would make the
		// common case cost fifteen times more to walk for geometry identical to its own
		// two ends.
		if (IsStraight(A, Control, B))
		{
			OutPoints.Add(A);
			OutPoints.Add(B);
			return;
		}

		const int32 Count = FMath::Max(Samples, 2);
		OutPoints.Reserve(OutPoints.Num() + Count);

		for (int32 Step = 0; Step < Count; ++Step)
		{
			// Endpoints are produced by evaluating at exactly 0 and 1 rather than being
			// substituted in, which for a quadratic returns A and B exactly - so a route
			// welded from consecutive edges meets at one point, not two a hair apart.
			OutPoints.Add(Eval(A, Control, B, static_cast<double>(Step) / (Count - 1)));
		}
	}

	double Length(const FVector2D& A, const FVector2D& Control, const FVector2D& B, int32 Samples)
	{
		TArray<FVector2D> Points;
		Sample(A, Control, B, Points, Samples);
		return PolylineLength(Points);
	}

	void Split(
		const FVector2D& A, const FVector2D& Control, const FVector2D& B, double T,
		FVector2D& OutMid, FVector2D& OutControlLeft, FVector2D& OutControlRight)
	{
		const double Clamped = FMath::Clamp(T, 0.0, 1.0);

		OutControlLeft = FMath::Lerp(A, Control, Clamped);
		OutControlRight = FMath::Lerp(Control, B, Clamped);
		OutMid = FMath::Lerp(OutControlLeft, OutControlRight, Clamped);
	}

	double ParamAtSample(int32 Index, double Fraction, int32 Count)
	{
		if (Count < 2)
		{
			return 0.0;
		}
		return FMath::Clamp((Index + Fraction) / (Count - 1), 0.0, 1.0);
	}

	double PolylineLength(const TArray<FVector2D>& Points)
	{
		double Total = 0.0;
		for (int32 At = 1; At < Points.Num(); ++At)
		{
			Total += FVector2D::Distance(Points[At - 1], Points[At]);
		}
		return Total;
	}

	double NearestOnPolyline(const TArray<FVector2D>& Points,
		const FVector2D& Query, int32& OutIndex, double& OutFraction)
	{
		OutIndex = 0;
		OutFraction = 0.0;
		if (Points.Num() < 2)
		{
			return TNumericLimits<double>::Max();
		}

		double Best = TNumericLimits<double>::Max();
		for (int32 At = 1; At < Points.Num(); ++At)
		{
			// RoadGeom's clamped parameter, so the answer is on the SEGMENT rather than on
			// its infinite line: a query past the end is answered by the end, which is what
			// a link measuring against a finite guideline needs.
			const double T = RoadGeom::ClosestPointOnSegment(Points[At - 1], Points[At], Query);
			const double Distance =
				FVector2D::Distance(FMath::Lerp(Points[At - 1], Points[At], T), Query);
			if (Distance < Best)
			{
				Best = Distance;
				OutIndex = At - 1;
				OutFraction = T;
			}
		}
		return Best;
	}

	double NearestBetweenPolylines(
		const TArray<FVector2D>& A, const TArray<FVector2D>& B,
		int32& OutAIndex, double& OutAFraction, int32& OutBIndex, double& OutBFraction)
	{
		OutAIndex = 0; OutAFraction = 0.0;
		OutBIndex = 0; OutBFraction = 0.0;
		if (A.Num() < 2 || B.Num() < 2)
		{
			return TNumericLimits<double>::Max();
		}

		// The span a VERTEX sits on, expressed the way the outputs are: the last vertex is
		// the END of the last span, never the start of a span that does not exist.
		auto SpanOf = [](int32 Vertex, int32 Count, int32& OutSpan, double& OutFractionAt)
		{
			OutSpan = FMath::Clamp(Vertex, 0, Count - 2);
			OutFractionAt = Vertex >= Count - 1 ? 1.0 : 0.0;
		};

		// EVERY CANDIDATE, KEPT, because the answer is decided in two passes: the first finds
		// how near the lines come, and the second finds the MIDDLE of everything that came
		// that near. A single strict-less-than pass cannot do both - it answers with whichever
		// tied member it happened to examine first.
		struct FCandidate
		{
			double Distance;
			double ParamOnA;
		};
		TArray<FCandidate, TInlineAllocator<32>> Candidates;
		Candidates.Reserve(A.Num() + B.Num());

		for (int32 At = 0; At < A.Num(); ++At)
		{
			int32 Index = 0;
			double Fraction = 0.0;
			const double Distance = NearestOnPolyline(B, A[At], Index, Fraction);

			int32 SpanOnA = 0;
			double FractionOnA = 0.0;
			SpanOf(At, A.Num(), SpanOnA, FractionOnA);
			Candidates.Add({ Distance, ParamAtSample(SpanOnA, FractionOnA, A.Num()) });
		}

		// THE SECOND SWEEP IS NOT REDUNDANT. For two parallel segments every vertex of A is
		// the same distance from B, so A's vertices alone describe a tie whose ends are A's
		// own corners - and where B is the SHORTER line, the tie ends part way along a span
		// of A that no vertex of A sits at. B's vertices, projected onto A, are those ends.
		for (int32 At = 0; At < B.Num(); ++At)
		{
			int32 Index = 0;
			double Fraction = 0.0;
			const double Distance = NearestOnPolyline(A, B[At], Index, Fraction);
			Candidates.Add({ Distance, ParamAtSample(Index, Fraction, A.Num()) });
		}

		double Best = TNumericLimits<double>::Max();
		for (const FCandidate& Candidate : Candidates)
		{
			Best = FMath::Min(Best, Candidate.Distance);
		}

		// THE MIDDLE OF THE TIE. Two parallel lines are equally near along a whole INTERVAL,
		// so "the nearest point" is not a point at all, and answering with an end of that
		// interval is answering with a corner. FAnchorLink then splits the lane there, against
		// its own comment saying entry is in the middle of a side - and a truck entering at a
		// corner has to drive round the lane to reach anything.
		//
		// ABSOLUTE, in uu, not a relative epsilon: these are distances between guidelines on
		// an airport, where a hundredth of a centimetre apart is the same place, and a
		// relative test would widen the tie as the lines got further apart.
		constexpr double TieTolerance = 1e-2;
		double Lowest = TNumericLimits<double>::Max();
		double Highest = -TNumericLimits<double>::Max();
		for (const FCandidate& Candidate : Candidates)
		{
			if (Candidate.Distance <= Best + TieTolerance)
			{
				Lowest = FMath::Min(Lowest, Candidate.ParamOnA);
				Highest = FMath::Max(Highest, Candidate.ParamOnA);
			}
		}

		// A single winner leaves Lowest == Highest, so this is the ordinary answer too and
		// needs no branch: the midpoint of a degenerate interval is the winner itself.
		const double ParamOnA = FMath::Clamp((Lowest + Highest) * 0.5, 0.0, 1.0);

		// ParamAtSample read backwards, and exactly as it is written: samples are evenly
		// spaced in the PARAMETER, not in arc length, so this is its inverse and not an
		// approximation of one. Kept here rather than published beside it - nothing else has
		// ever needed to go this way, and a second public spelling of the same mapping is a
		// second thing to keep in step.
		const double Position = ParamOnA * (A.Num() - 1);
		OutAIndex = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(Position)), 0, A.Num() - 2);
		OutAFraction = FMath::Clamp(Position - OutAIndex, 0.0, 1.0);

		// B FOLLOWS A, rather than being carried along from whichever sweep won. The point on
		// B has to be the one nearest the point on A that was actually chosen - carrying it
		// would pair the middle of the lane with a corner of the road.
		const FVector2D PointOnA =
			FMath::Lerp(A[OutAIndex], A[OutAIndex + 1], OutAFraction);
		NearestOnPolyline(B, PointOnA, OutBIndex, OutBFraction);
		return Best;
	}

	namespace
	{
		/**
		 * The direction of travel AT a vertex: the average of the two segments meeting
		 * there, rather than either one of them.
		 *
		 * A polyline has no direction at a vertex - it has two - and picking one is what
		 * made an agent's heading a staircase. Averaging gives the corner a single
		 * direction that both neighbours agree on, which is what lets the heading either
		 * side of it interpolate to the same value and so pass through it continuously.
		 *
		 * Degenerate spans are stepped over rather than treated as directions: a repeated
		 * point is not a hairpin, and normalising it would return an arbitrary one.
		 */
		/**
		 * Sharpest turn at a vertex still treated as an artefact of SAMPLING rather than as
		 * intended geometry, in radians.
		 *
		 * Not a taste setting - it separates two different things that look alike in a bare
		 * polyline. Sample() lays down DefaultSamples points along a quadratic, and a
		 * quadratic turns at most 180 degrees end to end, so the sharpest a sampling vertex
		 * can ever bend is about 180/15 = 12 degrees. Anything sharper was MEANT: a
		 * hand-drawn guideline doubling back, or two edges meeting at a real corner.
		 *
		 * The two want opposite treatment. Smoothing a sampled curve recovers the tangent
		 * the samples approximate. Smoothing a real corner would have the agent facing 22
		 * degrees off its direction of travel through the turn - an aircraft crabbing
		 * sideways down the taxiway, which is worse than the snap it replaced.
		 */
		constexpr double MaxSampledTurn = 0.35;   // ~20 degrees

		/**
		 * bLeaving says which side of the vertex the caller is on, and only matters at a
		 * real corner: the span BEFORE it ends facing the way it arrived, the span AFTER
		 * begins facing the way it leaves. On a sampled curve both sides get the same
		 * averaged direction, which is what makes the heading continuous through it.
		 */
		FVector2D VertexDirection(const TArray<FVector2D>& Points, int32 Vertex, bool bLeaving)
		{
			FVector2D Before = FVector2D::ZeroVector;
			for (int32 At = Vertex; At >= 1; --At)
			{
				const FVector2D Span = Points[At] - Points[At - 1];
				if (!Span.IsNearlyZero())
				{
					Before = Span.GetSafeNormal();
					break;
				}
			}

			FVector2D After = FVector2D::ZeroVector;
			for (int32 At = Vertex; At + 1 < Points.Num(); ++At)
			{
				const FVector2D Span = Points[At + 1] - Points[At];
				if (!Span.IsNearlyZero())
				{
					After = Span.GetSafeNormal();
					break;
				}
			}

			if (Before.IsNearlyZero()) { return After; }
			if (After.IsNearlyZero())  { return Before; }

			// A REAL corner keeps its own segments' directions: averaging across it would
			// point the agent into the corner rather than along either road.
			const double Turn = FMath::Abs(FMath::UnwindRadians(
				FMath::Atan2(After.Y, After.X) - FMath::Atan2(Before.Y, Before.X)));
			if (Turn > MaxSampledTurn)
			{
				return bLeaving ? After : Before;
			}

			const FVector2D Sum = Before + After;

			// A true 180 degree reversal cancels. Nothing the builder produces contains
			// one, but a hand-drawn link doubling back would - and the direction AFTER the
			// vertex is the one the agent is about to travel in.
			return Sum.IsNearlyZero() ? After : Sum.GetSafeNormal();
		}
	}

	void VertexHeadings(
		const TArray<FVector2D>& Points, int32 Vertex,
		double& OutArriving, double& OutLeaving)
	{
		if (!Points.IsValidIndex(Vertex) || Points.Num() < 2)
		{
			return;
		}

		const FVector2D Arriving = VertexDirection(Points, Vertex, /*bLeaving=*/false);
		const FVector2D Leaving = VertexDirection(Points, Vertex, /*bLeaving=*/true);
		if (Arriving.IsNearlyZero() || Leaving.IsNearlyZero())
		{
			return;
		}

		OutArriving = FMath::Atan2(Arriving.Y, Arriving.X);
		OutLeaving = FMath::Atan2(Leaving.Y, Leaving.X);
	}

	bool PointAtDistance(
		const TArray<FVector2D>& Points, double Distance,
		FVector2D& OutPosition, double& OutHeading)
	{
		if (Points.Num() < 2)
		{
			// One point has a position but no direction, and inventing one - zero, say -
			// would point every arrived agent due east. Refusing lets the caller keep
			// whatever heading it already had.
			return false;
		}

		double Walked = 0.0;
		for (int32 At = 1; At < Points.Num(); ++At)
		{
			const FVector2D Step = Points[At] - Points[At - 1];
			const double StepLength = Step.Size();
			if (StepLength <= 0.0)
			{
				continue;
			}

			if (Distance <= Walked + StepLength)
			{
				const double Into = FMath::Clamp(Distance - Walked, 0.0, StepLength);
				OutPosition = Points[At - 1] + Step * (Into / StepLength);

				// INTERPOLATED across the span, not the span's own direction.
				//
				// Holding the segment's direction made heading a staircase: constant for a
				// whole span, then a jump at the vertex. Invisible on a straight route,
				// where every span is parallel - but a 90 degree sweep sampled sixteen
				// times is fifteen jumps of six degrees, and that is what a taxiing
				// aircraft rounding a corner looked like.
				//
				// Position is untouched. The agent still walks exactly the polyline the
				// overlay draws; only which way it is facing while doing so has changed.
				const FVector2D DirStart = VertexDirection(Points, At - 1, /*bLeaving=*/true);
				const FVector2D DirEnd = VertexDirection(Points, At, /*bLeaving=*/false);

				const double HeadingStart = FMath::Atan2(DirStart.Y, DirStart.X);
				const double HeadingEnd = FMath::Atan2(DirEnd.Y, DirEnd.X);

				// Unwound before scaling, so a turn across the +/-PI seam is the short way
				// round rather than very nearly a full revolution.
				OutHeading = HeadingStart
					+ FMath::UnwindRadians(HeadingEnd - HeadingStart) * (Into / StepLength);
				return true;
			}

			Walked += StepLength;
		}

		// Past the end, or a polyline of nothing but coincident points. Report the last
		// real position and the last real direction: an agent that overshoots by a frame
		// should be standing at its destination facing the way it arrived.
		for (int32 At = Points.Num() - 1; At >= 1; --At)
		{
			const FVector2D Step = Points[At] - Points[At - 1];
			if (!Step.IsNearlyZero())
			{
				OutPosition = Points.Last();

				// The END vertex's direction, for the same reason as above - so an agent
				// that arrives does not snap as it stops.
				const FVector2D Facing = VertexDirection(Points, Points.Num() - 1, /*bLeaving=*/false);
				OutHeading = FMath::Atan2(Facing.Y, Facing.X);
				return true;
			}
		}

		return false;
	}
}
