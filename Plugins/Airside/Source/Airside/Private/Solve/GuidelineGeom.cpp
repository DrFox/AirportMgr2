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
				RoadGeom::Bearing(After) - RoadGeom::Bearing(Before)));
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

		OutArriving = RoadGeom::Bearing(Arriving);
		OutLeaving = RoadGeom::Bearing(Leaving);
	}

	bool PointAtDistance(
		const TArray<FVector2D>& Points, double Distance,
		FVector2D& OutPosition, double& OutHeading,
		int32& InOutHintVertex, double& InOutHintWalked)
	{
		if (Points.Num() < 2)
		{
			// One point has a position but no direction, and inventing one - zero, say -
			// would point every arrived agent due east. Refusing lets the caller keep
			// whatever heading it already had.
			return false;
		}

		// RESUME THE CHECKPOINT, or start over if it cannot be trusted for this call - see
		// the header. Trusting it blindly for a Distance BEHIND it would walk backwards from
		// the wrong end of the span and report a point nearer the START than the truth.
		//
		// CLAMPED UP TO Points.Num(), NOT Points.Num() - 1: Points.Num() is the "past the
		// end" checkpoint the fallback below leaves behind, and the loop's own condition
		// (At < Points.Num()) already skips it correctly - clamping it down to the last
		// valid span would re-test that span with a Walked that already counts it, reporting
		// a point short of the true one by that span's length.
		int32 At = FMath::Clamp(InOutHintVertex, 1, Points.Num());
		double Walked = InOutHintWalked;
		if (Distance < Walked)
		{
			At = 1;
			Walked = 0.0;
		}

		for (; At < Points.Num(); ++At)
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

				const double HeadingStart = RoadGeom::Bearing(DirStart);
				const double HeadingEnd = RoadGeom::Bearing(DirEnd);

				// Unwound before scaling, so a turn across the +/-PI seam is the short way
				// round rather than very nearly a full revolution.
				OutHeading = HeadingStart
					+ FMath::UnwindRadians(HeadingEnd - HeadingStart) * (Into / StepLength);

				// CHECKPOINTED AT THE START OF THIS SPAN, not one past it: the next call's
				// Distance may land in this same span again (two callers a footprint apart
				// on a long edge, or the same follower a substep later that barely moved),
				// and re-testing it first is one comparison, not a walk backed into it.
				InOutHintVertex = At;
				InOutHintWalked = Walked;
				return true;
			}

			Walked += StepLength;
		}

		// Past the end, or a polyline of nothing but coincident points. Report the last
		// real position and the last real direction: an agent that overshoots by a frame
		// should be standing at its destination facing the way it arrived.
		for (int32 Rewind = Points.Num() - 1; Rewind >= 1; --Rewind)
		{
			const FVector2D Step = Points[Rewind] - Points[Rewind - 1];
			if (!Step.IsNearlyZero())
			{
				OutPosition = Points.Last();

				// The END vertex's direction, for the same reason as above - so an agent
				// that arrives does not snap as it stops.
				const FVector2D Facing = VertexDirection(Points, Points.Num() - 1, /*bLeaving=*/false);
				OutHeading = RoadGeom::Bearing(Facing);

				// PARKED PAST THE LAST SPAN, so an agent sitting past the end of its route
				// (arrived, or overshot by a frame) does not re-walk the whole polyline on
				// every further call. Points.Num(), NOT Points.Num() - 1: Walked here is
				// already the FULL length (the for loop above ran to completion), so a hint
				// pointing at the last span would have this same length double-counted on
				// the next call's Walked + StepLength test - see the resume clamp's own
				// comment for the failure that produces.
				InOutHintVertex = Points.Num();
				InOutHintWalked = Walked;
				return true;
			}
		}

		return false;
	}

	bool PointAtDistance(
		const TArray<FVector2D>& Points, double Distance,
		FVector2D& OutPosition, double& OutHeading)
	{
		// THE ORACLE: always walks from vertex 1 with nothing carried in, so a test that
		// compares this against the hinted overload above is comparing two genuinely
		// different entry points into the SAME loop, not two copies of it that could drift
		// apart - see the header's "single-evaluator" note.
		int32 HintVertex = 1;
		double HintWalked = 0.0;
		return PointAtDistance(Points, Distance, OutPosition, OutHeading, HintVertex, HintWalked);
	}
}

