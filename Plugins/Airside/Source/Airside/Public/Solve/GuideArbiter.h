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
	 * WHAT a guide means. Declaration order is the tiebreak WITHIN a fit kind - see Arbitrate.
	 *
	 * A PLAIN ENUM, not a UENUM, for the reason ESource carried before it: UHT cannot see an
	 * enum without a .generated.h, and a Solve/ header may not have one. FSnapGuideSettings
	 * wraps it in named bools rather than a reflected array for the same reason.
	 *
	 * ORDER IS MOST-SPECIFIC-FIRST: what you are extending is what you are thinking about;
	 * matching a neighbour's gap is the most incidental thing on the list.
	 */
	enum class ERelation : uint8
	{
		Extending,
		LevelWith,
		Parallel,
		Collinear,
		MatchingGap
	};

	/**
	 * WHAT a guide is measured against. Declaration order breaks ties within one relation.
	 *
	 * SPLIT OUT OF ESource ON 2026-09-20. ESource mixed these two axes: Extending, PointAlign,
	 * Collinear, Parallel and Offset named relationships, while Runway, World and Aligned named
	 * references - and Parallel and Collinear carried an unnamed, unswitchable reference, "a
	 * road". A player switching Runway off still saw "parallel to runway 18/36", because there
	 * was no axis for the toggle to act along. See the 2026-09-20 guide-grid design section 1.
	 *
	 * ThisGesture RANKS FIRST because the shape under the cursor is more specific than anything
	 * already on the field; World ranks last because it is what you fall back on.
	 */
	enum class EReference : uint8
	{
		ThisGesture,
		Road,
		Runway,
		Apron,
		Stand,
		World
	};

	/**
	 * Whether this pair of axes names a guide that exists. Design section 3's grid.
	 *
	 * THE ONE PLACE THE GRID IS WRITTEN DOWN. FSnapGuideSettings::IsEnabled consults it, the
	 * registry test walks it, and Airside.Tool.GuideGridHasNoCellOutsideTheList asserts no
	 * source can propose a pair it rejects. Sixteen of the thirty pairs are legal; the holes
	 * are reasoned about one by one in the design, not merely left out.
	 */
	AIRSIDE_API bool IsLegalCell(ERelation Relation, EReference Reference);

	/**
	 * How a candidate is judged near.
	 *
	 * TWO KINDS, because two genuinely different questions are being asked. "Square to the
	 * frontage" is about the DIRECTION the drag went, and its tolerance is an angle. "Level
	 * with corner 3" is about where the cursor ENDED UP relative to a line that may be
	 * nowhere near the origin, and its tolerance is a distance. Forcing one measure on both
	 * would mean inventing an exchange rate between degrees and uu and hiding it inside the
	 * flicker rule.
	 */
	enum class EFit : uint8
	{
		/** Eligible when the cursor's direction from the origin is within ToleranceDegrees. */
		Angular,

		/** Eligible when the cursor is within ToleranceUu of the line, however it got there. */
		Perpendicular
	};

	/** One thing the cursor could line up with. */
	struct FCandidate
	{
		/** Unit. With Through below, this is the candidate's LINE. */
		FVector2D Direction = FVector2D(1.0, 0.0);

		/**
		 * The point the candidate's line passes THROUGH.
		 *
		 * Every source but PointAlign fills this with the drag's own origin - which is what
		 * made it implicit before 2026-09-17, when Direction alone was the whole answer. An
		 * alignment to another point is a line through THAT point, and the origin is nowhere
		 * on it, so a line has to carry its own.
		 */
		FVector2D Through = FVector2D::ZeroVector;

		/** Which of the two tolerances judges this candidate. See EFit. */
		EFit Fit = EFit::Angular;

		/**
		 * The point the dashed line is drawn TO - the road it is parallel with, the edge it
		 * is squared to. NOT the guide's own geometry: the player needs to see WHICH thing
		 * they are lining up with, which is the whole of Cities Skylines' advantage here.
		 */
		FVector2D ReferenceAt = FVector2D::ZeroVector;

		/** "square to the frontage", "45 degrees". Shown beside the line. */
		FString Description;

		/**
		 * WHAT this guide means, and WHAT it is measured against. Two fields since 2026-09-20;
		 * one `ESource` before, which is why a player could switch Runway off and still be told
		 * their taxiway was parallel to one - see EReference's own comment.
		 *
		 * THE DEFAULT IS THE WORLD GRID, exactly as `ESource::World` was: a candidate built
		 * without saying what it is should be the least specific thing on the list, never the
		 * most.
		 */
		ERelation Relation = ERelation::Parallel;
		EReference Reference = EReference::World;
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
		/** How far off an Angular candidate the cursor may be and still be offered it. Degrees. */
		double ToleranceDegrees = 7.0;

		/** How much better an Angular challenger must be to take the guide off the incumbent. */
		double StickinessDegrees = 2.0;

		/** How near a Perpendicular candidate's line counts as lined up with it. uu - 3 m. */
		double ToleranceUu = 300.0;

		/** The same rule as StickinessDegrees, in the units the other fit kind is measured in. */
		double StickinessUu = 100.0;

		/**
		 * How far the INTERSECTION of two winners may be from the cursor before the
		 * perpendicular one is given up. uu - 10 m.
		 *
		 * Two nearly parallel lines meet a kilometre away, and a corner that leapt there
		 * would be obeying a rule the player cannot see. NO PIE PASS YET - see design §11.
		 */
		double MaxPullUu = 1000.0;

		/**
		 * How far from the drag a network source will look for something to line up with. uu -
		 * 100 m.
		 *
		 * WITHOUT A REACH, every road on the field proposes and the nearest-wins race is
		 * decided by geometry the player cannot see - a taxiway half a kilometre away winning
		 * because it happened to be a degree closer. Runways are deliberately exempt (see
		 * FRunwayGuideSource): an airport squares to its runways from anywhere on it.
		 *
		 * NO PIE PASS YET.
		 */
		double SearchRadiusUu = 10000.0;
	};

	struct FResult
	{
		bool bActive = false;

		/**
		 * Every guide holding this frame - AT MOST ONE PER FIT KIND, so at most two.
		 *
		 * ONE LIST, not a Winner plus an also-ran: once the point is their intersection
		 * neither is privileged, and two named fields would be two things to keep in step.
		 * Inline-allocated because the cap is structural, not a guess.
		 */
		TArray<FCandidate, TInlineAllocator<2>> Winners;

		/** Where the constrained point ended up, which is what the tool uses. Left at zero
		 *  while bActive is false - a caller must branch on the flag, never read past it. */
		FVector2D Point = FVector2D::ZeroVector;

		/** The winner of this fit kind, or null. For a caller that wants one specifically. */
		const FCandidate* Of(EFit Fit) const
		{
			return Winners.FindByPredicate([Fit](const FCandidate& C) { return C.Fit == Fit; });
		}
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
