#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/GuideArbiter.h"
#include "Tool/SnapGuideSettings.h"

class URoadNetwork;
struct FEntityInstance;

/**
 * A point worth lining up with, and what to call it in the label.
 *
 * ONE STRUCT PER THING: the position and its name are one fact, and two parallel arrays are
 * exactly how they come apart.
 */
struct FGuidePoint
{
	FVector2D At = FVector2D::ZeroVector;

	/** "corner 3". The source composes "0 degrees to corner 3" from it. */
	FString Name;

	/**
	 * Which COLUMN this point belongs to - ThisGesture for the gesture's own corners, Road for
	 * a live network node, Apron for an apron's corner.
	 *
	 * THE TOOL TAGS IT, for the same reason the tool supplies the point at all: only the tool
	 * knows where its own points came from, and a source that went looking would be a second
	 * opinion about the gesture. Without it the LevelWith row could not be gated by column -
	 * ONE flat array serves both the plot's pinned corners and the road's network nodes, and
	 * those are different columns of the grid.
	 *
	 * DEFAULTS TO ThisGesture because the gesture's own points are the case that needs no
	 * switch: EReference::ThisGesture is the one column with no button.
	 */
	SnapGuide::EReference Reference = SnapGuide::EReference::ThisGesture;
};

/**
 * What the point the player is moving REPRESENTS.
 *
 * A positional guide aligns LIKE WITH LIKE: centreline to centreline, boundary to boundary, and
 * a centreline against a boundary is displaced by the drag's half-width. A road's cursor is its
 * CENTRELINE; a plot's or an apron's is a corner of the shape itself, which is a BOUNDARY.
 * Lining a road's centre up with an apron's edge would put half its pavement over the apron.
 * See the 2026-09-20 guide-grid design section 6.
 *
 * AN ENUM, NOT A BOOL, per CLAUDE.md: the two cannot both be true, so the illegal state is not
 * representable - and a third kind is easy to imagine, a kerb line or a painted edge.
 */
enum class EDragPoint : uint8
{
	Centreline,
	Boundary
};

/**
 * What the tool is dragging, and what it is dragging it against.
 *
 * THE TOOL SUPPLIES IT, THE DRIVER RESOLVES FROM IT - see IBuildTool::DescribeGuideAnchor.
 * A source cannot ask "what is the player doing": only the tool knows which corner moves and
 * which edge it grew from, and a source that guessed would be a second opinion about the
 * gesture.
 */
struct FGuideAnchor
{
	/** The fixed point the moving point swings around. Every candidate is a line through it. */
	FVector2D Origin = FVector2D::ZeroVector;

	/**
	 * The direction the gesture is already extending - the frontage for a plot corner, the
	 * incoming segment for a road. ZERO when the tool has none, and the Extending source
	 * then proposes nothing rather than inventing an axis.
	 */
	FVector2D Reference = FVector2D::ZeroVector;

	/**
	 * The point Extending's dashed line is drawn TO: the far end of the reference edge, so
	 * the line SHOWS which edge is being squared to. Design section 6 - a ray into the
	 * distance says "45 degrees", and a line to the thing says what you are lining up with,
	 * which is the whole of what the request asked for.
	 */
	FVector2D ReferenceAt = FVector2D::ZeroVector;

	/**
	 * "the frontage". Composed by the source into "square to the frontage", so the same
	 * source extending a road in stage 5 does not have to read as a plot. The tool names its
	 * own reference because the tool is the only thing that knows what it is.
	 */
	FString ReferenceName;

	/**
	 * Points the moving point may line UP WITH - the gesture's own pinned corners.
	 *
	 * Supplied by the tool for the same reason Reference is: only the tool knows which of its
	 * points are still meaningful this frame, and a source that went looking would be reading
	 * stale corners (see FPlotPlaceTool::Quad on entries past PinnedCount being stale).
	 *
	 * Stage 2's Aligned source feeds network points into the SAME source without changing it.
	 */
	TArray<FGuidePoint> AlignTo;

	/**
	 * See EDragPoint. Centreline unless the tool says otherwise, because a road is the common
	 * case and a tool that forgot to answer should not silently change how it aligns.
	 */
	EDragPoint Point = EDragPoint::Centreline;

