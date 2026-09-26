#include "Solve/TowReverse.h"

#include "Solve/RoadGeom.h"

namespace
{
	FVector2D Perp(const FVector2D& V) { return FVector2D(-V.Y, V.X); }
	FVector2D Dir(double Radians) { return FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)); }
	double HeadingOf(const FVector2D& V) { return FMath::Atan2(V.Y, V.X); }

	/**
	 * THE STEP, uu of tractor fixed-axle travel: VehicleSweep::TraceStep, the step the forward
	 * Trace and the agent's tow sub-step both take, so a reverse is sampled as finely as the
	 * forward drive either side of it.
	 */
	constexpr double Step = VehicleSweep::TraceStep;

	/**
	 * THE CONTROLLER'S TWO FIGURES, as multiples of the reverse link's length so one pair suits
	 * a 10 m semi-trailer and a 3.4 m locked drawbar alike. Chosen 2026-09-26 by a sweep of both
	 * chains round 90 degree bays of R 600-1500 (the ArcTrackedWithinTolerance shapes): the hitch
	 * loop must settle well inside the pursuit's own look-ahead, or the two chase each other and
	 * the trailer runs wide of the arc. At (3, 1.5) the rig ran 100 uu wide; at (12, 0.4) the
	 * worst of the sweep was 7 uu.
	 *  - HitchGainPerLength: the hitch angle's error decays over L / this of travel.
	 *  - LookAheadLengths: the pursuit point sits this many link lengths ahead of the trailer axle;
	 *    longer cuts the corner early, shorter chases the line's own sampling.
	 * NO STEERING-RATE LIMIT, tried and rejected in the same sweep: a rate limit is a delay, and
	 * a delay in an unstable loop (the reverse hitch) diverged at every limit tried, 0.2-0.5 deg
	 * per step. The small step-to-step twitch this leaves is smoothed for DISPLAY only - see
	 * SmoothSteering - never in the kinematics.
	 */
	constexpr double HitchGainPerLength = 12.0;
	constexpr double LookAheadLengths = 0.4;
	constexpr double MinLookAhead = 150.0;

	/** Samples either side a displayed wheel angle is averaged over: 3 x TraceStep = 30 uu. */
	constexpr int32 SteerSmoothingHalfWindow = 3;

	/**
	 * THE WHEELS SHOWN, averaged over a short window of travel. The solved curvature twitches by
	 * a degree or two from step to step (the controller above, 2026-09-26 sweep) - harmless to
	 * the path, but at playback speed it reads as a steering wheel shaking. What the view draws
	 * is the average; what the chain was stepped by is not touched.
	 */
	void SmoothSteering(TArray<TowReverse::FSample>& Samples)
	{
		TArray<double> Raw;
		Raw.Reserve(Samples.Num());
		for (const TowReverse::FSample& Sample : Samples)
		{
			Raw.Add(Sample.SteerDegrees);
		}
		for (int32 Index = 0; Index < Samples.Num(); ++Index)
		{
			double Sum = 0.0;
			int32 Count = 0;
			for (int32 K = FMath::Max(0, Index - SteerSmoothingHalfWindow); K <= FMath::Min(Samples.Num() - 1, Index + SteerSmoothingHalfWindow); ++K)
			{
				Sum += Raw[K];
				++Count;
			}
			Samples[Index].SteerDegrees = Sum / Count;
		}
	}

	/** The polyline measured once: cumulative distance per vertex. */
	struct FMeasuredLine
	{
		const TArray<FVector2D>& Points;
		TArray<double> Along;
		double Length = 0.0;

		explicit FMeasuredLine(const TArray<FVector2D>& InPoints) : Points(InPoints)
		{
			Along.SetNum(Points.Num());
			for (int32 Index = 0; Index < Points.Num(); ++Index)
			{
				Along[Index] = Index == 0 ? 0.0 : Along[Index - 1] + FVector2D::Distance(Points[Index - 1], Points[Index]);
			}
			Length = Along.Num() > 0 ? Along.Last() : 0.0;
		}

		/**
		 * Nearest point from span Hint on - FORWARD ONLY, because the leading axle only ever
		 * advances along its line, and a reverse leg's first stretch retraces the approach it
		 * began on: an unconstrained search could credit the axle to the wrong pass.
		 */
		double Project(const FVector2D& Query, int32& InOutHint, double& OutDistance) const
		{
			double Best = TNumericLimits<double>::Max();
			double BestAlong = 0.0;
			int32 BestSpan = InOutHint;
			for (int32 Span = FMath::Max(0, InOutHint); Span + 1 < Points.Num(); ++Span)
			{
				const FVector2D AB = Points[Span + 1] - Points[Span];
				const double LenSq = AB.SizeSquared();
				const double T = LenSq > 0.0 ? FMath::Clamp(FVector2D::DotProduct(Query - Points[Span], AB) / LenSq, 0.0, 1.0) : 0.0;
				const double Distance = FVector2D::Distance(Query, Points[Span] + AB * T);
				if (Distance < Best - UE_KINDA_SMALL_NUMBER)
				{
					Best = Distance;
					BestAlong = Along[Span] + T * FMath::Sqrt(LenSq);
					BestSpan = Span;
				}
			}
			// Past the last vertex, measured along the last span's own direction, so an axle that
			// overshoots the end by a step reads as past it rather than clamped onto it.
			if (Points.Num() >= 2 && BestSpan == Points.Num() - 2)
			{
				const FVector2D Tangent = (Points.Last() - Points[Points.Num() - 2]).GetSafeNormal();
				const double Beyond = FVector2D::DotProduct(Query - Points.Last(), Tangent);
				if (Beyond > 0.0)
				{
					BestAlong = Length + Beyond;
					Best = FMath::Abs(FVector2D::DotProduct(Query - Points.Last(), Perp(Tangent)));
				}
			}
			InOutHint = BestSpan;
			OutDistance = Best;
			return BestAlong;
		}

		/** The point Distance along, continued straight past the end along the last span. */
		FVector2D PointAt(double Distance) const
		{
			if (Distance >= Length)
			{
				const FVector2D Tangent = (Points.Last() - Points[Points.Num() - 2]).GetSafeNormal();
				return Points.Last() + Tangent * (Distance - Length);
			}
			// A linear walk: Algo/BinarySearch.h is outside what Solve/ may include, and a reverse
			// line is a few hundred vertices at most (2026-09-26).
			int32 Span = 0;
			while (Span + 2 < Points.Num() && Along[Span + 1] < Distance)
			{
				++Span;
			}
			const double SpanLength = Along[Span + 1] - Along[Span];
			const double T = SpanLength > 0.0 ? (Distance - Along[Span]) / SpanLength : 0.0;
			return FMath::Lerp(Points[Span], Points[Span + 1], FMath::Clamp(T, 0.0, 1.0));
		}

		FVector2D EndTangent() const { return (Points.Last() - Points[Points.Num() - 2]).GetSafeNormal(); }
	};

	/** The FULL chain's axles from the locked line: hitch Hitch, the locked link facing G. */
	void PoseFullChain(const VehicleSweep::FBody& Full, const FVector2D& Hitch, const FVector2D& G,
		TArray<FVector2D, TInlineAllocator<2>>& OutAxles)
	{
		OutAxles.Reset();
		FVector2D PullerAxle = Hitch;
		for (int32 Index = 0; Index < Full.Tow.Num(); ++Index)
		{
			const VehicleSweep::FLink& Link = Full.Tow[Index];
			const FVector2D LinkHitch = Index == 0 ? Hitch : PullerAxle + G * Link.HitchX;
			PullerAxle = LinkHitch - G * Link.Length;
			OutAxles.Add(PullerAxle);
		}
	}
}

