#include "Solve/VehicleSweep.h"

#include "Solve/RoadGeom.h"

VehicleSweep::FEnvelope VehicleSweep::Envelope(const FBody& Body, double SteerRadius)
{
	FEnvelope Out;
	if (SteerRadius <= Body.Wheelbase || SteerRadius <= 0.0)
	{
		return Out;
	}
	const double HalfWidth = Body.Width * 0.5;

	// The fixed axle's circle, and the tractor's corners on it: its inner side is closest to
	// the centre along the fixed axle line; its outer corners are the front and rear corners.
	const double Fixed = FMath::Sqrt(SteerRadius * SteerRadius - Body.Wheelbase * Body.Wheelbase);
	double InnerRadius = Fixed - HalfWidth;
	double OuterRadius = FMath::Max(
		FMath::Sqrt(FMath::Square(Fixed + HalfWidth) + FMath::Square(Body.FrontX)),
		FMath::Sqrt(FMath::Square(Fixed + HalfWidth) + FMath::Square(Body.RearX)));

	// Each hitch rides a circle of its own, set by the axle of whatever pulls it (the tractor's
	// fixed axle for link 0); the link's axle trails it, tangent, at Length - the steady-state
	// tractrix, once per link down the chain. A hitch circle no larger than the link cannot be
	// held: the link would fold into it.
	double Pull = Fixed;
	for (const FLink& Link : Body.Tow)
	{
		const double Hitch = FMath::Sqrt(Pull * Pull + Link.HitchX * Link.HitchX);
		if (Hitch <= Link.Length)
		{
			return Out;
		}
		const double LinkAxle = FMath::Sqrt(Hitch * Hitch - Link.Length * Link.Length);
		const double LinkHalf = Link.Width * 0.5;
		InnerRadius = FMath::Min(InnerRadius, LinkAxle - LinkHalf);
		OuterRadius = FMath::Max3(OuterRadius,
			FMath::Sqrt(FMath::Square(LinkAxle + LinkHalf) + FMath::Square(Link.Length + Link.BodyFront)),
			FMath::Sqrt(FMath::Square(LinkAxle + LinkHalf) + FMath::Square(Link.BodyRear)));
		Pull = LinkAxle;
	}

	Out.Inner = SteerRadius - InnerRadius;
	Out.Outer = OuterRadius - SteerRadius;
	Out.bHolds = true;
	return Out;
}

namespace
{
	FVector2D Perp(const FVector2D& V) { return FVector2D(-V.Y, V.X); }
}

FVector2D VehicleSweep::TrailerHeading(const FVector2D& Kingpin, const FVector2D& TrailerAxle)
{
	return (Kingpin - TrailerAxle).GetSafeNormal();
}

bool VehicleSweep::StepTrailer(const FVector2D& Kingpin, const FVector2D& CabHeading,
	double KingpinToAxle, FVector2D& InOutTrailerAxle)
{
	// Pursuit: the axle stays its fixed distance behind the kingpin it follows (discrete
	// tractrix - Trace's inline copy before this was pulled out, 2026-09-24).
	FVector2D Trailing = TrailerHeading(Kingpin, InOutTrailerAxle);
	InOutTrailerAxle = Kingpin - Trailing * KingpinToAxle;
	Trailing = TrailerHeading(Kingpin, InOutTrailerAxle);
	return FVector2D::DotProduct(Trailing, CabHeading) >= 0.0;   // < 0: folded past square, a jack-knife
}

void VehicleSweep::LayChainStraight(const FBody& Body, const FVector2D& Fixed, const FVector2D& Heading,
	TArray<FVector2D>& OutAxles)
{
	// Each hitch HitchX along the body ahead of it, each axle Length behind its hitch, all on
	// one line - the same arithmetic Trace's lead-in used for its one trailer, so a one-link
	// chain lands on exactly the point it did.
	OutAxles.Reset(Body.Tow.Num());
	FVector2D PullerAxle = Fixed;
	for (const FLink& Link : Body.Tow)
	{
		const FVector2D Hitch = PullerAxle + Heading * Link.HitchX;
		PullerAxle = Hitch - Heading * Link.Length;
		OutAxles.Add(PullerAxle);
	}
}