	/**
	 * This gesture has not started yet: Origin is to be filled with the CURSOR, by the driver.
	 *
	 * A FLAG RATHER THAN THE POINT ITSELF, because IBuildTool::DescribeGuideAnchor is not handed
	 * the cursor and deliberately is not - see that declaration on why it takes the target and
	 * never the half-built context. FBuildSession::MakeContext has the plane hit two lines above
	 * the call, so it is the one place that can answer; the tool says only that it wants it.
	 *
	 * ONLY THE POSITIONAL GUIDES SURVIVE IT, and that falls out rather than being enforced: with
	 * Origin ON the cursor, SnapGuide::Arbitrate can measure no direction from one to the other
	 * and every EFit::Angular candidate sits out of its own accord. So a free start offers
	 * Collinear, AngledFrom and MatchingGap - and LevelWith only where the tool ALSO names a
	 * Reference direction for those lines to run along, which is why FStandPlaceTool names its
	 * heading and FRunwayTool, having none, does not.
	 */
	bool bFreeStart = false;

	/**
	 * How far the drag's pavement reaches either side of its point, uu. Zero when the gesture
	 * has no width - a plot corner, a guideline - and zero is then a MEANING, not an omission.
	 *
	 * TWO FIELDS, NOT ONE: URoadProfile::GetHalfWidthLeft and GetHalfWidthRight are separate
	 * because a cross-section may be off-centre, so a flush-left candidate and a flush-right
	 * one are not a mirrored pair and must not be computed as one.
	 */
	double HalfWidthLeft = 0.0;
	double HalfWidthRight = 0.0;
};

/**
 * One link of the guide chain - design section 3.
 *
 * UNLIKE IRoadSnapRule, NOTHING CLAIMS. A snap rule answers "what did the cursor hit" and the
 * first hit ends the search; a guide source answers "what could this line up with", and the
 * whole point is that several answer at once and SnapGuide::Arbitrate chooses between them.
 * So this proposes into a shared array and never returns a verdict.
 */
struct AIRSIDE_API IGuideSource
{
	virtual ~IGuideSource() = default;

	/**
	 * Appends this source's candidates. NEVER clears Out - the chain owns that array.
	 *
	 * TAKES THE CURSOR AS WELL AS THE ANCHOR, since 2026-09-20, and the two are NOT
	 * interchangeable: they are the fixed end of the gesture and the moving end. A source picks
	 * whichever its reach should be measured from, and the choice is per source, not per chain.
	 *
	 * ORIGIN for the sources that answer "WHICH WAY from here" - Extending and Parallel. Their
	 * reference must not change halfway through a drag, which is what FParallelGuideSource's own
	 * comment records: "a search keyed to the cursor would hand the guide to a different road
	 * halfway through the drag".
	 *
	 * CURSOR for the sources that answer "WHERE did the far end land" - Collinear, MatchingGap,
	 * AngledFrom and the apron family. Reported from PIE with a diagram: a long road starts far
	 * from the pair it is being matched against, so an origin-keyed search found nothing and the
	 * matching-gap guide never appeared. The player was aiming with the cursor all along, and
	 * this signature is what lets a source see it. See
	 * Airside.Tool.OffsetGuideReachesWhatTheCursorIsNear.
	 *
	 * TAKES TUNING TOO, since #192 - every network source used to read a fresh, default
	 * `SnapGuide::FTuning()` for its own reach test rather than the Tuning FSnapGuideChain::
	 * Resolve was handed, so a caller that tightened SearchRadiusUu changed nothing a source
	 * actually measured against.
	 *
	 * DEFAULTED, not required at every call site: the tests that ask one source directly for
	 * its raw candidates (ProposedBy and its callers) are exercising that source in isolation
	 * and have no chain-level Tuning to hand it, so the default keeps them reading exactly as
	 * they did. FSnapGuideChain::ProposeAll passes its own, threaded from Resolve, explicitly.
	 */
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const = 0;

	/**
	 * Which ERelation this link proposes. The toggle asks, and the chain skips it when off.
	 *
	 * PURE VIRTUAL rather than a field, so a source cannot be written without answering it.
	 *
	 * A RELATION, NOT A CELL - the assumption the old comment here invited a reader to revisit,
	 * revisited on 2026-09-20. Several sources share one relation: Parallel is proposed by the
	 * road, runway, stand and world sources alike. And one source may span several REFERENCES -
	 * the segment walkers tag Road or Runway per segment - which is why the reference is tagged
	 * per candidate rather than declared here. The chain skips a source whose relation is off;
	 * a candidate whose reference is off is dropped as it is gathered.
	 */
	virtual SnapGuide::ERelation Relation() const = 0;
};