FString TowReverse::FSolution::Describe() const
{
	switch (Refusal)
	{
	case ERefusal::None:
		return FString::Printf(TEXT("solved: %d samples, worst hitch %.0f deg, worst line error %.0f uu, end %.0f uu / %.1f deg"),
			Samples.Num(), FMath::RadiansToDegrees(WorstHitchRadians), WorstLineError, EndPositionError, EndHeadingErrorDegrees);
	case ERefusal::BadLine:
		return TEXT("the reverse line has no length or direction");
	case ERefusal::OffLine:
		return FString::Printf(TEXT("leading axle %.0f uu off the line (limit %.0f) at (%.0f,%.0f)"), Figure, Limit, Where.X, Where.Y);
	case ERefusal::TurntableBent:
		return FString::Printf(TEXT("turntable bent %.1f deg (the lock engages within %.1f) at (%.0f,%.0f) - straighten it first"),
			Figure, Limit, Where.X, Where.Y);
	case ERefusal::Jackknife:
		return FString::Printf(TEXT("hitch reached %.0f deg (critical %.0f) at (%.0f,%.0f) - the line is too tight to back along"),
			Figure, Limit, Where.X, Where.Y);
	case ERefusal::MissedEnd:
		return FString::Printf(TEXT("ended %.0f uu / %.1f deg off the end pose (limits %.0f uu / %.1f deg) at (%.0f,%.0f)"),
			EndPositionError, EndHeadingErrorDegrees, MaxEndPositionError, MaxEndHeadingDegrees, Where.X, Where.Y);
	case ERefusal::NoArrival:
		return FString::Printf(TEXT("never reached the end of the line (%.0f uu along of %.0f)"), Figure, Limit);
	default:
		return TEXT("?");
	}
}

