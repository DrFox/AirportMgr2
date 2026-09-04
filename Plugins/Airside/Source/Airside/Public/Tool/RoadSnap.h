#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/** What a cursor position resolved to. */
enum class ERoadSnapKind : uint8
{
	/** Nothing was near enough; the raw cursor position stands. */
	Free,

	/** An existing node. Clicking reuses it, which is how a junction is closed. */
	Node,

	/** A point along an existing segment. Clicking splits it and creates a node there. */
	Segment,
};

/**
 * Where a click would actually land, and what it would mean.
 *
 * Position is always filled in, whatever the kind, so a caller that only wants a
 * position never has to branch. For a Node snap it is the node's stored position
 * copied verbatim - not the cursor, and not a recomputed value - because reusing a
 * node has to mean the click lands on exactly the coordinates the graph already holds.
 */
struct FRoadSnapResult
{
	ERoadSnapKind Kind = ERoadSnapKind::Free;

	FVector2D Position = FVector2D::ZeroVector;

	/** Set only when Kind is Node. */
	FRoadNodeId Node;

	/** Set only when Kind is Segment. */
	FRoadSegmentId Segment;

	/** Parameter along the segment's A->B chord, in (0,1). Only when Kind is Segment. */
	double SegmentT = 0.0;
};

/** Tuning shared by every rule, passed per call so it can be edited live. */
struct FRoadSnapSettings
{
	/** How close, in uu, the cursor must be to a node to reuse it. */
	double NodeRadius = 150.0;

	/** How close, in uu, the cursor must be to a segment to split it. */
	double SegmentRadius = 150.0;

	bool bSnapToSegments = true;

	/**
	 * Nearest a split may happen to either endpoint of the segment being split, in uu.
	 *
	 * A FLOOR, not the whole rule. The real exclusion is the endpoint's junction reach,
	 * which is far larger than this on any road of realistic width - a split 200 uu from
	 * a node whose junction reaches 550 uu drops a new node inside existing pavement. This
	 * still stands as the answer for a node that has no junction to reach anywhere.
	 */
	double MinSplitFromEndpoint = 50.0;

	/**
	 * Multiplier on a node's junction reach when deciding how far out it claims the cursor.
	 *
	 * The effective node snap radius is max(NodeRadius, reach * this). NodeRadius alone is
	 * a fixed number - 150 uu by default - while a junction's pavement extends by
	 * HalfWidth + |R / tan(Theta/2)|, which is several times that on any real road. Every
	 * click in the gap between the two used to make a SECOND node inside the first's
	 * junction: two overlapping junction polygons at the same Z, which is z-fighting and
	 * not a solvable surface.
	 *
	 * Snapping rather than refusing is deliberate. It makes the overlap unrepresentable
	 * instead of forbidden, the same move the mesh builder's weld map makes, and it leaves
	 * no dead band where clicking does nothing.
	 *
	 * Zero restores the old fixed-radius behaviour, for a test that needs it.
	 */
	double JunctionSnapFactor = 1.0;
};

/**
 * One link of the snap chain - design spec section 7.4.
 *
 * Resolve returns true to claim the cursor and stop the chain, false to pass. A rule
 * must not write to Out unless it claims.
 */
struct AIRSIDE_API IRoadSnapRule
{
	virtual ~IRoadSnapRule() = default;

	virtual bool Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
		const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const = 0;
};

/** Priority 1: an existing node within NodeRadius. The nearest one wins. */
struct AIRSIDE_API FRoadNodeSnapRule final : public IRoadSnapRule
{
	virtual bool Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
		const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const override;
};

/**
 * Priority 2: an existing segment within SegmentRadius, split at the closest point.
 *
 * Stands down when the closest point on the segment is one of its endpoints, or lies
 * within MinSplitFromEndpoint of one. That neighbourhood belongs to the node rule,
 * which has already had its turn - so declining here means "no snap", not "a node
 * snap", and the cursor falls through to Free exactly as it should.
 */
struct AIRSIDE_API FRoadSegmentSnapRule final : public IRoadSnapRule
{
	virtual bool Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
		const FRoadSnapSettings& Settings, FRoadSnapResult& Out) const override;
};

/**
 * Chain of Responsibility over the snap rules. First hit wins, in priority order.
 *
 * The point of the pattern here is that spec section 7.4's remaining rules - collinear
 * extension, angle snap, grid snap, and later "snap to runway centreline" - are new
 * links appended in priority order, not edits to a widening conditional.
 *
 * Free is NOT a link, though the spec's table lists it as priority 6. A terminal rule
 * that cannot fail is indistinguishable from no rule at all until someone builds a chain
 * without it, at which point Resolve has no answer to give. Making it the chain's
 * guaranteed fallback means a chain can be assembled wrongly and still return something
 * usable.
 *
 * Rules are searched linearly against every live node and segment. No spatial hash: the
 * graph is hand-authored at tens of nodes, and the hash is a swap behind this same
 * interface if that ever stops being true.
 */
class AIRSIDE_API FRoadSnapChain
{
public:
	/** Installs the node rule then the segment rule, in that priority order. */
	FRoadSnapChain();

	// Move-only: the chain OWNS its rules through TUniquePtr, so there is no copy to
	// make. Spelled out rather than left implicit because the implicit copy assignment
	// is still declared, and instantiating it is a compile error rather than a quiet
	// shallow copy - better to say what the type is than to be told by the compiler.
	FRoadSnapChain(const FRoadSnapChain&) = delete;
	FRoadSnapChain& operator=(const FRoadSnapChain&) = delete;
	FRoadSnapChain(FRoadSnapChain&&) = default;
	FRoadSnapChain& operator=(FRoadSnapChain&&) = default;

	/** Appends a rule at the LOWEST priority so far. Order of calls is the chain's order. */
	void AddRule(TUniquePtr<IRoadSnapRule> Rule);

	/** First rule to claim the cursor wins; Free when none does. */
	FRoadSnapResult Resolve(const URoadNetwork& Network, const FVector2D& Cursor,
		const FRoadSnapSettings& Settings) const;

	int32 NumRules() const { return Rules.Num(); }

private:
	TArray<TUniquePtr<IRoadSnapRule>> Rules;
};
