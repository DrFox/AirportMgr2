#pragma once

#include "CoreMinimal.h"
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

	/** Appends this source's candidates. NEVER clears Out - the chain owns that array. */
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const = 0;

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
		TArray<SnapGuide::FCandidate>& Out) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Extending; }
};

/**
 * Source 6: 0, 45, 90 and 135 degrees.
 *
 * FOUR, NOT EIGHT. 180 degrees away is the same LINE and SnapGuide::Arbitrate measures the
 * acute angle, so eight would put two identical candidates into every tie the source-order
 * rule then has to break for no reason.
 */
struct AIRSIDE_API FWorldGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;

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
		TArray<SnapGuide::FCandidate>& Out) const override;

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
		TArray<SnapGuide::FCandidate>& Out) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Parallel; }
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
struct AIRSIDE_API FCollinearGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Collinear; }
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
struct AIRSIDE_API FRunwayGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Parallel; }
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
struct AIRSIDE_API FRunwayLineGuideSource final : public IGuideSource
{
	virtual void Propose(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		TArray<SnapGuide::FCandidate>& Out) const override;

	virtual SnapGuide::ERelation Relation() const override { return SnapGuide::ERelation::Collinear; }
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
		TArray<SnapGuide::FCandidate>& Out) const override;

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
		TArray<SnapGuide::FCandidate>& Out) const override;

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
		const FSnapGuideSettings& Enabled, TArray<SnapGuide::FCandidate>& Out) const;

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