void VehicleSweep::PoseChain(const FBody& Body, const FVector2D& Fixed, const FVector2D& Heading,
	TArrayView<const FVector2D> Axles, TArray<FLinkPose, TInlineAllocator<2>>& OutPoses)
{
	OutPoses.Reset();
	FVector2D PullerAxle = Fixed;
	FVector2D PullerHeading = Heading;
	for (int32 Index = 0; Index < Body.Tow.Num() && Index < Axles.Num(); ++Index)
	{
		FLinkPose& Pose = OutPoses.AddDefaulted_GetRef();
		Pose.Hitch = PullerAxle + PullerHeading * Body.Tow[Index].HitchX;
		Pose.Axle = Axles[Index];
		Pose.Heading = TrailerHeading(Pose.Hitch, Pose.Axle);
		PullerAxle = Pose.Axle;
		PullerHeading = Pose.Heading;
	}
}

bool VehicleSweep::StepChain(const FBody& Body, const FVector2D& Fixed, const FVector2D& Heading,
	TArrayView<FVector2D> InOutAxles, int32& OutFoldedLink, double& OutFoldRadians)
{
	OutFoldedLink = INDEX_NONE;
	OutFoldRadians = 0.0;

	// IN ORDER, EACH LINK PULLED BY THE ONE AHEAD AS IT HAS JUST MOVED - a chain, not N copies
	// of one trailer. Link k's hitch is placed on link k-1 after link k-1 stepped, so a
	// drawbar body follows the towbar's front axle, which follows the tug; stepped off the tug
	// directly, the body would ride the towbar's path instead of cutting inside it.
	FVector2D PullerAxle = Fixed;
	FVector2D PullerHeading = Heading;
	for (int32 Index = 0; Index < Body.Tow.Num() && Index < InOutAxles.Num(); ++Index)
	{
		const FLink& Link = Body.Tow[Index];
		const FVector2D Hitch = PullerAxle + PullerHeading * Link.HitchX;
		const bool bPursued = StepTrailer(Hitch, PullerHeading, Link.Length, InOutAxles[Index]);
		const FVector2D LinkHeading = TrailerHeading(Hitch, InOutAxles[Index]);

		// THE ANGLE GUARD, beside StepTrailer's own and not instead of it. At MaxHitchRadians
		// = 90 degrees the two draw the same line (Dot < 0 is past square); the angle is what
		// the log reports and what a tighter limit would be written against.
		const double Angle = RoadGeom::AngleBetween(LinkHeading, PullerHeading);
		OutFoldRadians = FMath::Max(OutFoldRadians, Angle);
		if (!bPursued || Angle > MaxHitchRadians)
		{
			OutFoldedLink = Index;
			OutFoldRadians = Angle;
			return false;
		}
		PullerAxle = InOutAxles[Index];
		PullerHeading = LinkHeading;
	}
	return true;
}

