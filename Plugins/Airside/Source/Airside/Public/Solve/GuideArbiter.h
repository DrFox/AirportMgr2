#pragma once

#include "CoreMinimal.h"

/**
 * What the cursor could be lining up with, and which of those wins.
 *
 * DEPENDENCY-FREE like every Solve/ header - CoreMinimal.h and nothing else. The chain that
 * FILLS these lives in Tool/SnapGuideChain.h, because four of the seven sources must query
 * URoadNetwork; the arbitration itself must not, which is what makes every flicker case in
 * the 2026-09-17 snap-guides design section 5 a world-free test.
 */
namespace SnapGuide
{
	/**
	 * The sources, in priority order - most specific to what the player is doing, first.
	 *
	 * THE ORDER IS THE TIEBREAK AND NOTHING ELSE: a lower source still wins outright when it
	 * is the only one in tolerance. What you are extending is what you are thinking about;
	 * the world grid is what you fall back on when nothing else applies.
	 *
	 * ALL SEVEN ARE LISTED although stage 1 fills only Extending and World, because the
	 * ORDER is the contract - adding Parallel in stage 2 must not renumber what Runway means.
	 *
	 * A PLAIN ENUM, not a UENUM: UHT cannot see an enum without a .generated.h, and a Solve/
	 * header may not have one. Stage 3's toggles need reflection and will wrap it there.
	 */
	enum class ESource : uint8
	{
		Extending,
		Aligned,
		Collinear,
		Parallel,
		Runway,
		World,
		Offset
	};

	/** One thing the cursor could line up with. */
	struct FCandidate
	{
		/** Unit, and for a direction guide this is the whole answer. */
		FVector2D Direction = FVector2D(1.0, 0.0);

		/** For Offset: how far along the perpendicular, uu. Zero for direction guides, and
		 *  unread until stage 4 - carried now so the type does not change under stage 2. */
		double Distance = 0.0;

		/**
		 * The point the dashed line is drawn TO - the road it is parallel with, the edge it
		 * is squared to. NOT the guide's own geometry: the player needs to see WHICH thing
		 * they are lining up with, which is the whole of Cities Skylines' advantage here.
		 */
		FVector2D ReferenceAt = FVector2D::ZeroVector;

		/** "square to the frontage", "45 degrees". Shown beside the line. */
		FString Description;

		ESource Source = ESource::World;
	};

	/**
	 * Placement feel, judged in PIE - design section 11.
	 *
	 * CONSTANTS FOR NOW, not UAirsideSettings knobs: these are numbers nobody has yet had a
	 * reason to move, and the route ClearanceUu took (constant first, property when tuning
	 * demanded it) is the one this follows. Passed as a struct rather than read from a
	 * global so a test can state the numbers it depends on instead of inheriting them.
	 */
	struct FTuning
	{
		/** How far off a candidate the cursor may be and still be offered it. Degrees. */
		double ToleranceDegrees = 7.0;

		/** How much better a challenger must be before it takes the guide off the incumbent. */
		double StickinessDegrees = 2.0;
	};

	struct FResult
	{
		bool bActive = false;

		FCandidate Winner;

		/** Where the constrained point ended up, which is what the tool uses. Left at zero
		 *  while bActive is false - a caller must branch on the flag, never read past it. */
		FVector2D Point = FVector2D::ZeroVector;
	};

	/**
	 * Which candidate the cursor is lined up with, and where that puts it.
	 *
	 * PURE, and that is the whole reason this lives apart from the chain: no network, no
	 * world, no frame. Previous is the last answer given and is what stops the guide
	 * flickering between two candidates a degree apart; pass a default-constructed FResult
	 * when there was none.
	 *
	 * A GUIDE IS A LINE, NOT A RAY. Error is the ACUTE angle between the cursor's direction
	 * and the candidate's, so a candidate and its opposite are ONE guide. That is why the
	 * World source proposes four directions rather than eight, and why a corner dragged to
	 * the far side of its origin still gets the square rather than losing the guide at the
	 * moment it crosses.
	 *
	 * HYSTERESIS IS MEASURED AGAINST THE PREVIOUS WINNER, not the previous cursor. A player
	 * dragging slowly past two near-equal candidates should feel one guide hold and then
	 * hand over; a cursor-delta rule produces the rapid alternation this exists to stop.
	 */
	AIRSIDE_API FResult Arbitrate(TConstArrayView<FCandidate> Candidates,
		const FVector2D& Origin, const FVector2D& Cursor,
		const FResult& Previous, const FTuning& Tuning = FTuning());
}
