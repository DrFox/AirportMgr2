#pragma once

#include "CoreMinimal.h"
#include "Solve/RoadGeom.h"

/** One segment arriving at a junction. */
struct FJunctionArm
{
	/** Normalised, pointing away from the node. */
	FVector2D Tangent = FVector2D(1.0, 0.0);

	double HalfWidthLeft  = 0.0;
	double HalfWidthRight = 0.0;
	double FilletRadius   = 0.0;

	/** Opaque caller tag, e.g. a packed FRoadSegmentId index. Never read by the solver. */
	int32 UserData = INDEX_NONE;
};

struct FJunctionInput
{
	FVector2D Position = FVector2D::ZeroVector;

	/** MUST be sorted ascending by CCW bearing of Tangent. */
	TArray<FJunctionArm> Arms;

	/** Points sampled per fillet arc. */
	int32 ArcSegments = 8;
};

struct FJunctionArmResult
{
	/** Distance from the node at which the segment is cut, perpendicular to its centreline. */
	double CutDistance = 0.0;

	FVector2D LeftCut  = FVector2D::ZeroVector;
	FVector2D RightCut = FVector2D::ZeroVector;
};

struct FJunctionResult
{
	bool bValid = false;

	/** Parallel to FJunctionInput::Arms. */
	TArray<FJunctionArmResult> Arms;

	/** Corner i lies between arm i's LEFT edge and arm (i+1)'s RIGHT edge. */
	TArray<RoadGeom::FFillet> Corners;

	/** Closed CCW boundary polygon, with the fan centre appended last. Filled by SolveBoundary. */
	TArray<FVector2D> Boundary;

	/** Triangle fan indices into Boundary. Filled by SolveBoundary. */
	TArray<int32> Triangles;

	/** Fan centre. Appended to Boundary by SolveBoundary. */
	FVector2D Centre = FVector2D::ZeroVector;
};

/**
 * Owns the boundary of a node.
 *
 * The contract that makes seams structurally impossible: segments never compute
 * where they stop. SolveCuts writes each arm's cut distance and BOTH cut vertices,
 * and SolveBoundary assembles the junction polygon from those exact same values.
 * Neither side ever recomputes the other's geometry, so the shared vertices are
 * bitwise identical rather than merely close.
 */
class ROADNET_API FJunctionSolver
{
public:
	static FJunctionResult SolveCuts(const FJunctionInput& Input);
	static void SolveBoundary(const FJunctionInput& Input, FJunctionResult& InOutResult);

	/** Left edge of an arm, as a ray originating at the node. */
	static FRay2D MakeLeftEdge(const FJunctionInput& Input, int32 ArmIndex);

	/** Right edge of an arm, as a ray originating at the node. */
	static FRay2D MakeRightEdge(const FJunctionInput& Input, int32 ArmIndex);
};