/**
 * Source 1: what the tool is already extending, and its perpendicular.
 *
 * NEEDS NO NETWORK - the tool supplied the reference. It still takes one, like every source,
 * because the chain calls them through one interface and a signature that varied per source
 * would put the branch in the chain instead.
 */
struct AIRSIDE_API FExtendingGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Extending; }
};

/**
 * Source 6: the four world axes - north-south, northeast-southwest, east-west and
 * northwest-southeast.
 *
 * FOUR, NOT EIGHT. 180 degrees away is the same LINE and SnapGuide::Arbitrate measures the
 * acute angle, so eight would put two identical candidates into every tie the source-order
 * rule then has to break for no reason. Naming each by BOTH its ends says so on screen.
 *
 * COMPASS, NOT A MATHS ANGLE, and it always was - RunwayDesignator declares north to be +X, so
 * the bearings these are built from are the same numbers a runway is named after. A guide on
 * the northeast-southwest axis is parallel to runway 05/23. See FWorldAxis in the .cpp for why
 * the labels stopped being those numbers.
 */
struct AIRSIDE_API FWorldGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Parallel; }
};

/**
 * Source 2: lines THROUGH each point the tool named, along the reference and its perpendicular.
 *
 * THE FIRST SOURCE WHOSE LINES DO NOT PASS THROUGH THE DRAG'S ORIGIN, which is why
 * FCandidate carries Through at all. Its candidates are EFit::Perpendicular: "level with
 * corner 3" is about where the cursor ended up, not about which way it set off, and an angle
 * measured from an origin that is nowhere on the line would answer a different question.
 *
 * It needs no network: the tool supplies the points, as it supplies the reference.
 */
struct AIRSIDE_API FPointAlignGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::LevelWith; }
};

/**
 * Source 5: the nearest road's direction, and its perpendicular.
 *
 * THE NEAREST ONE ONLY. Every road proposing would put the whole field in the race, and the
 * winner would be decided by a road the player cannot see - see FTuning::SearchRadiusUu.
 *
 * Angular, through the drag's own origin: this answers "which way from here", the same
 * question Extending answers, and it loses to Extending on a tie because the edge you are
 * extending is what you are thinking about.
 */
struct AIRSIDE_API FParallelGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Parallel; }
};

/**
 * Which end of the gesture a segment source's reach test is measured from - see
 * IGuideSource::Propose on Origin vs Cursor. FILE SCOPE, not nested in FSegmentGuideSource
 * below: FParallelGuideSource is not one of that base's children (see its own comment) but
 * still shares WalkGuideSegments in the .cpp, so both need to name the same policy.
 */
enum class EGuideReachFrom : uint8
{
	/** Unbounded - the Runway column's own sources; see FRunwayGuideSource. */
	None,
	Origin,
	Cursor
};

/**
 * Shared machinery for the five sources that walk URoadNetwork::GetSegments() once and emit
 * PER MATCHING SEGMENT - Collinear, RunwayLine, AngledRoad, AngledRunway and Runway (#192).
 * Each used to carry its own copy of the same prologue - SegmentIdAt, GuideRoadColumn-or-
 * IsRunwaySegment, GuideSegmentEnds (now URoadNetwork::SegmentEnds), the Span.IsNearlyZero
 * guard, and an optional reach test - eight walks in this file differing only in TWO
 * policies (which column, how far the reach extends, if at all) and WHAT each did with a
 * segment that passed.
 *
 * FParallelGuideSource IS NOT ONE OF THE FIVE: it reduces the same walk to a single NEAREST
 * match before emitting once, a different shape from "emit per segment" that would make
 * EmitForSegment lie about when it runs. It shares the walk itself - see WalkGuideSegments in
 * the .cpp - without deriving from this base.
 *
 * TWO POLICY KNOBS, set once by each source's constructor rather than copied into a loop:
 * bRunwayColumn picks GuideRoadColumn's Road columns (bounded by Tuning.SearchRadiusUu) or the
 * Runway column (unbounded - see FRunwayGuideSource on why); ReachFrom says which end of the
 * gesture an optional reach test is measured from, or that there is none.
 */
struct AIRSIDE_API FSegmentGuideSource : public IGuideSource
{
protected:
	FSegmentGuideSource(SnapGuide::ERelation InRelation, bool bInRunwayColumn, EGuideReachFrom InReachFrom)
		: SourceRelation(InRelation), bRunwayColumn(bInRunwayColumn), ReachFromPoint(InReachFrom)
	{
	}

