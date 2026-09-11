#pragma once

#include "CoreMinimal.h"

class URoadNetwork;

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
 * Service ANCHORS do not link here at all on a stand with a lane. FServiceLoopBuild has
 * already spurred each of them to the stand's service loop, because a straight spur from a
 * road on one side to a box on the other crosses 37 m of fuselage - and it is the LANE that
 * links to the road, entering wherever it comes nearest.
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
	 * 50 m: how far a SERVICE connection may reach, in any direction.
	 *
	 * SHORT ON PURPOSE, and the shortness is what keeps the rejected case rejected. A Code C
	 * stand is about 40 m deep and the gap from stand to service road is typically 10-30 m,
	 * so this reaches the road the player meant and cannot reach the far side of a terminal -
	 * which is the whole reason nearest-guideline was refused for aircraft.
	 *
	 * The DEFAULT only. The live figure is level-authored on
	 * ARoadNetworkActor::ServiceLinkRadius, because it is per-airport gameplay tuning rather
	 * than a content default - the same distinction FTrafficRules records.
	 */
	static constexpr double DefaultServiceLinkRadius = 5000.0;

	/**
	 * Lays every service lane, then casts or measures every unjoined lead-in. Returns how
	 * many joined.
	 *
	 * An anchor that already has an incident edge is skipped, so a hand-drawn connection
	 * wins over the automatic one rather than being doubled up by it - and so does an anchor
	 * FServiceLoopBuild has just spurred to its stand's lane.
	 */
	static int32 Build(URoadNetwork& Network, double MaxLeadIn = DefaultMaxLeadIn,
		double ServiceLinkRadius = DefaultServiceLinkRadius);
};
