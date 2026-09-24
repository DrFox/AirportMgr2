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

		/**
		 * A direction, AND ITS PERPENDICULAR - "parallel to the taxiway" and "square to the
		 * taxiway" are one fact about one road, so one toggle governs both. For the World
		 * column it is neither: the four compass axes are parallel to no thing at all.
		 *
		 * THE BAR CALLS THIS "Direction", and the names differ deliberately. A player switched
		 * on Angled from and World on 2026-09-20, got nothing, and observed that a button
		 * marked "Parallel" also showed them "square" - which is not parallel - while the thing
		 * they wanted, the world grid, was neither. The design's own grid had always called the
		 * row "Parallel / square"; the button had kept the first word and dropped the rest.
		 *
		 * THE ENUM WAS LEFT ALONE rather than renamed with it. "Parallel" is 34 sites across 11
		 * files, plus FParallelGuideSource and eighteen uses of bParallel - and a good number of
		 * those are comments that REASON about Parallel by name, which a substitution would
		 * flatten. A name only developers read did not justify that; this paragraph is the tie
		 * between the two instead. See BuildActions.cpp's snap.direction.
		 */
		Parallel,

		Collinear,

		/**
		 * "45 degrees to that taxiway" - a line radiating out of a reference's END, at an angle
		 * to the reference itself. Added 2026-09-20 on a sketch (samples/suggestion.png).
		 *
		 * COLLINEAR IS THE 0 DEGREE MEMBER OF THIS FAMILY, which is what fixes its shape: both
		 * are a line through a reference's end at some angle to it, and both are judged by where
		 * the cursor ENDED UP. It ranks just below Collinear for the same reason - the line a
		 * road lies on is more specific than a line merely angled off it.
		 */
		AngledFrom,

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

		/**
		 * TWO COLUMNS, NOT ONE "Road" - split on 2026-09-20, the day after the axes were.
		 *
		 * Nothing else in this codebase has ever called these one thing: two registry entries
		 * under two keys (1 and 9), two cross-sections, two traversal classes, and
		 * RoadNaming::Describe already put "the taxiway" or "the service road" in the LABEL.
		 * Only the grid collapsed them - so a line reading "parallel to the service road"
		 * appeared under a button marked Road, and no toggle could reach one without the other.
		 *
		 * TAXIWAY FIRST, matching ERoadKind and the keys. Declaration order breaks ties, and an
		 * aircraft lane is the more central thing on an airfield than the van road beside it.
		 */
		Taxiway,
		ServiceRoad,

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
	 * source can propose a pair it rejects. Twenty-six of the forty-two pairs are legal; the
	 * holes are reasoned about one by one at each row below, not merely left out.
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

	/**
	 * WHICH TEMPLATE a label reads from - see FGuideLabel. One member per distinct string shape
	 * the 13 `FString::Printf` sites named in #183 used to build, so Describe can switch on this
	 * instead of every caller carrying its own format string.
	 *
	 * DegreesTo COVERS BOTH "0 degrees to %s" (FPointAlignGuideSource's Level) and the diagonal
	 * "%d degrees to %s" AddDirections used for 45 and 135: the two were always the same template
	 * at Degrees=0, and giving PointAlign its own member would be a second name for one string.
	 */
	enum class ELabelKind : uint8
	{
		/** "<Verb> <Name>" - FGuideLabel::Verb + the resolved name. */
		Along,
		/** "square to <Name>". */
		SquareTo,
		/** "<Degrees> degrees to <Name>", Degrees printed even at 0. */
		DegreesTo,
		/** "<Degrees> degrees from the end of <Name>" - AddSpokes. */
		AngledFromEnd,
		/** "in line with <Name>". */
		InLineWith,
		/** "edge flush with <Name>". */
		EdgeFlushWith,
		/** "<GapUu, in metres> m, matching <Name>" - FOffsetGuideSource. */
		MatchingGap,
		/** FGuideLabel::Text IS the whole string - a compile-time literal, so a pointer costs
		 *  nothing and needs no <Name> at all. The world axes use this. (The apron corner's two
		 *  fixed strings did until 2026-09-24; they are ApronCorner + DegreesTo/SquareTo now,
		 *  byte-identical, so a plot's corner could share their recipe.) */
		Literal,
		/**
		 * "level with <Name>" - FApronCornerGuideSource with NO gesture reference, since
		 * 2026-09-24. "0 degrees to the corner" is measured from the gesture's own direction, and
		 * a free start has none; the line then runs along the corner's own edge, and this is the
		 * one honest thing to call it.
		 */
		LevelWith,
	};

	/**
	 * WHERE <Name> COMES FROM - see ELabelKind. Solve/ may not know a URoadNetwork or a
	 * FGuideAnchor (this header's own "DEPENDENCY-FREE" banner), so a label cannot carry the
	 * resolved FString itself; it carries enough to ask the Tool/ layer that built the candidate,
	 * which still has both. See SnapGuide::Describe in Tool/SnapGuideLabel.h.
	 */
	enum class ELabelSubject : uint8
	{
		/** No <Name> to resolve - ELabelKind::Literal supplies the whole string via Text. */
		None,
		/** RoadNaming::Describe(Network, Network.SegmentIdAt(SegmentIndex)) - see that field. */
		Segment,
		/** FGuideAnchor::ReferenceName - the anchor carries exactly one, so no index is needed. */
		GestureReference,
		/** FGuideAnchor::AlignTo[SubjectIndex].Name. */
		GesturePoint,
		/** EntityNaming::Describe(Network.GetEntities()[SubjectIndex]). */
		Entity,
		/** The fixed "the apron edge" text - FApronSurface carries no name of its own. */
		ApronEdge,
		/** The fixed "the apron corner" text, for the same reason. */
		ApronCorner,
		/**
		 * "the stand's edge" / "the fuel depot's edge", by the KIND of Network.GetEntities()
		 * [SubjectIndex] - added 2026-09-24 with the plot outline sources.
		 *
		 * THE KIND, NOT EntityNaming::Describe's display name: an edge is a PART of a stand or a
		 * depot, and "the Code C Stand's edge" reads as the name of a thing rather than a part of
		 * one. The dashed line says WHICH, exactly as it does for the apron.
		 */
		EntityEdge,
		/** "the stand's corner" / "the fuel depot's corner" - see EntityEdge. */
		EntityCorner,
	};

	/**
	 * WHAT Description WOULD SAY, if this candidate wins - the INGREDIENTS, not the FString.
	 *
	 * #183: 13 `FString::Printf` sites paid for every candidate `Propose` emitted - every spoke
	 * off every segment in reach, every direction off every reference - when `Arbitrate` keeps at
	 * most two. A label costs nothing to fill (an enum, an int, a pointer with STATIC duration -
	 * never a heap string), so `Propose` can fill one per candidate for free and the one real
	 * `FString::Printf` call happens only for the winners, in `SnapGuide::Describe`.
	 *
	 * A CANDIDATE MAY STILL SET Description DIRECTLY instead of a Label - GuideArbiterTest builds
	 * candidates by hand to test Arbitrate in isolation from Tool/, and Arbitrate itself never
	 * reads Label; it only ever copies whichever candidates it keeps, Label and Description alike.
	 */
	struct FGuideLabel
	{
		ELabelKind Kind = ELabelKind::Literal;
		ELabelSubject Subject = ELabelSubject::None;

		/** AddSpokes' angle, AddDirections' diagonal, or PointAlign's fixed 0 - degrees, not
		 *  radians, and never read outside ELabelKind::DegreesTo/AngledFromEnd. */
		int32 Degrees = 0;

		/** FOffsetGuideSource's gap, uu - only ELabelKind::MatchingGap reads it. */
		double GapUu = 0.0;

		/**
		 * The PLAIN ARRAY INDEX a FRoadSegmentId was read from - not the handle itself, and NOT
		 * paired with a copied Generation: Model/RoadHandles.h needs its own .generated.h, and
		 * this header may not gain one (the DEPENDENCY-FREE banner at the top of this file), so
		 * FRoadSegmentId cannot be a member here at all. Carrying the bare index and asking
		 * URoadNetwork::SegmentIdAt for a fresh, live-generation handle at format time is the
		 * SAME RULE RoadSlot::HandleAt exists to enforce elsewhere in this codebase: a hand-built
		 * {index, generation} pair is a second place that can go stale or be built wrong (#79,
		 * #173, #214's own "hand-built handle" architecture check) - a bare index cannot be
		 * either, because it is never mistaken for a handle in the first place. Only meaningful
		 * when Subject == ELabelSubject::Segment; see SnapGuide::Describe.
		 */
		int32 SegmentIndex = INDEX_NONE;

		/** FGuideAnchor::AlignTo's index (GesturePoint) or Network.GetEntities()'s (Entity,
		 *  EntityEdge, EntityCorner). */
		int32 SubjectIndex = INDEX_NONE;

		/**
		 * "parallel to", "square to", "aligned with", "along" - always a string LITERAL passed
		 * down from the call site, never built at runtime, so a raw pointer costs nothing and
		 * outlives every candidate it is copied into. Only ELabelKind::Along reads it.
		 */
		const TCHAR* Verb = nullptr;

		/** The whole label when Kind == Literal - again always a literal, never heap text. */
		const TCHAR* Text = nullptr;
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

		/**
		 * "square to the frontage", "45 degrees". Shown beside the line.
		 *
		 * EMPTY UNTIL A CANDIDATE WINS - #183. `Propose` no longer fills this (see FGuideLabel):
		 * `FSnapGuideChain::Resolve` fills it, from `Label`, for the at most two survivors of
		 * `Arbitrate`, via `SnapGuide::Describe`. A caller that builds a candidate by hand rather
		 * than through a source - GuideArbiterTest, to test Arbitrate without Tool/ - may still
		 * set this directly; Arbitrate carries it unchanged either way, exactly as before.
		 */
		FString Description;

		/** The recipe for Description, cheap to fill on every candidate - see FGuideLabel. */
		FGuideLabel Label;

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
