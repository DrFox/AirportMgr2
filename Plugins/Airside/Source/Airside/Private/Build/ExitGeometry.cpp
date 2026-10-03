#include "Build/ExitGeometry.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"

namespace ExitGeometry
{
	double NodeExitLength(const URoadNetwork& Network, int32 NodeIndex, const TArray<FRoadSegmentId>& Arms)
	{
		int32 Continuous = 0;
		double ExitLength = 0.0;
		double Shortest = TNumericLimits<double>::Max();
		for (const FRoadSegmentId& ArmId : Arms)
		{
			const FRoadSegment* Arm = Network.GetSegment(ArmId);
			const URoadProfile* Profile = Arm ? Network.ProfileFor(*Arm) : nullptr;
			if (Arm == nullptr || Profile == nullptr)
			{
				continue;
			}
			if (Profile->bContinuousThroughJunctions)
			{
				++Continuous;
				// The runway decides its exits. Two runways crossing a taxiway at one node
				// would disagree only by profile; the longer wins, which is the safer arc.
				ExitLength = FMath::Max(ExitLength, Profile->ExitLength);
			}
			const FRoadNode* A = Network.GetNode(Arm->A);
			const FRoadNode* B = Network.GetNode(Arm->B);
			if (A && B)
			{
				Shortest = FMath::Min(Shortest, FVector2D::Distance(A->Position, B->Position));
			}
		}
		if (Continuous == 0 || Continuous == Arms.Num() || ExitLength <= 0.0)
		{
			return 0.0;
		}
		// The tightest any arm at this node can afford, so nothing crosses its own far end,
		// and the same on every side so every arc here is symmetric.
		return FMath::Max(0.0, FMath::Min(ExitLength, ArmShare * Shortest));
	}

	double FlareRadius(double TangentLength, double CornerAngle, double TaxiwayHalfWidth)
	{
		if (TangentLength <= 0.0 || CornerAngle <= 0.0 || CornerAngle >= PI)
		{
			return 0.0;
		}
		return FMath::Max(0.0, TangentLength * FMath::Tan(CornerAngle * 0.5) - TaxiwayHalfWidth);
	}

	double TaxiwayEndFloor(double RunwayHalfWidth, double TaxiwayHalfWidth, double AxisAngle)
	{
		const double Acute = FMath::Abs(FMath::UnwindRadians(AxisAngle));
		const double Sine = FMath::Max(FMath::Sin(FMath::Min(Acute, PI - Acute)), FMath::Sin(FMath::DegreesToRadians(10.0)));
		return (RunwayHalfWidth + TaxiwayHalfWidth) / Sine;
	}

	double TaxiwayEndFloorAtRunwayEnd(double RunwayHalfWidth, double TaxiwayHalfWidth,
		const FVector2D& RunwayTangent, const FVector2D& TaxiwayTangent)
	{
		const FVector2D R = RunwayTangent.GetSafeNormal();
		const FVector2D A = TaxiwayTangent.GetSafeNormal();
		const double Along = FVector2D::DotProduct(A, R);
		const double Across = FMath::Abs(FVector2D::CrossProduct(A, R));
		const double Side = TaxiwayEndFloor(RunwayHalfWidth, TaxiwayHalfWidth, RoadGeom::AngleBetween(A, R));
		if (Along >= 0.0)
		{
			return Side;   // back alongside the body: the side case
		}
		// Each corner of the bar is at L * A +/- T * (A rotated), its distance into the runway
		// L * Along +/- T * Across; both are behind the end line once L >= T * Across / -Along.
		return FMath::Min(TaxiwayHalfWidth * Across / -Along, Side);
	}

	bool IsInLineAtRunwayEnd(const FVector2D& RunwayTangent, const FVector2D& TaxiwayTangent)
	{
		// RoadGeom's one definition - see InLineEndDegrees.
		return RoadGeom::IsInLine(RunwayTangent, TaxiwayTangent);
	}
}
