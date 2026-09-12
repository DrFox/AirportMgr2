#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"

class URoadNetwork;

/**
 * WHICH strategy resolves a pending link, replacing "which optional fields are set" as the
 * tag - the bool-soup CLAUDE.md's "a phase is an enum" forbids, and what FPendingLink used
 * to do: Lane.Num() > 0 meant one thing, Class == Aircraft (with Lane empty) meant another,
 * and everything else fell through to a third. Set explicitly wherever a link is gathered.
 */
enum class ELinkKind : uint8
{
	/** Aircraft: casts a ray along Dir; the first guideline it strictly meets wins. */
	Ray,

	/** A vehicle anchor or pose: nearest guideline of the link's class, any direction. */
	Proximity,

	/** A stand's service lane side: nearest approach between the lane and a road. */
	Lane,
};

/** One anchor waiting to be joined, gathered before the graph is mutated. */
struct FPendingLink
{
	ELinkKind Kind = ELinkKind::Proximity;

	FGuidelineNodeId Node;
	FVector2D At = FVector2D::ZeroVector;
	FVector2D Dir = FVector2D(1.0, 0.0);
	ETraversalClass Class = ETraversalClass::GroundVehicle;
	double MaxWingspan = 0.0;

	/** Sweep radius for this stand's painted line - see FAnchorLink's RadiusForCode. */
	double Radius = 2500.0;

	/**
	 * How far this link may reach. Always set explicitly by FAnchorLink::Gather - the
	 * aircraft cap or the service radius, decided once where the link is built rather than
	 * at the comparison - the two differ by a factor of four and the reasons are on
	 * FAnchorLink::DefaultMaxLeadIn and ::DefaultServiceLinkRadius. Zero here is not a usable
	 * default, deliberately: a link nobody sized should reach nothing rather than guess.
	 */
	double Reach = 0.0;

	/**
	 * Set when this link is a stand's whole service LANE rather than one node: the lane's
	 * edges, to measure from.
	 *
	 * A lane has no single point to link from - the road may come nearest anywhere along
	 * any side - so Node and At are filled in only once the search has said where, by
	 * splitting the lane there. Empty for every ordinary anchor or pose link.
	 *
	 * ONE SIDE, since a lane gets a link per side. The array remains an array because the
	 * splitting below is written against it and a side that has already been split by an
	 * anchor spur IS several edges.
	 */
	TArray<FGuidelineEdgeId> Lane;

	/**
	 * Which stand's ring this side belongs to. Unset for every ordinary anchor link.
	 *
	 * Read TWICE, and both readings need the owner rather than a list of the ring's edges:
	 * ConnectorCrossesLane re-finds the ring in the live graph because a link splits the
	 * side it joins and retires its handle, and the report below is per ring.
	 *
	 * Failure is reported ONCE PER RING. A ring has four sides and a service road
	 * along one of them; the other three failing to reach a road of their own is the
	 * NORMAL case, not a fault, and warning per side would put three false lines in the
	 * log for every stand on the airport. What is worth a warning is a ring where no side
	 * joined anything - which is the same condition the one-link-per-stand rule reported,
	 * and the reason the counters below are still per-ring for lanes.
	 */
	FEntityInstanceId LaneOwner;
};

/** What a finder found: which guideline (and where) a link should join. */
struct FLinkHit
{
	FGuidelineEdgeId Edge;
	double Param = 0.0;

	/** Lane kind only: which of the lane's own sides came nearest, and where on it. */
	FGuidelineEdgeId LaneEdge;
	double LaneParam = 0.0;

	bool IsSet() const { return Edge.IsSet(); }
};

/**
 * One strategy for finding what a pending link should join.
 *
 * Replaces the three branches FAnchorLink::Build used to hide inline (lane->road nearest,
 * proximity point->polyline, ray cast) - see FAnchorLink's own doc comment for why the rule
 * splits on traversal class and lane-ness in the first place. Each finder owns its own
 * MEASUREMENT only; the shared eligibility test (alive, derived, not a self-loop, neither
 * end already anchored, allows the link's class) lives once in AnchorLinkFinder.cpp rather
 * than being copied into all three.
 *
 * Read-only by design: a finder only searches, it never mutates the graph. FAnchorLink::Join
 * is the only thing that splits an edge, so a failed search can never leave a half-applied
 * change behind.
 */
struct AIRSIDE_API ILinkFinder
{
	virtual ~ILinkFinder() = default;

	/**
	 * Best candidate for Link among Network's live, derived guidelines, excluding anything
	 * incident to AnchorNodes (every lead-in already has an anchor at one end, so excluding
	 * those nodes excludes every existing lead-in with no separate mark on the edge needed)
	 * and anything closer than FAnchorLink::LeadInWeldTolerance (which would report a hit
	 * this link is already sitting on) or further than Link.Reach.
	 *
	 * False, with OutHit left untouched, when nothing qualifies.
	 */
	virtual bool Find(const URoadNetwork& Network, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const = 0;
};

/**
 * Aircraft: the first guideline the lead-in's ray meets, strictly ahead of the anchor.
 *
 * "Nearest" is deliberately not this rule - see FAnchorLink's doc comment on why a nearest
 * guideline is regularly the taxiway on the far side of the terminal.
 */
struct AIRSIDE_API FRayLinkFinder final : public ILinkFinder
{
	virtual bool Find(const URoadNetwork& Network, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const override;
};

/** A vehicle anchor or pose: the nearest guideline of the link's class, in any direction. */
struct AIRSIDE_API FProximityLinkFinder final : public ILinkFinder
{
	virtual bool Find(const URoadNetwork& Network, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const override;
};

/**
 * A stand's service lane side: nearest approach between the lane and a road, tried both
 * directions (see GuidelineGeom::NearestBetweenPolylines) so a road drawn PARALLEL to the
 * lane is measured side to side rather than corner to corner.
 */
struct AIRSIDE_API FLaneLinkFinder final : public ILinkFinder
{
	virtual bool Find(const URoadNetwork& Network, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const override;
};

/** Picks the finder for Kind. One switch, not scattered through FAnchorLink::Build. */
AIRSIDE_API const ILinkFinder& LinkFinderFor(ELinkKind Kind);
