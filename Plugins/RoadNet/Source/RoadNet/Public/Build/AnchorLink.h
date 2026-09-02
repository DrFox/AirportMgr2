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
 * Only DERIVED guidelines are split. A hand-drawn one is left alone: splitting it would
 * either discard the player's edit on the next rebuild, or - if the halves inherited its
 * non-derived flag - split again every rebuild and accumulate forever. Joining a hand-drawn
 * guideline is therefore done by hand, which is the same hand that drew it.
 */
struct ROADNET_API FAnchorLink
{
	/** 200 m. Beyond this an anchor is not "just off" a taxiway and stays unjoined. */
	static constexpr double DefaultMaxLeadIn = 20000.0;

	/**
	 * Casts every unjoined anchor's lead-in. Returns how many joined.
	 *
	 * An anchor that already has an incident edge is skipped, so a hand-drawn connection
	 * wins over the automatic one rather than being doubled up by it.
	 */
	static int32 Build(URoadNetwork& Network, double MaxLeadIn = DefaultMaxLeadIn);
};
