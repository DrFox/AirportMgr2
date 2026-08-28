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
	InOutResult.Boundary.Reset();
	InOutResult.Triangles.Reset();

	if (!InOutResult.bValid)
	{
		return;
	}

	constexpr double WeldTolerance = 1e-6;

	// Arc endpoints and cut vertices are mathematically the same point but reached by
	// different floating-point paths, so they are not bitwise equal. The cut vertex is
	// the shared truth, so it always wins: a coincident arc sample is replaced, never
	// appended alongside. This is what keeps the junction and its segments welded.
	auto AddArcPoint = [&InOutResult](const FVector2D& Point)
	{
		if (InOutResult.Boundary.Num() > 0 &&
			InOutResult.Boundary.Last().Equals(Point, WeldTolerance))
		{
			return;
		}
		InOutResult.Boundary.Add(Point);
	};

	auto AddCutVertex = [&InOutResult](const FVector2D& Point)
	{
		if (InOutResult.Boundary.Num() > 0 &&
			InOutResult.Boundary.Last().Equals(Point, WeldTolerance))
		{
			InOutResult.Boundary.Last() = Point;   // exact value replaces the approximation
			return;
		}
		InOutResult.Boundary.Add(Point);
	};

	const int32 ArmCount = Input.Arms.Num();
	TArray<FVector2D> ArcSamples;

	for (int32 Index = 0; Index < ArmCount; ++Index)
	{
		// The segment's cut line, traversed right-to-left so the interior stays on the
		// left and the polygon comes out counter-clockwise.
		AddCutVertex(InOutResult.Arms[Index].RightCut);
		AddCutVertex(InOutResult.Arms[Index].LeftCut);

		if (ArmCount == 1)
		{
			continue;  // dead end: no corner to round
		}

		const RoadGeom::FFillet& Corner = InOutResult.Corners[Index];
		if (Corner.bStraightThrough)
		{
			continue;  // collinear: the next arm's cut vertex joins directly
		}

		ArcSamples.Reset();
		RoadGeom::SampleArc(Corner, Input.ArcSegments, ArcSamples);
		for (const FVector2D& Sample : ArcSamples)
		{
			AddArcPoint(Sample);
		}
	}

	// Close the ring: the first vertex may coincide with the last arc sample.
	if (InOutResult.Boundary.Num() > 1 &&
		InOutResult.Boundary.Last().Equals(InOutResult.Boundary[0], WeldTolerance))
	{
		InOutResult.Boundary.Pop();
	}

	// The fan centre is appended last so rim indices stay stable for callers.
	InOutResult.Centre = Input.Position;
	const int32 CentreIndex = InOutResult.Boundary.Add(Input.Position);
	const int32 RimCount = CentreIndex;

	if (RimCount < 3)
	{
		// A dead end contributes only two cut vertices: there is no fan to build.
		// End-cap geometry belongs to the mesh builder, which is where it is first
		// needed. The boundary is still populated so debug draw can show the cut.
		return;
	}

	InOutResult.Triangles.Reserve(RimCount * 3);
	for (int32 Index = 0; Index < RimCount; ++Index)
	{
		InOutResult.Triangles.Add(CentreIndex);
		InOutResult.Triangles.Add(Index);
		InOutResult.Triangles.Add((Index + 1) % RimCount);
	}
}
