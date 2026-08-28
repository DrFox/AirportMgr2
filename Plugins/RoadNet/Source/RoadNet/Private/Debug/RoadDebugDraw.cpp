#include "Debug/RoadDebugDraw.h"

#include "DrawDebugHelpers.h"
#include "Solve/JunctionSolver.h"

namespace
{
	TAutoConsoleVariable<int32> CVarRoadDebugDraw(
		TEXT("road.DebugDraw"),
		1,
		TEXT("RoadNet debug drawing. 0 = off, 1 = boundary and cuts, 2 = adds solver internals."),
		ECVF_Default);

	FVector To3D(const FVector2D& Point, double Z)
	{
		return FVector(Point.X, Point.Y, Z);
	}
}

int32 RoadDebug::GetDebugDrawLevel()
{
	return CVarRoadDebugDraw.GetValueOnGameThread();
}

void RoadDebug::DrawJunction(UWorld* World, const FJunctionInput& Input, const FJunctionResult& Result, double ZHeight)
{
	const int32 Level = GetDebugDrawLevel();
	if (World == nullptr || Level <= 0 || !Result.bValid || Result.Boundary.Num() < 3)
	{
		return;
	}

	// Rim excludes the appended fan centre.
	const int32 RimCount = Result.Boundary.Num() - 1;

	// Boundary polygon in green.
	for (int32 Index = 0; Index < RimCount; ++Index)
	{
		const FVector Start = To3D(Result.Boundary[Index], ZHeight);
		const FVector End   = To3D(Result.Boundary[(Index + 1) % RimCount], ZHeight);
		DrawDebugLine(World, Start, End, FColor::Green, false, -1.0f, 0, 6.0f);
	}

	// Cut lines in cyan, cut vertices as spheres.
	for (const FJunctionArmResult& Arm : Result.Arms)
	{
		DrawDebugLine(World, To3D(Arm.RightCut, ZHeight), To3D(Arm.LeftCut, ZHeight),
			FColor::Cyan, false, -1.0f, 0, 10.0f);
		DrawDebugSphere(World, To3D(Arm.LeftCut,  ZHeight), 40.0f, 8, FColor::Cyan, false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Arm.RightCut, ZHeight), 40.0f, 8, FColor::Cyan, false, -1.0f, 0, 2.0f);
	}

	if (Level < 2)
	{
		return;
	}

	// Solver internals.
	for (int32 Index = 0; Index < Input.Arms.Num(); ++Index)
	{
		const FRay2D LeftEdge  = FJunctionSolver::MakeLeftEdge(Input, Index);
		const FRay2D RightEdge = FJunctionSolver::MakeRightEdge(Input, Index);
		const double Extent = 6000.0;

		DrawDebugLine(World,
			To3D(LeftEdge.Origin, ZHeight),
			To3D(LeftEdge.Origin + LeftEdge.Dir * Extent, ZHeight),
			FColor::Yellow, false, -1.0f, 0, 2.0f);
		DrawDebugLine(World,
			To3D(RightEdge.Origin, ZHeight),
			To3D(RightEdge.Origin + RightEdge.Dir * Extent, ZHeight),
			FColor::Orange, false, -1.0f, 0, 2.0f);
	}

	for (const RoadGeom::FFillet& Corner : Result.Corners)
	{
		if (!Corner.bValid || Corner.bStraightThrough)
		{
			continue;
		}
		DrawDebugSphere(World, To3D(Corner.Corner,   ZHeight), 60.0f, 8, FColor::Red,     false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Corner.Centre,   ZHeight), 50.0f, 8, FColor::Magenta, false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Corner.TangentA, ZHeight), 35.0f, 8, FColor::White,   false, -1.0f, 0, 2.0f);
		DrawDebugSphere(World, To3D(Corner.TangentB, ZHeight), 35.0f, 8, FColor::White,   false, -1.0f, 0, 2.0f);
	}
}
