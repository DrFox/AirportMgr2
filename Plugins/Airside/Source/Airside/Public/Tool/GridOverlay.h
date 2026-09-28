#pragma once

#include "CoreMinimal.h"

struct FToolContext;
struct IToolPreviewSink;

/**
 * Draws the grid round the build point - faint graph paper under the gesture, world-aligned or
 * turned, whichever FToolContext::GridFrame says.
 *
 * ONE EMITTER, CALLED BY BOTH DRIVERS before the active tool's own preview (ARoadBuildHUD and
 * URoadBuildEditorTool), in GuidelineOverlay's shape and for its reason: the grid is context
 * true whatever the gesture, so no tool owns it.
 * ENFORCED BY: Check-Architecture rule 26 (grid-overlay-both-drivers).
 *
 * GATED ON THE CONTEXT, NOT ON THE SETTING. FToolContext::GridFrame is on only when the
 * grid will actually move the point this frame (FBuildSession::MakeContext), so the overlay
 * cannot show a grid under Select, under Alt, or under any tool that ignores it.
 *
 * CENTRED ON THE GUIDED CURSOR, so the lines shown are the ones the point is landing on.
 */
namespace GridOverlay
{
	/** Grid pieces within Context.GridOverlayRadiusUu, or nothing when GridFrame is off. */
	AIRSIDE_API void Describe(const FToolContext& Context, IToolPreviewSink& Sink);
}
