#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

class URoadNetwork;

/**
 * The geometry of a holding-position marking at a flagged guideline node: which way the
 * pattern extends, its perpendicular, and how wide the taxiway is there. In ROAD PLANE
 * coordinates, like everything Tool/ hands a sink and everything Build/ derives a mesh from.
 *
 * Toward is zero on a node HoldingBarAt could not find a direction for (see its own comment);
 * IsSet() is that check spelled out once rather than at every call site.
 */
struct AIRSIDE_API FHoldingBarFrame
{
	/** Unit direction from the node INTO the junction - the side the pattern sits on. */
	FVector2D Toward = FVector2D::ZeroVector;

	/** Toward's left-hand perpendicular - across the taxiway, the axis the bar is drawn along. */
	FVector2D Across = FVector2D::ZeroVector;

	/**
	 * Half the taxiway's width at the node, uu. NOT floored to any minimum line width - that
	 * is a paint concern (a bar must never be zero-thick), applied by the one caller that
	 * draws a quad from it, not baked into a fact about the pavement.
	 */
	double HalfWidth = 0.0;

	bool IsSet() const { return !Toward.IsNearlyZero(); }
};

/**
 * THE ONE EVALUATOR of a holding-bar's geometry (issue #307, CLAUDE.md #255: fix the SHAPE,
 * not the site). Before this, GuidelineOverlay drew its cross-mark along the guideline EDGE's
 * chord to whichever node was on the other end, while FHoldingPositionMarkingBuilder painted
 * its bar along the ROAD SEGMENT's own tangent at the node's Origin - and a comment on the
 * builder claimed the two were "exactly" the same fallback. They are not: a chord and a
 * tangent coincide only for a straight, centred guideline, which is every fixture that existed
 * before this issue (Airside.Build.HoldingPositionMarking, ...AtFullWidth, the runway-crossing
 * fixture) - none of them curve, so none of them could have caught the divergence. The moment
 * the taxiway curves, or its holding-position end is set back from the node along its own
 * tangent (RoadGuidelineBuilder.cpp's SetBack, ~line 446), the chord to whatever guideline
 * node happens to be on the far side of the edge stops pointing where the tangent does, and
 * the overlay showed a bar facing one way while the paint laid it facing another.
 *
 * Bitwise agreement between what the overlay shows and what the paint lays is now guaranteed
 * by construction: both call this and nothing else. See Airside.Model.HoldingBarFrame, which
 * is what goes red if either grows a second evaluator again.
 */
AIRSIDE_API FHoldingBarFrame HoldingBarAt(const URoadNetwork& Network, FGuidelineNodeId Node);
