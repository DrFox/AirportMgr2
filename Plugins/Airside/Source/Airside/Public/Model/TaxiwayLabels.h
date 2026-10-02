#pragma once

#include "CoreMinimal.h"

class URoadNetwork;

/** One taxiway name to show and the road-plane point it belongs at - a MEANING (Tool/'s rule): the driver picks the look. */
struct FTaxiwayLabel
{
	int32 Taxiway = INDEX_NONE;
	FString Name;
	FVector2D At = FVector2D::ZeroVector;
};

/**
 * Where taxiway names go (spec "UI"): one per live taxiway WITH segments, at the middle of its longest segment (ties to
 * the first in chain order), repeated every RepeatEvery uu along the chain both ways while on it. Model/, world-free,
 * so the two drivers draw one answer (Tool/TaxiwayNameOverlay hands it to their sinks).
 * ENFORCED BY: Airside.Model.TaxiwayNames.LabelAnchors, Airside.Present.TaxiwayNames.LabelsMatchNames
 */
namespace TaxiwayLabels
{
	AIRSIDE_API TArray<FTaxiwayLabel> Anchors(const URoadNetwork& Network, double RepeatEvery);
}
