#pragma once

#include "CoreMinimal.h"
#include "Build/AnchorLinkFinder.h"

class URoadNetwork;
struct FAirframe;

/**
 * Joins entity anchors to the guideline graph by casting each one's lead-in.
 *
 * PlaceEntity creates a guideline node per anchor and stops there, so until something runs
 * this an anchor is an ISLAND: it resolves, it draws, and no route can reach it. That is
 * the state the graph shipped in.
 *
 * The rule is the real one an airport uses. A stand's lead-in line runs from the taxiway to
 * the nose-stop along the stand's own heading, so the anchor casts a ray along its world
 * heading, and the first guideline that ray meets is the one it joins - splitting that
 * guideline at the hit and linking the two. A "nearest guideline" rule was considered and
 * rejected: nearest is regularly the taxiway on the far side of the terminal, and the link
 * would run straight through the building with nothing to report it.
 *
 * Runs AFTER FRoadGuidelineBuilder::Build, and everything it creates is bDerived, so the
 * next rebuild sweeps it and casts again. Anchor NODES stay non-derived and survive, which
 * is what keeps a stand's handles stable across edits.
 *
 * THE RULE SPLITS ON TRAVERSAL CLASS, since 2026-09-07. The paragraph above is about an
 * AIRCRAFT lead-in, where the ray IS the painted line and "nearest" is regularly the taxiway
 * on the far side of the terminal. It never applied to a VEHICLE, which may genuinely arrive
 * from any side - and a Code C stand made that maximally wrong, because four of its five
 * service anchors cast along a row of stands rather than across it, so a road drawn the way
 * a player naturally draws one, ALONGSIDE the row, was parallel to every ray and served
 * none of them. A service link therefore joins the NEAREST guideline of its class in any
 * direction, within a much shorter reach - see DefaultServiceLinkRadius.
 *
 * Service ANCHORS do not link here at all on a stand with a lane. FStandLayoutBuild has already
 * laid the lane THROUGH each of them, because a straight connector from a road on one side to
 * a box on the other crosses 37 m of fuselage - and it is the LANE that links to the road, at
 * an entry the definition declares.
 *
 * A DECLARED ENTRY IS JUST A NODE, which is what makes that link an ordinary proximity one.
 * The entrance used to be DISCOVERED - the ring was searched for its nearest approach to a
 * road and cut wherever that fell - and the apparatus that took (four helpers, a per-side
 * sweep and three thresholds tuned against one another) is gone with the question. What
 * Gather still decides, because no finder can, is WHICH of a stand's entries gets a given
 * road: the nearer node of each rounded corner's pair, and then the entry nearest each point
 * of road. See Gather.
 *
 * Only DERIVED guidelines are split. A hand-drawn one is left alone: splitting it would
 * either discard the player's edit on the next rebuild, or - if the halves inherited its
 * non-derived flag - split again every rebuild and accumulate forever. Joining a hand-drawn
 * guideline is therefore done by hand, which is the same hand that drew it.
 */
struct AIRSIDE_API FAnchorLink
{
	/** 200 m. Beyond this an anchor is not "just off" a taxiway and stays unjoined. */
	static constexpr double DefaultMaxLeadIn = 20000.0;

	/**
	 * 65 m: how far a SERVICE connection may reach, in any direction.
	 *
	 * SHORT ON PURPOSE, and the shortness is what keeps the rejected case rejected. A Code C
	 * stand is about 40 m deep and the gap from stand to service road is typically 10-30 m,
	 * so this reaches the road the player meant and cannot reach the far side of a terminal -
	 * which is the whole reason nearest-guideline was refused for aircraft.
	 *
	 * IT WAS 50 m UNTIL 2026-09-16, AND WHAT MOVED IS THE END THE MEASUREMENT IS TAKEN FROM.
	 * This reach is measured from the node that links to the nearest point of the road, and
	 * that node used to be on a ring lying 3 m outboard of the wingtips - the line a player
	 * read as the edge of the stand. The lane runs INSIDE the wingtip now (see
	 * UEntityDefinition::ServiceLane for why), so the same road is further from the thing that
	 * reaches it by exactly the distance between the two lines:
	 *
	 *     port side       ring at y = -2090, lane run at y =  -600   ->  1490 uu
	 *     starboard side  ring at y = +2090, box row at y = +1100   ->   990 uu
	 *
	 * THE PORT SIDE BINDS, being the side that came furthest in, so 5000 + 1490 = 6490, taken
	 * as 6500. Sizing on the starboard 990 would shorten the reach on the port side, which is
	 * the side a service road is actually drawn past on this layout.
	 *
	 * SO THE PLAYER'S PROMISE IS UNCHANGED - 50 m from where the stand's edge is drawn - and
	 * that is the point of deriving the figure rather than picking a rounder one. MEASURED on
	 * the suite's own fixture (Airside.Build.StandLinkClearsTheTruckLock, the 5400 uu gap): the
	 * road at y = -6000 that FuelServiceTest, RoadAlongsideARowOfStands and
	 * StandFuelAnchorJoinsRoad all lay is joined at the entries at (1431, -600) and
	 * (-3236, -600), 5400 uu away - the PERPENDICULAR from the port run, not a diagonal, since
	 * an entry sits at each end of the run and a road that spans the stand has a foot square
	 * abeam each of them. A road that stops SHORT of an entry is nearest at its own end
	 * instead, so such a road is reached on the diagonal and needs more of this reach than its
	 * clearance alone suggests; nothing in the suite measures that case yet.
	 *
	 * The 200 m case Airside.Build.ServiceLinkJoinsFromAnyDirection refuses is three times this
	 * and still does.
	 *
	 * The DEFAULT only. The live figure is level-authored on
	 * ARoadNetworkActor::ServiceLinkRadius, because it is per-airport gameplay tuning rather
	 * than a content default - the same distinction FTrafficRules records. M_Starter authors
	 * no value for it, so the placed actor takes this one; a level that HAS authored one keeps
	 * what it authored, and its stands stop reaching their roads until it is raised by hand.
	 *
	 * NOT DERIVED IN CODE, because there is nothing here to derive it from: the two lines above
	 * are a property of a DEFINITION (which stand, sized for which aircraft) and this is one
	 * constant for every stand on every airport. The arithmetic and its inputs are therefore
	 * written out, and a second stand type whose lane sits further inboard than the port run
	 * has to be checked against this figure by hand.
	 */
	static constexpr double DefaultServiceLinkRadius = 6500.0;

