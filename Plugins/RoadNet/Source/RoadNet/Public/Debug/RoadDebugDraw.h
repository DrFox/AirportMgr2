#pragma once

#include "CoreMinimal.h"

struct FJunctionInput;
struct FJunctionResult;
class UWorld;

namespace RoadDebug
{
	/** 0 = off, 1 = boundary and cuts, 2 = adds solver internals. */
	ROADNET_API int32 GetDebugDrawLevel();

	ROADNET_API void DrawJunction(UWorld* World, const FJunctionInput& Input, const FJunctionResult& Result, double ZHeight);
}
