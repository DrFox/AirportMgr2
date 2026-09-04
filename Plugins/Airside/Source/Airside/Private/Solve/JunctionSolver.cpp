#include "Solve/JunctionSolver.h"

namespace
{
	/**
	 * True only when EVERY triangle (Apex, Rim[i], Rim[i+1]) winds counter-clockwise,
	 * i.e. the apex can see the whole rim and the fan does not overlap itself.
	 *
	 * A strictly positive cross product is required: a zero means the apex is collinear
	 * with that rim edge, which produces a degenerate triangle and, one step further,
	 * an inverted one.
	 */
	bool IsFanCounterClockwise(TArrayView<const FVector2D> Rim, const FVector2D& Apex)
	{
		const int32 Count = Rim.Num();
		if (Count < 3)
		{
			return false;
		}

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D EdgeA = Rim[Index] - Apex;
			const FVector2D EdgeB = Rim[(Index + 1) % Count] - Apex;
			if (EdgeA.X * EdgeB.Y - EdgeA.Y * EdgeB.X <= 0.0)
			{
				return false;
			}
		}
		return true;
	}
}

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
			if (Input.Arms[Index].bContinuous)
			{
				// PASSES THROUGH, so it is not trimmed at all. Its cut vertices then come
				// from the node position and its own half-widths alone, which is what makes
				// two opposite continuous arms share them bitwise - see FJunctionArm.
				Result.Arms[Index].CutDistance = 0.0;
				continue;
			}

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

	// Which boundary slots hold a cut vertex rather than an arc sample. A cut vertex is
	// shared verbatim with the segment mesh, so it may never be dropped by the welding
	// or the ring close; an arc sample is ours alone and may.
	TArray<int32> CutVertexSlots;

	auto AddCutVertex = [&InOutResult, &CutVertexSlots](const FVector2D& Point)
	{
		if (InOutResult.Boundary.Num() > 0 &&
			InOutResult.Boundary.Last().Equals(Point, WeldTolerance))
		{
			InOutResult.Boundary.Last() = Point;   // exact value replaces the approximation
			CutVertexSlots.AddUnique(InOutResult.Boundary.Num() - 1);
			return;
		}
		CutVertexSlots.Add(InOutResult.Boundary.Add(Point));
	};

	const int32 ArmCount = Input.Arms.Num();
	TArray<FVector2D> ArcSamples;

	for (int32 Index = 0; Index < ArmCount; ++Index)
	{
		// A CONTINUOUS ARM CONTRIBUTES NO RIM. Its "cut line" is the node's own cross
		// section, which lies INSIDE the surface it is passing through - emitting it walks
		// the boundary up across the runway and back, a zero-width spike that a shoelace
		// area cancels out and a triangle fan happily paves. Measured: the rim reached the
		// far edge of a 45 m runway while reporting exactly the area of the taxiway lens.
		//
		// Leaving it out, the ring closes along the arm's edge between the two fillet
		// tangents, which is the lens the taxiway actually needs paving.
		//
		// This does not weaken the weld contract, it moves it: an uncut segment has no rim
		// vertex to weld TO, because it does not end here. It welds to the collinear segment
		// opposite, whose cut vertices are computed from the same node and the same
		// half-widths and so are bitwise identical - see FJunctionArm::bContinuous.
		if (!Input.Arms[Index].bContinuous)
		{
			// The segment's cut line, traversed right-to-left so the interior stays on the
			// left and the polygon comes out counter-clockwise.
			AddCutVertex(InOutResult.Arms[Index].RightCut);
			AddCutVertex(InOutResult.Arms[Index].LeftCut);
		}

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

	// Close the ring: the first vertex may coincide with the last point added. Only an
	// arc sample may be dropped here. Popping a cut vertex would leave a segment end
	// with no bitwise-identical rim vertex to weld to, which is the entire failure this
	// solver exists to prevent - and it is not hypothetical: a collinear pass-through
	// node's last cut vertex coincides with its first to within the weld tolerance.
	{
		const int32 LastSlot = InOutResult.Boundary.Num() - 1;
		if (LastSlot > 0 &&
			!CutVertexSlots.Contains(LastSlot) &&
			InOutResult.Boundary[LastSlot].Equals(InOutResult.Boundary[0], WeldTolerance))
		{
			InOutResult.Boundary.Pop();
		}
	}

	const int32 RimCount = InOutResult.Boundary.Num();

	// Pick a fan apex that can see the whole rim.
	//
	// The node itself is the natural choice and is correct for every node of 3 or more
	// arms. It is NOT correct for a 2-arm node whose arms are separated by less than
	// roughly 28 degrees: the boundary is then a long lobe lying entirely AHEAD of the
	// node, so the node sits outside its own rim, the fan self-overlaps and some
	// triangles come out clockwise. The rim's centroid recovers every such case.
	//
	// If neither apex works we emit no triangles at all. A correct empty result beats a
	// silently inverted one. The general fix is ear-clipping (design spec section 5.7);
	// that is the deliberate follow-up, not implemented here.
	FVector2D Apex = Input.Position;
	bool bFanIsCounterClockwise = false;

	if (RimCount >= 3)
	{
		const TArrayView<const FVector2D> Rim(InOutResult.Boundary.GetData(), RimCount);

		bFanIsCounterClockwise = IsFanCounterClockwise(Rim, Apex);
		if (!bFanIsCounterClockwise)
		{
			FVector2D Sum = FVector2D::ZeroVector;
			for (const FVector2D& Point : Rim)
			{
				Sum += Point;
			}
			const FVector2D Centroid = Sum / static_cast<double>(RimCount);

			if (IsFanCounterClockwise(Rim, Centroid))
			{
				Apex = Centroid;
				bFanIsCounterClockwise = true;
			}
		}
	}

	// The fan apex is appended last so rim indices stay stable for callers: the rim is
	// Boundary.Num() - 1 points and Triangles indexes into Boundary.
	InOutResult.Centre = Apex;
	const int32 CentreIndex = InOutResult.Boundary.Add(Apex);

	if (RimCount < 3)
	{
		// A dead end contributes only two cut vertices: there is no fan to build.
		// End-cap geometry belongs to the mesh builder, which is where it is first
		// needed. The boundary is still populated so debug draw can show the cut.
		return;
	}

	if (!bFanIsCounterClockwise)
	{
		// No apex sees the whole rim. Boundary is still populated; Triangles stays empty.
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