	/**
	 * Called once per segment that cleared the column and reach gates, in GetSegments() order.
	 * Column is this segment's classification - the Taxiway/ServiceRoad GuideRoadColumn found,
	 * or Runway when the source was built with bRunwayColumn - so an override never asks again.
	 */
	virtual void EmitForSegment(FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
		SnapGuide::EReference Column, const FGuideAnchor& Anchor, const FVector2D& Cursor,
		TArray<SnapGuide::FCandidate>& Out) const = 0;

public:
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override final;

	virtual SnapGuide::ERelation Relation() const override final { return SourceRelation; }

private:
	SnapGuide::ERelation SourceRelation;
	bool bRunwayColumn;
	EGuideReachFrom ReachFromPoint;
};

/**
 * Source 4: the line an existing segment already lies on.
 *
 * PERPENDICULAR, not angular, and that is the whole difference from Parallel above. Parallel
 * says "point the same way as that taxiway"; this says "you are ON the line that taxiway lies
 * along", which is a statement about where the cursor ENDED UP. Measuring it as an angle from
 * an origin that is nowhere on the line would answer a different question - the same
 * reasoning that gave PointAlign its fit kind.
 *
 * ONE CANDIDATE PER SEGMENT IN REACH, not just the nearest: a cursor can be on the extension
 * of one segment while standing beside another, and that is exactly the case worth telling
 * the player about.
 */
