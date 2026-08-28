#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "RoadNode.generated.h"

class URoadProfile;

USTRUCT()
struct ROADNET_API FRoadNode
{
	GENERATED_BODY()

	UPROPERTY() FVector2D Position = FVector2D::ZeroVector;

	/** Incident segments, maintained sorted by outgoing bearing, ascending in (-UE_DOUBLE_PI, UE_DOUBLE_PI]. */
	UPROPERTY() TArray<FRoadSegmentId> Incident;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};

USTRUCT()
struct ROADNET_API FRoadSegment
{
	GENERATED_BODY()

	UPROPERTY() FRoadNodeId A;
	UPROPERTY() FRoadNodeId B;

	/** Quadratic Bezier control point. Equals (A+B)/2 for a straight segment. */
	UPROPERTY() FVector2D Control = FVector2D::ZeroVector;

	UPROPERTY() TObjectPtr<URoadProfile> Profile = nullptr;

	/** Written ONLY by FRoadNetworkSolver. Distance from each end at which the segment is cut. */
	UPROPERTY() double TrimA = 0.0;
	UPROPERTY() double TrimB = 0.0;

	/**
	 * The segment's four end vertices, written ONLY by FRoadNetworkSolver.
	 *
	 * These are the SAME values the junction boundary polygon contains, stored rather
	 * than recomputed. A mesh builder that rebuilt them from Position + Tangent*Trim
	 * would be bitwise-identical only if its Tangent were the same double the solver
	 * received; one differing low bit reopens the seam. Never recompute these.
	 */
	UPROPERTY() FVector2D LeftCutA  = FVector2D::ZeroVector;
	UPROPERTY() FVector2D RightCutA = FVector2D::ZeroVector;
	UPROPERTY() FVector2D LeftCutB  = FVector2D::ZeroVector;
	UPROPERTY() FVector2D RightCutB = FVector2D::ZeroVector;

	/** True once a solve has written TrimA/LeftCutA/RightCutA - this segment's A end only. */
	UPROPERTY() bool bSolvedA = false;

	/** True once a solve has written TrimB/LeftCutB/RightCutB - this segment's B end only. */
	UPROPERTY() bool bSolvedB = false;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