double GuidelineGeom::ParamAtArcOffset(const TArray<FVector2D>& Points, double Param, double Offset)
{
	if (Points.Num() < 2)
	{
		return FMath::Clamp(Param, 0.0, 1.0);
	}

	const int32 Spans = Points.Num() - 1;
	const double Scaled = FMath::Clamp(Param, 0.0, 1.0) * Spans;
	int32 Index = FMath::Clamp(static_cast<int32>(Scaled), 0, Spans - 1);
	double Fraction = Scaled - Index;

	double Remaining = FMath::Abs(Offset);
	const bool bForward = Offset >= 0.0;

	while (Remaining > 0.0)
	{
		const double SpanLength = FVector2D::Distance(Points[Index], Points[Index + 1]);
		const double Available = bForward ? SpanLength * (1.0 - Fraction) : SpanLength * Fraction;

		if (SpanLength <= 0.0 || Available >= Remaining)
		{
			Fraction += (bForward ? 1.0 : -1.0) * (SpanLength > 0.0 ? Remaining / SpanLength : 0.0);
			break;
		}

		Remaining -= Available;
		if (bForward)
		{
			if (Index + 1 >= Spans) { Fraction = 1.0; break; }
			++Index;
			Fraction = 0.0;
		}
		else
		{
			if (Index == 0) { Fraction = 0.0; break; }
			--Index;
			Fraction = 1.0;
		}
	}

	return ParamAtSample(Index, FMath::Clamp(Fraction, 0.0, 1.0), Points.Num());
}

double GuidelineGeom::TightestRadius(
	const FVector2D& A, const FVector2D& Control, const FVector2D& B)
{
	const FVector2D First = Control - A;
	const FVector2D Second = B - Control;

	const double Cross = FMath::Abs(First.X * Second.Y - First.Y * Second.X);
	if (Cross <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		// Collinear control: a straight line, which bends nowhere.
		return TNumericLimits<double>::Max();
	}

	const FVector2D Sweep = Second - First;
	const double Length = Sweep.SizeSquared();
	const double At = Length > 0.0
		? FMath::Clamp(-FVector2D::DotProduct(First, Sweep) / Length, 0.0, 1.0)
		: 0.0;

	const double Least = (First + Sweep * At).Size();
	return 2.0 * Least * Least * Least / Cross;
}

double GuidelineGeom::ShiftDeflectionFor(double Radius, double Shift, double& OutRun)
{
	OutRun = 0.0;
	if (Shift <= UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return 0.0;
	}

	// The right-angle cap, and the answer outright for a caller with no radius to clear.
	double Deflect = UE_DOUBLE_HALF_PI;
	if (Radius > UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		// sin^2(b/2) = (sqrt(1 + 4k^2) - 1) / (2k^2), k = 4R/Shift. See the header for where
		// that comes from; it is CornerRunFor solved for its second argument.
		const double K = 4.0 * Radius / Shift;
		const double SinSquared = (FMath::Sqrt(1.0 + 4.0 * K * K) - 1.0) / (2.0 * K * K);
		Deflect = FMath::Min(2.0 * FMath::Asin(FMath::Sqrt(FMath::Clamp(SinSquared, 0.0, 1.0))),
			UE_DOUBLE_HALF_PI);
	}

	// Both curves carry half the shift between them: 2 s sin(b) = Shift.
	const double Sine = FMath::Sin(Deflect);
	OutRun = Sine > UE_DOUBLE_KINDA_SMALL_NUMBER ? Shift / (2.0 * Sine) : 0.0;
	return Deflect;
}

double GuidelineGeom::LaneChangeLength(double Radius, double Shift)
{
	double Run = 0.0;
	const double Deflect = ShiftDeflectionFor(Radius, Shift, Run);
	return 2.0 * Run * (1.0 + FMath::Cos(Deflect));
}

bool GuidelineGeom::LaneChange(const FVector2D& From, const FVector2D& To, const FVector2D& Travel,
	FVector2D& OutControlIn, FVector2D& OutMid, FVector2D& OutControlOut)
{
	const FVector2D Dir = Travel.GetSafeNormal();
	const double Along = FVector2D::DotProduct(To - From, Dir);
	const double Shift = FMath::Abs(FVector2D::CrossProduct(Dir, To - From));
	if (Along <= 1.0 || Shift <= 1.0)
	{
		return false;
	}
	// tan(b/2) = Shift / Along, s = Shift / (2 sin b) - see the header.
	const double Deflect = 2.0 * FMath::Atan2(Shift, Along);
	const double Run = Shift / (2.0 * FMath::Sin(Deflect));
	OutControlIn = From + Dir * Run;
	OutMid = (From + To) * 0.5;
	OutControlOut = To - Dir * Run;
	return true;
}

