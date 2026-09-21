#pragma once

#include "CoreMinimal.h"

struct FJunctionInput;
struct FJunctionResult;
class UWorld;

namespace RoadDebug
{
	/** 0 = off, 1 = boundary and cuts, 2 = adds solver internals. */
	AIRSIDE_API int32 GetDebugDrawLevel();

	/**
	 * Draw one solved junction.
	 *
	 * Thickness is in world units, not pixels. The gallery spans ~150,000 uu, so the
	 * 6 uu default that reads fine up close is a small fraction of a pixel when the
	 * whole gallery is in frame - it looks like nothing was drawn at all.
	 */
	AIRSIDE_API void DrawJunction(UWorld* World, const FJunctionInput& Input, const FJunctionResult& Result, double ZHeight, double Thickness = 6.0);
}
