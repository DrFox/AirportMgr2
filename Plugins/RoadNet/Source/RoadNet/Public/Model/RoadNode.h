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

	/** Written ONLY by FJunctionSolver. Distance from each end at which the segment is cut. */
	UPROPERTY() double TrimA = 0.0;
	UPROPERTY() double TrimB = 0.0;

	UPROPERTY() int32 Generation = 0;
	UPROPERTY() bool  bAlive = false;
};
