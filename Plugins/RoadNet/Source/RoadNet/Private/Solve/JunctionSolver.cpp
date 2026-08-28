#include "Solve/JunctionSolver.h"

FRay2D FJunctionSolver::MakeLeftEdge(const FJunctionInput& Input, int32 ArmIndex)
{
	const FJunctionArm& Arm = Input.Arms[ArmIndex];
	FRay2D Ray;
	Ray.Dir = Arm.Tangent;
	Ray.Origin = Input.Position + RoadGeom::PerpCCW(Arm.Tangent) * FMath::Max(Arm.HalfWidthLeft, 0.0);
	return Ray;
}

FRay2D FJunctionSolver::MakeRightEdge(const FJunctionInput& Input, int32 ArmIndex)
{
	const FJunctionArm& Arm = Input.Arms[ArmIndex];
	FRay2D Ray;
	Ray.Dir = Arm.Tangent;
	Ray.Origin = Input.Position - RoadGeom::PerpCCW(Arm.Tangent) * FMath::Max(Arm.HalfWidthRight, 0.0);
	return Ray;
}

FJunctionResult FJunctionSolver::SolveCuts(const FJunctionInput& Input)
{
	FJunctionResult Result;

	const int32 ArmCount = Input.Arms.Num();
	if (ArmCount == 0)
	{
		return Result;
	}

	Result.Arms.SetNum(ArmCount);

	if (ArmCount == 1)
	{
		// Dead end: cut back by the arm's own half-width so an end cap has room.
		const FJunctionArm& Arm = Input.Arms[0];
		Result.Arms[0].CutDistance = FMath::Max3(Arm.HalfWidthLeft, Arm.HalfWidthRight, 0.0);
		Result.bValid = true;
	}
	else
	{
		Result.Corners.SetNum(ArmCount);

		for (int32 Index = 0; Index < ArmCount; ++Index)
		{
			const int32 NextIndex = (Index + 1) % ArmCount;

			const FRay2D LeftEdge  = MakeLeftEdge(Input, Index);
			const FRay2D RightEdge = MakeRightEdge(Input, NextIndex);

			// Where two profiles meet, the tighter radius wins; geometry may clamp
			// it further inside SolveFillet.
			const double Radius = FMath::Min(Input.Arms[Index].FilletRadius,
			                                 Input.Arms[NextIndex].FilletRadius);

			Result.Corners[Index] = RoadGeom::SolveFillet(LeftEdge, RightEdge, Radius);
			if (!Result.Corners[Index].bValid)
			{
				return Result; // bValid stays false: this node cannot be solved
			}
		}

		// Arm i's cut is the larger of the two tangent parameters on its own edges:
		// corner i supplied ParamA on arm i's LEFT edge,
		// corner i-1 supplied ParamB on arm i's RIGHT edge.
		// Taking the max guarantees the cut sits at or beyond BOTH tangent points,
		// which is what keeps the boundary walk's straight runs non-negative.
		for (int32 Index = 0; Index < ArmCount; ++Index)
		{
			const int32 PrevIndex = (Index + ArmCount - 1) % ArmCount;

			const double FromLeft = Result.Corners[Index].bStraightThrough
				? 0.0 : Result.Corners[Index].ParamA;
			const double FromRight = Result.Corners[PrevIndex].bStraightThrough
				? 0.0 : Result.Corners[PrevIndex].ParamB;

			Result.Arms[Index].CutDistance = FMath::Max3(FromLeft, FromRight, 0.0);
		}

		Result.bValid = true;
	}

	// Cut vertices, computed ONCE here and shared verbatim with the boundary walk.
	for (int32 Index = 0; Index < ArmCount; ++Index)
	{
		const FJunctionArm& Arm = Input.Arms[Index];
		const FVector2D Normal = RoadGeom::PerpCCW(Arm.Tangent);
		const FVector2D CutCentre = Input.Position + Arm.Tangent * Result.Arms[Index].CutDistance;

		Result.Arms[Index].LeftCut  = CutCentre + Normal * FMath::Max(Arm.HalfWidthLeft, 0.0);
		Result.Arms[Index].RightCut = CutCentre - Normal * FMath::Max(Arm.HalfWidthRight, 0.0);
	}

	return Result;
}

void FJunctionSolver::SolveBoundary(const FJunctionInput& Input, FJunctionResult& InOutResult)
{
	// Implemented in Task 8.
}
