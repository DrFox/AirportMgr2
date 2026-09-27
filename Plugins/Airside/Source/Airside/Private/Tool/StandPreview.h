#pragma once

#include "CoreMinimal.h"

class UEntityDefinition;
struct IToolPreviewSink;

/**
 * Describes an installation - a stand, a fuel depot - at a pose to a preview sink.
 *
 * Shared because a stand has to look the same whether it is being aimed and has no
 * existence in the model yet, or was placed ten minutes ago. It was written once inside
 * the placement tool, so the editor's view of a PLACED stand was a ring and a tick while
 * the in-progress one showed its aircraft and every anchor - the same object drawn two
 * different ways depending on which code path found it.
 *
 * Takes a pose rather than an FEntityInstance for exactly that reason: the thing being
 * aimed is not in the graph yet and has nothing to be asked about.
 */
namespace StandPreview
{
	/** DescribeBody, then the stop mark at At - what a placement tool aims with. */
	AIRSIDE_API void Describe(const UEntityDefinition* Definition, const FVector2D& At,
		double Heading, IToolPreviewSink& Sink);

	/**
	 * Everything EXCEPT the stop mark: design aircraft, service points, footprint box, fixtures.
	 *
	 * Split out for GraphOverlay, which draws PLACED installations all the time, at every zoom.
	 * While aiming, the stop mark is the cursor - the thing being positioned - so the tool keeps
	 * it for every kind. Once placed it is an aircraft's nose stop, and on a fuel depot it was a
	 * pair of rings on the road that nobody could name (2026-09-27, zoomed-out readability).
	 */
	AIRSIDE_API void DescribeBody(const UEntityDefinition* Definition, const FVector2D& At,
		double Heading, IToolPreviewSink& Sink);
}
