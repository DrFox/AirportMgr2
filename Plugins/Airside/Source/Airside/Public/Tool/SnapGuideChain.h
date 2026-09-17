#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"

class URoadNetwork;

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
};

/**
 * Gathers every source's candidates and arbitrates between them - design sections 3 and 5.
 *
 * MODELLED ON FRoadSnapChain, deliberately, down to the move-only ownership: a source added
 * in stage 2 is a new link, not an edit to a widening conditional. It differs in the one way
 * that matters - see IGuideSource on why nothing claims.
 *
 * THE ORDER SOURCES ARE ADDED IN DOES NOT DECIDE TIES. SnapGuide::ESource does, inside the
 * arbiter. This chain's order is only the order they are asked, which is unobservable.
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
	 * Every source's candidates, arbitrated, with Previous carrying the flicker rule.
	 *
	 * Previous is the caller's business to store: FBuildSession holds it, because
	 * IBuildTool::BuildPreview and BuildReadout are both const and neither could.
	 */
	SnapGuide::FResult Resolve(const URoadNetwork& Network, const FGuideAnchor& Anchor,
		const FVector2D& Cursor, const SnapGuide::FResult& Previous,
		const SnapGuide::FTuning& Tuning = SnapGuide::FTuning()) const;

private:
	TArray<TUniquePtr<IGuideSource>> Sources;
};
