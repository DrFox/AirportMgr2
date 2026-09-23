#pragma once

#include "CoreMinimal.h"
#include "Solve/IcaoCode.h"

/**
 * A drawn stand rectangle and the aircraft's stop-mark pose, as ONE geometry read both ways.
 *
 * The player draws a rectangle off a taxiway - an entrance edge and how far it was dragged
 * inward - and that single gesture has to become both the pose an arrival stops at
 * (PoseFor) and, read back from a saved box, the letter and dimensions that pose implies
 * (WidthOf/DepthOf/LetterOf). BoxAt is PoseFor's exact inverse: building a box from a pose
 * and a letter, then reading that box's letter back, must return the letter it was built
 * from - that round trip is what the commit and migration tasks after this one depend on.
 *
 * Dependency-free like the rest of Solve/: CoreMinimal.h and Solve/ only, no engine types
 * beyond it.
 */
namespace StandBox
{
	/** The aircraft's stop-mark pose: Position is the nose-gear stop mark, Facing is the
	 *  unit nose direction (away from the taxiway the stand opens off). */
	struct FStandPose
	{
		FVector2D Position = FVector2D::ZeroVector;
		FVector2D Facing = FVector2D(1, 0);
	};

	/**
	 * The stop-mark pose for a stand entered along EntranceA->EntranceB, dragged Inward.
	 *
	 * THE TEMPLATE'S BACK EDGE (X = NoseFwd - Depth, the tail side) IS LAID ON THE ENTRANCE
	 * EDGE, centred - see UEntityDefinition::BuildStandTemplate. So the stop mark sits
	 * Depth - NoseFwd in from the entrance edge, along Inward: every metre the player drew
	 * beyond the aircraft's own floor is apron past the nose, not room the airframe uses.
	 *
	 * Inward need not be perpendicular to EntranceA-EntranceB or unit length; only its
	 * direction is read (GetSafeNormal), so a freeform drag still yields a clean pose.
	 */
	AIRSIDE_API FStandPose PoseFor(const FVector2D& EntranceA, const FVector2D& EntranceB,
		const FVector2D& Inward, EIcaoCode Letter);

	/**
	 * PoseFor's inverse: the four corners of the letter's box at this pose, entrance edge
	 * first. OutCorners is entrance-A, entrance-B, then the two corners inward of them, so
	 * the entrance edge is corners 0->1 and PolygonArea is POSITIVE - the winding the pad
	 * triangulator needs.
	 */
	AIRSIDE_API void BoxAt(const FStandPose& Pose, EIcaoCode Letter, TArray<FVector2D>& OutCorners);

	/** |Rect[1] - Rect[0]| - the entrance edge's own length. Rect convention: entrance edge
	 *  0->1, then inward to 2 and 3, as BoxAt produces and a drawn stand is saved. */
	AIRSIDE_API double WidthOf(TArrayView<const FVector2D> Rect);

	/** |Rect[2] - Rect[1]| - how far the entrance edge was dragged inward. */
	AIRSIDE_API double DepthOf(TArrayView<const FVector2D> Rect);

	/**
	 * The letter a drawn rectangle reads as - IcaoCode::LetterForStandSize on
	 * WidthOf/DepthOf, parsed back to the enum. Unset for a Rect with fewer than four points
	 * (nothing to measure) or one too small for any letter (LetterForStandSize's own empty
	 * answer - see its header for why that is refused rather than rounded up to Code A).
	 */
	AIRSIDE_API TOptional<EIcaoCode> LetterOf(TArrayView<const FVector2D> Rect);
}