bool VehicleSweep::Trace(const FBody& Body, TArrayView<const FVector2D> Path,
	TArray<double>& OutInner, TArray<double>& OutOuter, TArray<FVector2D>* OutAxles)
{
	if (OutAxles != nullptr)
	{
		OutAxles->Reset();
	}
	OutInner.Init(0.0, Path.Num());
	OutOuter.Init(0.0, Path.Num());
	if (Path.Num() < 2)
	{
		return true;
	}

	// 10 uu steps: a tenth of the lane margin, and the step the Python prototype that set the
	// test figures used (2026-09-24). NAMED since the tow became a chain - see TraceStep, which
	// the driving agent's sub-step is sized from too.
	constexpr double Step = TraceStep;
	const FVector2D InTangent = (Path[1] - Path[0]).GetSafeNormal();
	const FVector2D OutTangent = (Path.Last() - Path[Path.Num() - 2]).GetSafeNormal();

	// The whole train's length, so it is running straight into the path. ADDED IN THE ORDER the
	// one-trailer sum was (wheelbase, kingpin, trailer, front) so a one-link chain gets the
	// bitwise-same lead and the same steps - #276's gating must not move. Abs: a drawbar eye
	// sits BEHIND its axle and still lengthens the train.
	double Lead = Body.Wheelbase;
	for (const FLink& Link : Body.Tow)
	{
		Lead = Lead + FMath::Abs(Link.HitchX);
		Lead = Lead + Link.Length;
	}
	Lead = Lead + Body.FrontX + 200.0;

	// Which side is the inside: the sign of the turn from start through middle to end.
	const FVector2D Mid = Path[Path.Num() / 2];
	const double Turn = FVector2D::CrossProduct(Mid - Path[0], Path.Last() - Mid);
	const double InwardSign = Turn >= 0.0 ? 1.0 : -1.0;

	TArray<FVector2D> Steps;
	for (double D = Lead; D > 0.0; D -= Step)
	{
		Steps.Add(Path[0] - InTangent * D);
	}
	for (int32 Index = 0; Index + 1 < Path.Num(); ++Index)
	{
		const FVector2D A = Path[Index];
		const FVector2D B = Path[Index + 1];
		const int32 Count = FMath::Max(1, FMath::FloorToInt32(FVector2D::Distance(A, B) / Step));
		for (int32 Sub = 0; Sub < Count; ++Sub)
		{
			Steps.Add(A + (B - A) * (static_cast<double>(Sub) / Count));
		}
	}
	for (double D = 0.0; D < Lead; D += Step)
	{
		Steps.Add(Path.Last() + OutTangent * D);
	}

	FVector2D Fixed = Steps[0] - InTangent * Body.Wheelbase;
	TArray<FVector2D> Axles;
	LayChainStraight(Body, Fixed, InTangent, Axles);
	TArray<FLinkPose, TInlineAllocator<2>> Poses;

	TArray<FVector2D, TInlineAllocator<24>> Corners;
	for (const FVector2D& Steered : Steps)
	{
		// Pursuit: each trailing point stays its fixed distance behind the one it follows.
		FVector2D Heading = (Steered - Fixed).GetSafeNormal();
		Fixed = Steered - Heading * Body.Wheelbase;
		Heading = (Steered - Fixed).GetSafeNormal();
		const FVector2D Side = Perp(Heading) * (Body.Width * 0.5);

		Corners.Reset();
		for (const double X : { Body.FrontX, Body.RearX, 0.0 })
		{
			Corners.Add(Fixed + Heading * X + Side);
			Corners.Add(Fixed + Heading * X - Side);
		}
		if (Body.Tow.Num() > 0)
		{
			int32 FoldedLink = INDEX_NONE;
			double FoldRadians = 0.0;
			if (!StepChain(Body, Fixed, Heading, Axles, FoldedLink, FoldRadians))
			{
				return false;   // folded past square: a jack-knife
			}
			if (OutAxles != nullptr)
			{
				OutAxles->Append(Axles);
			}
			// Every link's body: front, rear, axle and mid-length, each side. A bar (no body)
			// still sweeps its width from hitch to axle.
			PoseChain(Body, Fixed, Heading, Axles, Poses);
			for (int32 Index = 0; Index < Poses.Num(); ++Index)
			{
				const FLink& Link = Body.Tow[Index];
				const FLinkPose& Pose = Poses[Index];
				const FVector2D LinkSide = Perp(Pose.Heading) * (Link.Width * 0.5);
				for (const double X : { Link.Length + Link.BodyFront, -Link.BodyRear, 0.0, Link.Length * 0.5 })
				{
					Corners.Add(Pose.Axle + Pose.Heading * X + LinkSide);
					Corners.Add(Pose.Axle + Pose.Heading * X - LinkSide);
				}
			}
		}

		for (const FVector2D& Corner : Corners)
		{
			double Best = TNumericLimits<double>::Max();
			int32 BestSpan = 0;
			double BestT = 0.0;
			for (int32 Span = 0; Span + 1 < Path.Num(); ++Span)
			{
				const FVector2D AB = Path[Span + 1] - Path[Span];
				const double T = FMath::Clamp(FVector2D::DotProduct(Corner - Path[Span], AB)
					/ FMath::Max(AB.SizeSquared(), UE_DOUBLE_SMALL_NUMBER), 0.0, 1.0);
				const double Distance = FVector2D::Distance(Corner, Path[Span] + AB * T);
				if (Distance < Best)
				{
					Best = Distance;
					BestSpan = Span;
					BestT = T;
				}
			}
			if ((BestSpan == 0 && BestT <= 0.0) || (BestSpan == Path.Num() - 2 && BestT >= 1.0))
			{
				continue;   // beyond an end: the straight lanes, gated by Width
			}
			const FVector2D AB = Path[BestSpan + 1] - Path[BestSpan];
			const FVector2D Foot = Path[BestSpan] + AB * BestT;
			const double Lateral = FVector2D::DotProduct(Corner - Foot, Perp(AB.GetSafeNormal()) * InwardSign);
			const int32 Sample = BestT < 0.5 ? BestSpan : BestSpan + 1;
			if (Lateral > 0.0)
			{
				OutInner[Sample] = FMath::Max(OutInner[Sample], Lateral);
			}
			else
			{
				OutOuter[Sample] = FMath::Max(OutOuter[Sample], -Lateral);
			}
		}
	}
	return true;
}
