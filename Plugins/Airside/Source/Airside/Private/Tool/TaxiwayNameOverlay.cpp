#include "Tool/TaxiwayNameOverlay.h"

#include "Model/TaxiwayLabels.h"
#include "Tool/RoadBuildTool.h"

int32 TaxiwayNameOverlay::Describe(const URoadNetwork& Network, double RepeatEvery, IToolPreviewSink& Sink)
{
	const TArray<FTaxiwayLabel> Labels = TaxiwayLabels::Anchors(Network, RepeatEvery);
	for (const FTaxiwayLabel& Label : Labels)
	{
		Sink.Label(Label.At, Label.Name, EPreviewStyle::TaxiwayName);
	}
	return Labels.Num();
}
