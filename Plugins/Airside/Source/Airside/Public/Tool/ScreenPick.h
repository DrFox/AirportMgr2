#pragma once

#include "CoreMinimal.h"

/**
 * The one judgement in picking an aircraft: which of N screen points is nearest the cursor
 * and close enough. Pulled out of the controller so it can be tested with no camera - the
 * projection is the engine's job, the rule is ours.
 */
namespace ScreenPick
{
	/** Index of the nearest point within Radius (inclusive) of Cursor, or INDEX_NONE. */
	AIRSIDE_API int32 NearestWithin(TConstArrayView<FVector2D> Points, const FVector2D& Cursor, double Radius);
}