bool GuidelineGeom::Arc(const FVector2D& From, const FVector2D& FromDir, const FVector2D& To, const FVector2D& ToDir,
	const FVector2D& Centre, double MaxPieceSweep, TArray<FArcPiece>& OutPieces)
{
	OutPieces.Reset();
	const FVector2D InDir = FromDir.GetSafeNormal();
	const FVector2D OutDir = ToDir.GetSafeNormal();
	const double Turn = FVector2D::CrossProduct(InDir, OutDir);
	if (FMath::Abs(Turn) < 1e-9 || MaxPieceSweep <= 0.0)
	{
		return false;
	}
	const double Sign = Turn > 0.0 ? 1.0 : -1.0;
	const double Radius = 0.5 * (FVector2D::Distance(From, Centre) + FVector2D::Distance(To, Centre));
	const double Start = FMath::Atan2(From.Y - Centre.Y, From.X - Centre.X);
	const double Finish = FMath::Atan2(To.Y - Centre.Y, To.X - Centre.X);
	// The sweep in the direction of travel: counter-clockwise for a left turn, so in (0, 2 pi).
	double Sweep = (Finish - Start) * Sign;
	while (Sweep <= 0.0) { Sweep += 2.0 * UE_DOUBLE_PI; }
	while (Sweep > 2.0 * UE_DOUBLE_PI) { Sweep -= 2.0 * UE_DOUBLE_PI; }
	const int32 Count = FMath::Max(1, FMath::CeilToInt32(Sweep / MaxPieceSweep - 1e-9));
	const double Step = Sign * Sweep / Count;

	// Each piece's two ends and their travel directions; its control is where the two tangent
	// lines cross. From and To keep the caller's directions, so the lanes stay tangent exactly.
	FVector2D PrevAt = From;
	FVector2D PrevDir = InDir;
	for (int32 Piece = 1; Piece <= Count; ++Piece)
	{
		const bool bLast = Piece == Count;
		const double Angle = Start + Step * Piece;
		const FVector2D Radial(FMath::Cos(Angle), FMath::Sin(Angle));
		const FVector2D At = bLast ? To : Centre + Radial * Radius;
		// Travelling counter-clockwise the tangent is the radial turned left; clockwise, right.
		const FVector2D Dir = bLast ? OutDir : FVector2D(-Radial.Y, Radial.X) * Sign;
		const double Cross = FVector2D::CrossProduct(PrevDir, Dir);
		if (FMath::Abs(Cross) < 1e-12)
		{
			OutPieces.Reset();
			return false;
		}
		const double Along = FVector2D::CrossProduct(At - PrevAt, Dir) / Cross;
		OutPieces.Add({ At, PrevAt + PrevDir * Along });
		PrevAt = At;
		PrevDir = Dir;
	}
	return true;
}

bool GuidelineGeom::BendLane(const FVector2D& From, const FVector2D& FromDir, const FVector2D& To, const FVector2D& ToDir,
	const FVector2D& Centre, TArray<FArcPiece>& OutPieces)
{
	OutPieces.Reset();
	const FVector2D InDir = FromDir.GetSafeNormal();
	const FVector2D OutDir = ToDir.GetSafeNormal();
	const double Turn = FVector2D::CrossProduct(InDir, OutDir);
	// Round Centre: it lies on the side the turn bends toward.
	if (FMath::Abs(Turn) < 1e-9 || Turn * FVector2D::CrossProduct(InDir, Centre - From) <= 0.0)
	{
		return false;
	}
	// The tangent points: Centre's feet on the two lane lines, ahead of From and behind To.
	const double AheadIn = FVector2D::DotProduct(Centre - From, InDir);
	const double BehindOut = FVector2D::DotProduct(To - Centre, OutDir);
	if (AheadIn < -BendTangentTolerance || BehindOut < -BendTangentTolerance)
	{
		return false;
	}
	const bool bLeadIn = AheadIn > BendTangentTolerance;
	const bool bLeadOut = BehindOut > BendTangentTolerance;
	const FVector2D ArcFrom = bLeadIn ? From + InDir * AheadIn : From;
	const FVector2D ArcTo = bLeadOut ? To - OutDir * BehindOut : To;
	if (FMath::Abs(FVector2D::Distance(ArcFrom, Centre) - FVector2D::Distance(ArcTo, Centre)) > BendTangentTolerance)
	{
		return false;
	}
	TArray<FArcPiece> Round;
	if (!Arc(ArcFrom, InDir, ArcTo, OutDir, Centre, BendArcPieceSweep, Round))
	{
		return false;
	}
	// A straight piece is spelled as the builder spells it: control on the midpoint (IsStraight).
	if (bLeadIn)
	{
		OutPieces.Add({ ArcFrom, (From + ArcFrom) * 0.5 });
	}
	OutPieces.Append(Round);
	if (bLeadOut)
	{
		OutPieces.Add({ To, (ArcTo + To) * 0.5 });
	}
	return true;
}

double GuidelineGeom::CornerRunFor(double Radius, double Interior)
{
	const double Half = Interior * 0.5;
	const double Sin = FMath::Sin(Half);
	if (Sin * Sin < UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return TNumericLimits<double>::Max();
	}
	return Radius * FMath::Cos(Half) / (Sin * Sin);
}