	/**
	 * Within this of an endpoint, join the endpoint rather than splitting off a stub - and,
	 * inside a finder's search, within this of a hit is refused rather than reported, since
	 * a hit that close would only ever be welded back to nothing anyway. ONE constant for
	 * both: Gather/Join and the three ILinkFinder strategies used to each keep their own
	 * copy, which is exactly the kind of pair CLAUDE.md's "lists that must agree are one
	 * list" is about - a search tolerance and a weld tolerance that quietly drifted apart
	 * would report a hit its own join would then refuse to make.
	 */
	static constexpr double LeadInWeldTolerance = 10.0;

	/**
	 * Lays every service lane, then casts or measures every unjoined lead-in. Returns how
	 * many joined.
	 *
	 * An anchor that already has an incident edge is skipped, so a hand-drawn connection
	 * wins over the automatic one rather than being doubled up by it - and so does an anchor
	 * FStandLayoutBuild has just laid its stand's lane through.
	 *
	 * Three steps, in order, per Pending link: Gather collects every anchor, pose and declared
	 * entry awaiting a link before anything mutates; Resolve dispatches to the ILinkFinder for
	 * the link's Kind and reports the best candidate, if any; Join splits the candidate and
	 * lays the lead-in and its entry sweeps. Splitting these out is what makes each finder
	 * unit-testable in isolation - nothing below Resolve needs a mutated graph, a placed
	 * entity, or even a second guideline to react to.
	 *
	 * LargestServiceVehicle IS REQUIRED, not resolved in here - issue #190. Join asks
	 * UAirsideSettings::ResolveLargestServiceVehicle() TWICE PER LINK for the lock a lane
	 * connector must clear; the caller now resolves it once per rebuild and this Build hands
	 * the same answer to every Join it calls, which is also why this file no longer includes
	 * Content/AirsideSettings.h (Check-Architecture's Build->Content rule).
	 */
	static int32 Build(URoadNetwork& Network, const FAirframe& LargestServiceVehicle,
		double MaxLeadIn = DefaultMaxLeadIn, double ServiceLinkRadius = DefaultServiceLinkRadius);

	/**
	 * Every anchor, pose and declared lane entry that has nothing joined yet, plus every node
	 * a link must not itself target (anchor/pose nodes, and every node of every service lane
	 * - see AnchorNodes' use in Resolve for why the second half matters).
	 *
	 * Gathered up front, deliberately: joining one anchor adds and removes edges, and an
	 * iteration over the graph must not be holding pointers into it while that happens.
	 *
	 * AND THE CHOICE BETWEEN A STAND'S ENTRIES IS MADE HERE, on the graph as it stands BEFORE
	 * any of this pass's links - which is what keeps the answer independent of the order the
	 * entries happen to be visited in. Each link splits the road it joins, and an entry
	 * measured after that is measuring a different airport from the one measured before it.
	 */
	static void Gather(URoadNetwork& Network, double MaxLeadIn, double ServiceLinkRadius,
		TArray<FPendingLink>& OutPending, TSet<FGuidelineNodeId>& OutAnchorNodes);

	/** Strategy dispatch: LinkFinderFor(Link.Kind)'s best candidate, or an unset hit. */
	static FLinkHit Resolve(const URoadNetwork& Network, const FPendingLink& Link,
		const TSet<FGuidelineNodeId>& AnchorNodes);

	/**
	 * Splits Hit's guideline and lays the lead-in and its entry sweeps.
	 *
	 * Mutates Link: a link found by distance rather than by ray has no Dir until the join says
	 * which way the road lies, and a link leaving a stand's lane (FPendingLink::LaneOwner) has
	 * one only once the tangent run along that lane is known. Mutates AnchorNodes too - every
	 * node the join creates is added, because the lead and both sweeps just added would
	 * otherwise be a target for the NEXT link's own Resolve.
	 *
	 * Returns the LeadEnd node on success, an unset handle if Hit's guideline no longer
	 * resolves (already spent by an earlier link this same pass), or if any
	 * SplitGuidelineEdge call inside this Join fails - the hard join, or either half of the
	 * two-cut sweep. Both are the same data-race-only case: every id Join splits was resolved
	 * moments earlier by this same link's own Resolve.
	 */
	static FGuidelineNodeId Join(URoadNetwork& Network, FPendingLink& Link, const FLinkHit& Hit,
		TSet<FGuidelineNodeId>& AnchorNodes, const FAirframe& LargestServiceVehicle);
};