struct AIRSIDE_API FCollinearGuideSource final : public FSegmentGuideSource
{
	FCollinearGuideSource()
		: FSegmentGuideSource(SnapGuide::ERelation::Collinear, /*bRunwayColumn=*/false, EGuideReachFrom::Cursor)
	{
	}

protected:
	virtual void EmitForSegment(FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
		SnapGuide::EReference Column, const FGuideAnchor& Anchor, const FVector2D& Cursor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * Source 6: every runway's heading, and its perpendicular.
 *
 * DELIBERATELY UNBOUNDED by SearchRadiusUu, unlike every other network source. An airport
 * squares to its runways from anywhere on it - that is what makes a field read as one place
 * rather than as a pile of unrelated pavement - and there are at most a handful of runways to
 * walk. Design section 3 says "every runway's heading" and means it.
 *
 * Below the local sources and above the world axes, because an airport squares to its runways
 * but not in preference to the taxiway the player is actually working on.
 */
struct AIRSIDE_API FRunwayGuideSource final : public FSegmentGuideSource
{
	FRunwayGuideSource()
		: FSegmentGuideSource(SnapGuide::ERelation::Parallel, /*bRunwayColumn=*/true, EGuideReachFrom::None)
	{
	}

protected:
	virtual void EmitForSegment(FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
		SnapGuide::EReference Column, const FGuideAnchor& Anchor, const FVector2D& Cursor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * The line a runway lies on, extended - its Collinear half.
 *
 * A SECOND SOURCE RATHER THAN A THIRD CANDIDATE ON FRunwayGuideSource, and the reason is the
 * gate: FSnapGuideChain::Resolve skips a source by its declared Relation() BEFORE it walks
 * anything, so a source proposing two relations would have both silenced by whichever one it
 * happened to declare. Switching the Parallel row off would have taken this line with it.
 * One relation per source is what makes that skip safe.
 *
 * WHY IT IS NOT IN FCollinearGuideSource: that source is bounded by SearchRadiusUu and this
 * must not be. A runway's extended centreline is the approach path - it is meaningful from
 * anywhere on the field, which is the same argument FRunwayGuideSource makes for its heading.
 * Before 2026-09-20 Collinear DID offer it, by accident, because it walked every segment and
 * a runway is just a segment; the line therefore existed but answered to the wrong toggle.
 */
struct AIRSIDE_API FRunwayLineGuideSource final : public FSegmentGuideSource
{
	FRunwayLineGuideSource()
		: FSegmentGuideSource(SnapGuide::ERelation::Collinear, /*bRunwayColumn=*/true, EGuideReachFrom::None)
	{
	}

protected:
	virtual void EmitForSegment(FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
		SnapGuide::EReference Column, const FGuideAnchor& Anchor, const FVector2D& Cursor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * A line out of a ROAD's end, at 45, 90 or 135 degrees to that road.
 *
 * THE SKETCHED REQUEST, 2026-09-20 (samples/suggestion.png): "45 degrees to other road", drawn
 * from the drag to the far road's near END. Nothing offered it - FParallelGuideSource squares
 * to a road through the DRAG'S origin, never through the road's own end, and FCollinearGuideSource
 * offers only the 0 degree member.
 *
 * BOTH ENDS OF EVERY SEGMENT IN REACH. A spoke off one end and a spoke off the other are
 * parallel lines a segment-length apart, so proposing one end would be picking for the player.
 * At a junction the shared node throws a spoke per incident segment, which is right: each is
 * "45 degrees to THAT road".
 *
 * RUNWAYS ARE NOT WALKED HERE - FAngledRunwayGuideSource owns them, unbounded, exactly as the
 * partition has FParallelGuideSource leave them to FRunwayGuideSource.
 */
struct AIRSIDE_API FAngledRoadGuideSource final : public FSegmentGuideSource
{
	FAngledRoadGuideSource()
		: FSegmentGuideSource(SnapGuide::ERelation::AngledFrom, /*bRunwayColumn=*/false, EGuideReachFrom::Cursor)
	{
	}

protected:
	virtual void EmitForSegment(FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
		SnapGuide::EReference Column, const FGuideAnchor& Anchor, const FVector2D& Cursor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * The same spokes off a RUNWAY's threshold, and unbounded like the rest of the Runway column.
 *
 * A TAXIWAY LEAVING A THRESHOLD AT 45 DEGREES is the case that earns it - a rapid-exit is
 * exactly this line, and it is laid from anywhere on the field. See FRunwayLineGuideSource for
 * why a second source rather than a filter inside the road one: the chain skips a source by its
 * declared Relation(), so the reach policy is the only thing that can vary per source.
 */
struct AIRSIDE_API FAngledRunwayGuideSource final : public FSegmentGuideSource
{
	FAngledRunwayGuideSource()
		: FSegmentGuideSource(SnapGuide::ERelation::AngledFrom, /*bRunwayColumn=*/true, EGuideReachFrom::None)
	{
	}

protected:
	virtual void EmitForSegment(FRoadSegmentId Id, const FVector2D& A, const FVector2D& B,
		SnapGuide::EReference Column, const FGuideAnchor& Anchor, const FVector2D& Cursor,
		TArray<SnapGuide::FCandidate>& Out) const override;
};

/**
 * An apron's EDGES, as a direction to point along and its perpendicular.
 *
 * AN APRON IS A BOUNDARY, NOT A CENTRELINE, and that is what separates this whole family from
 * the road sources. A road is a line with pavement either side; an apron edge IS the pavement's
 * limit. FApronLineGuideSource is where that difference bites - see the displacement there.
 *
 * FOUR SOURCES, ONE PER RELATION - Parallel here, Collinear, AngledFrom and LevelWith below.
 * FSnapGuideChain::Resolve skips a source by its declared Relation() BEFORE it walks anything,
 * so one source proposing four relations would have all four silenced by whichever it happened
 * to declare. That is not hypothetical: it is what FRunwayGuideSource did on 2026-09-20 until
 * FRunwayLineGuideSource split off it.
 *
 * NO NAME OF ITS OWN. FApronSurface carries a material slot and nothing a player would read, so
 * the label is "the apron edge" and the dashed line says WHICH - exactly as FRoadDrawTool labels
 * an unnamed node "that node" and lets the drawn line carry the rest.
 */
struct AIRSIDE_API FApronGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Parallel; }
};

/**
 * The line an apron edge lies on, extended - and the one source that displaces by the drag's
 * half-width.
 *
 * FLUSH, NOT CENTRED. Lining a road's centreline up with an apron's edge would put half the
 * pavement over the apron; the player means the road's EDGE to sit on it. See EDragPoint and the
 * 2026-09-20 design section 6 - this is the only cell where a centreline meets an extended
 * boundary, so it is the only place the rule applies.
 */
struct AIRSIDE_API FApronLineGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Collinear; }
};

/** Spokes at 45, 90 and 135 degrees out of an apron's corners - see FAngledRoadGuideSource. */
struct AIRSIDE_API FApronAngledGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::AngledFrom; }
};

/**
 * An apron's corners, as points to be level with.
 *
 * NOT DISPLACED, unlike FApronLineGuideSource. A corner is a POINT, not an extended edge - there
 * is nothing for a road's flank to run flush along, so centre-to-corner is what "level with" can
 * mean here.
 */
struct AIRSIDE_API FApronCornerGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::LevelWith; }
};

