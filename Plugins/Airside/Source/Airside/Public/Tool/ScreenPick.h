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

	/** One pickable body: a box in its own frame, and the frame that places it in the world. */
	struct FPickBox
	{
		FBox LocalBox = FBox(ForceInit);
		FTransform LocalToWorld = FTransform::Identity;
	};

	/**
	 * Index of the box the segment Start->End enters FIRST, or INDEX_NONE. A segment that
	 * starts inside a box enters it at once. An invalid (empty) box is never hit.
	 *
	 * THE WHOLE BODY, not a point on it (2026-09-27): an aircraft used to be picked within
	 * AgentPickPixels of its actor origin, which is its nose gear, so the player had to find a
	 * 24-pixel spot under the nose of an aeroplane filling half the screen. A ray against the
	 * body's box picks it wherever it is drawn - wing, tail or fuselage - and the nearest entry
	 * wins, so the aircraft in front is the one clicked when two overlap on screen.
	 *
	 * The box's corners past the swept wings count as the aircraft. Rejected alternative: a
	 * per-triangle trace, which would need collision on the agent meshes (ARoadAgentActor
	 * builds them with NoCollision), for a precision the player never asked for.
	 */
	AIRSIDE_API int32 FirstBoxHit(TConstArrayView<FPickBox> Boxes, const FVector& Start, const FVector& End);
}
