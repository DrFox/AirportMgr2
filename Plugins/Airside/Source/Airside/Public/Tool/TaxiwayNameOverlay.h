#pragma once

#include "CoreMinimal.h"

class URoadNetwork;
struct IToolPreviewSink;

/**
 * The taxiway names, described to a sink as EPreviewStyle::TaxiwayName labels at TaxiwayLabels::Anchors - ONE call
 * both drivers make (ARoadBuildHUD::DrawHUD, URoadBuildEditorTool::DrawPersistentState), GuidelineOverlay's shape, so
 * they cannot place names differently. WHETHER to call it is FBuildSession::WantsTaxiwayNamesDrawn's. Returns how many
 * labels it described, for the drivers' once-per-change log line (a visual feature the suite cannot see).
 * ENFORCED BY: Airside.Present.TaxiwayNames.LabelsMatchNames, Airside.Editor.TaxiwayNamesReachTheEditor,
 * Check-Architecture rule 26 (both drivers call it)
 */
namespace TaxiwayNameOverlay
{
	AIRSIDE_API int32 Describe(const URoadNetwork& Network, double RepeatEvery, IToolPreviewSink& Sink);
}
