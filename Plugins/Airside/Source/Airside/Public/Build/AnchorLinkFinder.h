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

	/**
	 * A vehicle anchor, a pose, or a stand's DECLARED ENTRY: nearest guideline of the link's
	 * class, any direction.
	 *
	 * THE THIRD KIND IS GONE, 2026-09-16, and its absence is the point. There was a Lane kind
	 * here - "nearest approach between the whole lane and a road" - because a ring DECLARED
	 * nothing, so the pass had to search the lane for a point to join at as well as searching
	 * the airport for something to join to. A stand now says where it may be entered
	 * (UEntityDefinition::ServiceLane's Entry waypoints, reported as nodes by
	 * FStandLaneBuild::FResult::Entries), so the FROM end is a NODE like any other and one
	 * question is left rather than two. FLaneLinkFinder went with it.
	 */
	Proximity,
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

	/** Sweep radius for this stand's painted line - see RadiusForCode in AnchorLink.cpp. */
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
	 * Which stand's lane this link LEAVES. Unset for every ordinary anchor or pose link.
	 *
	 * Read THREE times. FAnchorLink::Build reads it twice - once to defer an unresolved entry
	 * into StandsRefused, once to record a resolved one into StandsJoined - keyed on the owner
	 * rather than on a list of the lane's edges, because joining retires handles and the
	 * stand must still be identifiable by something else afterwards. FAnchorLink::Join reads
	 * it a third time, as the one thing that says "this link leaves a lane": a lane is joined
	 * ALONG itself, so the lead-in's control goes back along the entry's own straight instead
	 * of on the midpoint. Set IF AND ONLY IF Node is a declared entry.
	 *
	 * Failure is reported ONCE PER STAND. A stand declares four entries and a service road
	 * runs past one side of it; the entries that reach no road of their own are the NORMAL
	 * case, not a fault, and warning per entry would put three false lines in the log for
	 * every stand on the airport. What is worth a warning is a stand where NO entry joined
	 * anything, which is a stand no truck can ever be sent to.
	 */
	FEntityInstanceId LaneOwner;
};

/** What a finder found: which guideline (and where) a link should join. */
struct FLinkHit
{
	FGuidelineEdgeId Edge;
	double Param = 0.0;

	bool IsSet() const { return Edge.IsSet(); }
};

/**
 * One strategy for finding what a pending link should join.
 *
 * Replaces the branches FAnchorLink::Build used to hide inline (proximity point->polyline,
 * ray cast) - see FAnchorLink's own doc comment for why the rule splits on traversal class in
 * the first place. Each finder owns its own MEASUREMENT only; the shared eligibility test
 * (alive, derived, not a self-loop, neither end already anchored, allows the link's class)
 * lives once in AnchorLinkFinder.cpp rather than being copied into both.
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

/**
 * A vehicle anchor, a pose or a declared entry: the nearest guideline of the link's class,
 * in any direction.
 *
 * WHICH OF A STAND'S ENTRIES SHOULD HAVE A GIVEN ROAD is deliberately NOT asked here. A
 * finder measures ONE link against the graph and knows nothing of the link's siblings; the
 * choice between them - the nearer node of a rounded corner's pair, and the entry nearest
 * each point of road - is made once in FAnchorLink::Gather, which can see all of them.
 */
struct AIRSIDE_API FProximityLinkFinder final : public ILinkFinder
{
	virtual bool Find(const URoadNetwork& Network, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes, FLinkHit& OutHit) const override;
};

/** Picks the finder for Kind. One switch, not scattered through FAnchorLink::Build. */
AIRSIDE_API const ILinkFinder& LinkFinderFor(ELinkKind Kind);