VehicleSweep::FBody TowReverse::ReverseBody(const VehicleSweep::FBody& Body)
{
	VehicleSweep::FBody Out = Body;
	if (Body.Tow.Num() <= 1)
	{
		return Out;
	}
	// MERGED ALONG ONE LINE: with every joint after the first locked straight, link k's hitch is
	// HitchX_k along link k-1 from ITS axle, and its axle Length_k behind that - so each locked
	// link adds (Length_k - HitchX_k) to the distance from link 0's hitch to the rearmost axle.
	// The utility: 114 + (221 - 0) = 335 uu, the towbar eye to the trailer's rear axle.
	VehicleSweep::FLink Merged = Body.Tow[0];
	double FromHitch = Body.Tow[0].Length;
	for (int32 Index = 1; Index < Body.Tow.Num(); ++Index)
	{
		const VehicleSweep::FLink& Link = Body.Tow[Index];
		const double HitchFromMergedHitch = FromHitch - Link.HitchX;
		FromHitch = HitchFromMergedHitch + Link.Length;
		// The body reaches, re-measured from the merged link's own hitch and axle.
		Merged.BodyFront = FMath::Max(Merged.BodyFront, Link.BodyFront - HitchFromMergedHitch);
		Merged.Width = FMath::Max(Merged.Width, Link.Width);
	}
	Merged.Length = FromHitch;
	Merged.BodyRear = Body.Tow.Last().BodyRear;
	Out.Tow.Reset();
	Out.Tow.Add(Merged);
	return Out;
}

double TowReverse::CriticalHitchRadians(const VehicleSweep::FBody& Reverse, double MaxSteerRadians)
{
	if (Reverse.Tow.Num() == 0 || Reverse.Wheelbase <= 0.0)
	{
		return 0.0;
	}
	const double A = Reverse.Tow[0].HitchX;
	const double L = Reverse.Tow[0].Length;
	const double KMax = FMath::Tan(MaxSteerRadians) / Reverse.Wheelbase;
	// A SCAN, at a tenth of a degree, because the inequality has no tidy inverse once a != 0 and
	// this runs once per solve. The first angle that fails bounds the recoverable range.
	const double Increment = FMath::DegreesToRadians(0.1);
	for (double Phi = 0.0; Phi <= VehicleSweep::MaxHitchRadians; Phi += Increment)
	{
		if (FMath::Sin(Phi) + A * KMax * FMath::Cos(Phi) > L * KMax)
		{
			return FMath::Max(0.0, Phi - Increment);
		}
	}
	return VehicleSweep::MaxHitchRadians;
}

double TowReverse::SteadyHitchRadians(const VehicleSweep::FBody& Reverse, double Kappa)
{
	if (Reverse.Tow.Num() == 0 || FMath::Abs(Kappa) < UE_DOUBLE_SMALL_NUMBER)
	{
		return 0.0;
	}
	const double A = Reverse.Tow[0].HitchX;
	const double L = Reverse.Tow[0].Length;
	const double Rt = 1.0 / FMath::Abs(Kappa);
	const double Rf = FMath::Sqrt(FMath::Max(0.0, Rt * Rt + L * L - A * A));
	const double Phi = FMath::Atan(L / Rt) - (Rf > 0.0 ? FMath::Atan(A / Rf) : 0.0);
	return Kappa > 0.0 ? Phi : -Phi;
}

