#pragma once

#include "CoreMinimal.h"
#include "Build/RoadMeshSink.h"

class URoadNetwork;

/**
 * The PAINT of a holding position: the ground marking across the taxiway at a flagged
 * guideline node, as mesh buffers for a component of its own.
 *
 * Two patterns, from the markings themselves (spec 2026-09-07 §6):
 *  - RUNWAY: two solid bars nearest the aircraft, two dashed bars beyond them toward the
 *    runway. An aircraft stops with its whole body behind the solid pair; crossing from the
 *    runway side it passes the dashed pair first and is clear once past the solid ones.
 *  - INTERMEDIATE: one dashed bar.
 *
 * Every vertex carries UV1 = (0, 0). M_RoadSurface paints its centreline from UV1.X, the
 * lateral offset, as mask = 1 - saturate((|lateral| - CentrelineWidth) * Sharpness); at
 * lateral 0 the mask is 1 over the whole quad and the quad is MarkingColor. That is the
 * "solid yellow slab" Airside.Build.ProfileFallback documents as a defect on a road - and
 * exactly the paint a marking wants, at the cost of no new material asset (authoring one
 * needs the editor closed, and this needs none).
 *
 * A Build/ class because it derives geometry from the model, like FRoadMeshBuilder, and
 * for the same reason it is not part of it: a marking corner that lands on a road vertex
 * must not weld to it. The two surfaces meet; they are not one surface.
 */
struct AIRSIDE_API FHoldingPositionMarkingBuilder
{
	/** Line width and the gap between lines, uu. Exaggerated from ICAO's 0.15 m so they read from the build camera. */
	static constexpr double LineWidth = 30.0;
	static constexpr double LineGap = 30.0;
	/** A dash and the gap after it, along the bar, uu. */
	static constexpr double DashLength = 90.0;
	static constexpr double DashGap = 90.0;

	/**
	 * Append the markings of every holding position in Network to Out, in the road plane at Z.
	 * Returns how many positions were painted.
	 *
	 * The bar lies ACROSS the node's own segment (Origin) - the taxiway the aircraft is on -
	 * and the pattern sits on the far side of the node in the direction of travel toward the
	 * junction, so a nose stopped AT the node is exactly behind the first solid line. A node
	 * with no Origin (an anchor, a hand-placed node) uses its first incident edge for both.
	 */
	static int32 Build(const URoadNetwork& Network, double Z, FRoadMeshBuffers& Out);
};
