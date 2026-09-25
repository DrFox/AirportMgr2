#include "Model/VehicleFit.h"

#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/Vehicle.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/UTurnGeom.h"
#include "Solve/VehicleSweep.h"

VehicleSweep::FBody VehicleFit::BodyOf(const FVehicle& Vehicle)
{
	VehicleSweep::FBody Body;
	Body.Wheelbase = Vehicle.Chassis.Wheelbase();
	Body.Width = Vehicle.BodyWidth;
	Body.FrontX = Vehicle.BodyFrontX;
	Body.RearX = Vehicle.BodyRearX;
	// Link for link, field for field: FTowLink is FLink with UPROPERTYs on. An empty Tow maps to
	// an empty chain - rigid - as an unset FTrailer used to map to KingpinToAxle 0.
	for (const FTowLink& Link : Vehicle.Tow)
	{
		VehicleSweep::FLink& Out = Body.Tow.AddDefaulted_GetRef();
		Out.HitchX = Link.HitchX;
		Out.Length = Link.Length;
		Out.BodyFront = Link.BodyFront;
		Out.BodyRear = Link.BodyRear;
		Out.Width = Link.Width;
	}
	return Body;
}

FString FFitVerdict::Describe() const
{
	switch (Refusal)
	{
	case EFitRefusal::LaneTooNarrow:
		return FString::Printf(TEXT("body %.2f m with margins vs lane %.2f m"), Needed / 100.0, Available / 100.0);
	case EFitRefusal::TighterThanLock:
		return FString::Printf(TEXT("lock radius %.1f m vs curve %.1f m"), Needed / 100.0, Available / 100.0);
	case EFitRefusal::Jackknife:
		return TEXT("the tow jack-knifes on the curve");
	case EFitRefusal::SweptOverTarmac:
		return FString::Printf(TEXT("swept %.1f m vs tarmac %.1f m at sample %d"), Needed / 100.0, Available / 100.0, Sample);
	default:
		return FString();
	}
}

FFitVerdict VehicleFit::Judge(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network)
{
	FFitVerdict Verdict;
	const double Widest = Vehicle.WidestBody();
	if (Widest > 0.0 && Edge.Width > 0.0 && Widest + 2.0 * WidthMargin > Edge.Width)
	{
		Verdict.Refusal = EFitRefusal::LaneTooNarrow;
		Verdict.Needed = Widest + 2.0 * WidthMargin;
		Verdict.Available = Edge.Width;
		return Verdict;
	}
	if (Edge.MinRadius <= 0.0)
	{
		return Verdict;
	}

	const double Lock = Vehicle.Chassis.TightestFollowableRadius();
	if (Lock > Edge.MinRadius)
	{
		Verdict.Refusal = EFitRefusal::TighterThanLock;
		Verdict.Needed = Lock;
		Verdict.Available = Edge.MinRadius;
		return Verdict;
	}
	if (Widest <= 0.0 || Edge.ClearInnerAt.Num() == 0)
	{
		return Verdict;
	}

	const FGuidelineNode* A = Network.GetGuidelineNode(Edge.A);
	const FGuidelineNode* B = Network.GetGuidelineNode(Edge.B);
	if (A == nullptr || B == nullptr)
	{
		return Verdict;
	}
	// The samples the builder measured the clearances on, so they line up with the simulated
	// path index for index.
	// ENFORCED BY: Airside.Build.MeasuredOnFollowerSamples (these are the follower's points too)
	TArray<FVector2D> Path;
	GuidelineGeom::Sample(A->Position, Edge.Control, B->Position, Path);
	if (Path.Num() != Edge.ClearInnerAt.Num() || Path.Num() != Edge.ClearOuterAt.Num())
	{
		return Verdict;   // measured against a different sampling: say nothing rather than guess
	}

	TArray<double> Inner, Outer;
	if (!VehicleSweep::Trace(BodyOf(Vehicle), Path, Inner, Outer))
	{
		Verdict.Refusal = EFitRefusal::Jackknife;
		return Verdict;
	}
	// THE WORST SAMPLE, not the first: the verdict is the same either way (any excess refuses),
	// and the worst is the figure that says how much wider the road must be.
	double WorstExcess = 0.0;
	for (int32 Index = 0; Index < Path.Num(); ++Index)
	{
		const double Swept = Inner[Index] + Outer[Index];
		const double Tarmac = Edge.ClearInnerAt[Index] + Edge.ClearOuterAt[Index];
		if (Swept > Tarmac && Swept - Tarmac > WorstExcess)
		{
			WorstExcess = Swept - Tarmac;
			Verdict.Refusal = EFitRefusal::SweptOverTarmac;
			Verdict.Needed = Swept;
			Verdict.Available = Tarmac;
			Verdict.Sample = Index;
		}
	}
	return Verdict;
}

bool VehicleFit::Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network)
{
	return Judge(Edge, Vehicle, Network).Fits();
}

double VehicleFit::BalloonRadiusFor(const FVehicle& Vehicle, const FVector2D& InEnd, const FVector2D& OutEnd,
	const FVector2D& Axis)
{
	const double Lock = Vehicle.Chassis.TightestFollowableRadius();
	if (!Vehicle.HasTrailer() || Lock <= 0.0)
	{
		return Lock;
	}
	const VehicleSweep::FBody Body = BodyOf(Vehicle);
	// A lead-in as long as the whole vehicle, so the chain starts the loop laid out behind it
	// rather than folded against the first sample.
	double Chain = Body.Wheelbase;
	for (const VehicleSweep::FLink& Link : Body.Tow)
	{
		Chain += FMath::Abs(Link.HitchX) + Link.Length;
	}
	const FVector2D Along = Axis.GetSafeNormal();
	double Needed = Lock;
	for (int32 Step = 0; Step < 80; ++Step, Needed *= 1.03)
	{
		const TArray<UTurnGeom::FPiece> Pieces = UTurnGeom::Balloon(InEnd, OutEnd, Along, Needed);
		if (Pieces.Num() == 0)
		{
			return Lock;
		}
		TArray<FVector2D> Path;
		const int32 LeadSteps = FMath::Max(2, FMath::CeilToInt32(Chain / 100.0));
		for (int32 Index = LeadSteps; Index > 0; --Index)
		{
			Path.Add(InEnd - Along * (Chain * Index / LeadSteps));
		}
		FVector2D From = InEnd;
		for (const UTurnGeom::FPiece& Piece : Pieces)
		{
			TArray<FVector2D> Points;
			GuidelineGeom::Sample(From, Piece.Control, Piece.End, Points);
			if (Path.Num() > 0 && Points.Num() > 0 && Path.Last().Equals(Points[0]))
			{
				Points.RemoveAt(0);
			}
			Path.Append(Points);
			From = Piece.End;
		}
		TArray<double> Inner, Outer;
		if (VehicleSweep::Trace(Body, Path, Inner, Outer))
		{
			return Needed * TowBalloonMargin;
		}
	}
	return Needed;
}