/**
 * Source 8: the gap a neighbouring parallel road already keeps.
 *
 * PERPENDICULAR, NOT A NEW "DISTANCE FAMILY". Design §2 asked for direction and distance to be
 * separate lists with separate arbitration, because "a rule that picked one winner across both
 * would have 'parallel to that taxiway' losing to '30 m from the last one'". EFit already does
 * exactly that: one winner per kind, so an angular guide and this one both hold and never
 * compete. And §4's "how far along the perpendicular" IS a line parallel to the reference at
 * that offset - which is what a Perpendicular candidate already means.
 *
 * WHAT WOULD still need the second family, and is not this: a LENGTH guide - "make this segment
 * the same 40 m as the last one" - which constrains distance ALONG the drag and is a point on a
 * ray, not a line. §3 lists no such source.
 */
struct AIRSIDE_API FOffsetGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::MatchingGap; }
};

/** What to call a placed entity where the player reads it. Falls back to the asset name. */
namespace EntityNaming
{
	// DECLARED AT FILE SCOPE, not as `const struct FEntityInstance&` in the signature below:
	// an elaborated type specifier inside a namespace declares a NEW type in THAT namespace,
	// so the parameter became EntityNaming::FEntityInstance and nothing could be passed to it.
	AIRSIDE_API FString Describe(const FEntityInstance& Entity);
}

/**
 * Source 3: a placed entity's pose direction, and its perpendicular.
 *
 * Bounded by SearchRadiusUu like the other local sources. Angular, through the drag's own
 * origin: "point the way that stand points" is a direction, not a line the cursor is on.
 *
 * THE WEAKEST OF THE FOUR NETWORK SOURCES, and worth saying why it is still here: a stand's
 * pose is usually square to the taxiway it serves, so Parallel already offers the same
 * direction most of the time. It earns its place on the apron, where a row of stands sets the
 * local grain and the nearest road is a long way off.
 */
struct AIRSIDE_API FAlignedGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Parallel; }
};

/**
 * Gathers every source's candidates and arbitrates between them - design sections 3 and 5.
 *
 * MODELLED ON FRoadSnapChain, deliberately, down to the move-only ownership: a source added
 * in stage 2 is a new link, not an edit to a widening conditional. It differs in the one way
 * that matters - see IGuideSource on why nothing claims.
 *
 * THE ORDER SOURCES ARE ADDED IN DOES NOT DECIDE TIES. The (ERelation, EReference) pair does,
 * inside the arbiter. This chain's order is only the order they are asked, which is
 * unobservable.
 */
class AIRSIDE_API FSnapGuideChain
{
public:
	/** Extending, PointAlign, then World, in design section 3's order. */
	FSnapGuideChain();

	// Move-only for the same reason FRoadSnapChain is: the chain OWNS its sources through
	// TUniquePtr, so there is no copy to make, and saying so beats being told by the compiler.
	FSnapGuideChain(const FSnapGuideChain&) = delete;
	FSnapGuideChain& operator=(const FSnapGuideChain&) = delete;
	FSnapGuideChain(FSnapGuideChain&&) = default;
	FSnapGuideChain& operator=(FSnapGuideChain&&) = default;

	void AddSource(TUniquePtr<IGuideSource> Source);

	int32 NumSources() const { return Sources.Num(); }

	/**
	 * Every enabled source's candidates, gathered and gated but NOT arbitrated.
	 *
	 * FOR THE GRID TEST, and said plainly rather than hidden behind a friend declaration:
	 * Resolve returns at most two winners, so a source proposing into a hole would be invisible
	 * the moment it lost its race - which is exactly the shape of the 2026-09-20 report.
	 * Production callers want Resolve.
	 */
	void ProposeAll(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, const FSnapGuideSettings& Enabled,
		TArray<SnapGuide::FCandidate>& Out,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const;

	/**
	 * Every source's candidates, arbitrated, with Previous carrying the flicker rule.
	 *
	 * Previous is the caller's business to store: FBuildSession holds it, because
	 * IBuildTool::BuildPreview and BuildReadout are both const and neither could.
	 */
	SnapGuide::FResult Resolve(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, const SnapGuide::FResult& Previous,
		const FSnapGuideSettings& Enabled = FSnapGuideSettings(),
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const;

private:
	TArray<TUniquePtr<IGuideSource>> Sources;
};