TowReverse::FSolution TowReverse::Solve(const FInput& In)
{
	FSolution Out;
	if (In.Line.Num() < 2 || In.Body.Tow.Num() == 0 || In.Axles.Num() != In.Body.Tow.Num() || In.Body.Wheelbase <= 0.0)
	{
		Out.Refusal = ERefusal::BadLine;
		return Out;
	}
	const FMeasuredLine Line(In.Line);
	if (Line.Length < 1.0)
	{
		Out.Refusal = ERefusal::BadLine;
		return Out;
	}

	// THE LOCK: every joint after the first must already be near straight to engage.
	{
		TArray<VehicleSweep::FLinkPose, TInlineAllocator<2>> Poses;
		VehicleSweep::PoseChain(In.Body, In.Fixed, Dir(In.Heading), In.Axles, Poses);
		for (int32 Index = 1; Index < Poses.Num(); ++Index)
		{
			const double Bent = FMath::RadiansToDegrees(RoadGeom::AngleBetween(Poses[Index].Heading, Poses[Index - 1].Heading));
			if (Bent > TurntableLockDegrees)
			{
				Out.Refusal = ERefusal::TurntableBent;
				Out.Figure = Bent;
				Out.Limit = TurntableLockDegrees;
				Out.Where = Poses[Index].Hitch;
				return Out;
			}
		}
	}

	const VehicleSweep::FBody Reverse = ReverseBody(In.Body);
	const VehicleSweep::FLink& Link = Reverse.Tow[0];
	const double A = Link.HitchX;
	const double L = Link.Length;
	const double W = Reverse.Wheelbase;
	const double KMax = FMath::Tan(In.MaxSteerRadians) / W;
	const double Critical = CriticalHitchRadians(Reverse, In.MaxSteerRadians);
	const double Gain = HitchGainPerLength / L;
	const double LookAhead = FMath::Max(LookAheadLengths * L, MinLookAhead);

	// STATE, snapped onto the locked line: the rearmost axle keeps its bearing from the hitch,
	// at the merged length. Within TurntableLockDegrees that moves the axle by at most
	// Length * (1 - cos 3 deg) - a fraction of a centimetre on the utility.
	FVector2D Fixed = In.Fixed;
	double Theta = In.Heading;
	FVector2D Hitch = Fixed + Dir(Theta) * A;
	FVector2D G = (Hitch - In.Axles.Last()).GetSafeNormal();
	TArray<FVector2D> Axle = { Hitch - G * L };

	int32 Hint = 0;
	double LineError = 0.0;
	double Along = Line.Project(Axle[0], Hint, LineError);
	if (LineError > MaxLineError)
	{
		Out.Refusal = ERefusal::OffLine;
		Out.Figure = LineError;
		Out.Limit = MaxLineError;
		Out.Where = Axle[0];
		return Out;
	}

	auto Record = [&](double SteerDegrees)
	{
		FSample& Sample = Out.Samples.AddDefaulted_GetRef();
		Sample.Fixed = Fixed;
		Sample.Heading = Theta;
		PoseFullChain(In.Body, Hitch, G, Sample.Axles);
		Sample.SteerDegrees = SteerDegrees;
		Sample.HitchRadians = FMath::UnwindRadians(Theta - HeadingOf(G));
		Sample.Along = Along;
	};
	Record(0.0);

	const double Chain = W + FMath::Abs(A) + L;
	const int32 MaxSteps = FMath::CeilToInt32(4.0 * (Line.Length + 2.0 * Chain) / Step) + 10;
	for (int32 Iteration = 0; Iteration < MaxSteps; ++Iteration)
	{
		if (Along >= Line.Length - UE_KINDA_SMALL_NUMBER)
		{
			break;
		}
		const double Phi = FMath::UnwindRadians(Theta - HeadingOf(G));

		// OUTER: pure pursuit of the trailer axle in ITS direction of travel, -G.
		const FVector2D Travel = -G;
		const FVector2D ToGoal = Line.PointAt(Along + LookAhead) - Axle[0];
		const double Reach = FMath::Max(ToGoal.Size(), UE_KINDA_SMALL_NUMBER);
		const double Alpha = FMath::Atan2(FVector2D::CrossProduct(Travel, ToGoal), FVector2D::DotProduct(Travel, ToGoal));
		// Curvature in the travel sense; the trailer's FORWARD curvature is its negative, since
		// travel runs against the way it faces.
		const double KappaTrailer = -2.0 * FMath::Sin(Alpha) / Reach;
		const double PhiRef = FMath::Clamp(SteadyHitchRadians(Reverse, KappaTrailer), -ReferenceShare * Critical, ReferenceShare * Critical);

		// INNER: the tractor curvature that makes dphi/dsigma = -Gain (phi - phiRef) exactly,
		// from the kinematics in the header with sigma = -s. Clamped to the lock.
		const double Denominator = 1.0 - A * FMath::Cos(Phi) / L;
		double KappaF = FMath::Abs(Denominator) > UE_KINDA_SMALL_NUMBER
			? (Gain * (Phi - PhiRef) + FMath::Sin(Phi) / L) / Denominator
			: 0.0;
		KappaF = FMath::Clamp(KappaF, -KMax, KMax);

		// THE LAST STEP SHORTENED so the leading axle lands on the end rather than up to a step
		// past it: the trailer axle covers about cos(phi) of what the fixed axle does.
		const double Remaining = Line.Length - Along;
		const double Ds = -FMath::Min(Step, Remaining / FMath::Max(FMath::Cos(Phi), 0.2) + UE_KINDA_SMALL_NUMBER);

		// Midpoint heading for the fixed axle's chord, then the chain stepped as the forward
		// drive steps it - StepTrailer's projection holds going backwards too.
		const double ThetaNext = Theta + Ds * KappaF;
		Fixed += Dir(0.5 * (Theta + ThetaNext)) * Ds;
		Theta = ThetaNext;
		int32 Folded = INDEX_NONE;
		double FoldRadians = 0.0;
		VehicleSweep::StepChain(Reverse, Fixed, Dir(Theta), Axle, Folded, FoldRadians);
		Hitch = Fixed + Dir(Theta) * A;
		G = (Hitch - Axle[0]).GetSafeNormal();

		const double PhiNow = FMath::UnwindRadians(Theta - HeadingOf(G));
		Out.WorstHitchRadians = FMath::Max(Out.WorstHitchRadians, FMath::Abs(PhiNow));
		if (Folded != INDEX_NONE || FMath::Abs(PhiNow) > Critical)
		{
			Out.Refusal = ERefusal::Jackknife;
			Out.Figure = FMath::RadiansToDegrees(FMath::Abs(PhiNow));
			Out.Limit = FMath::RadiansToDegrees(Critical);
			Out.Where = Hitch;
			Out.Samples.Reset();
			return Out;
		}

		Along = Line.Project(Axle[0], Hint, LineError);
		Out.WorstLineError = FMath::Max(Out.WorstLineError, LineError);
		if (LineError > MaxLineError)
		{
			Out.Refusal = ERefusal::OffLine;
			Out.Figure = LineError;
			Out.Limit = MaxLineError;
			Out.Where = Axle[0];
			Out.Samples.Reset();
			return Out;
		}
		Record(FMath::RadiansToDegrees(FMath::Atan(W * KappaF)));
	}

	if (Along < Line.Length - UE_KINDA_SMALL_NUMBER)
	{
		Out.Refusal = ERefusal::NoArrival;
		Out.Figure = Along;
		Out.Limit = Line.Length;
		Out.Samples.Reset();
		return Out;
	}

	// THE END POSE: the leading axle on the line's end, the trailer facing AWAY from the way it
	// travelled there - backed in, not driven in.
	Out.EndPositionError = FVector2D::Distance(Axle[0], Line.Points.Last());
	Out.EndHeadingErrorDegrees = FMath::RadiansToDegrees(RoadGeom::AngleBetween(G, -Line.EndTangent()));
	if (Out.EndPositionError > MaxEndPositionError || Out.EndHeadingErrorDegrees > MaxEndHeadingDegrees)
	{
		Out.Refusal = ERefusal::MissedEnd;
		Out.Figure = Out.EndPositionError;
		Out.Limit = MaxEndPositionError;
		Out.Where = Axle[0];
		Out.Samples.Reset();
		return Out;
	}
	SmoothSteering(Out.Samples);
	return Out;
}
