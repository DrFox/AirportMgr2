#pragma once

#include "CoreMinimal.h"

class UEntityDefinition;
struct IToolPreviewSink;

/**
 * Describes a stand at a pose to a preview sink.
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
	ROADNET_API void Describe(const UEntityDefinition* Definition, const FVector2D& At,
		double Heading, IToolPreviewSink& Sink);
}
