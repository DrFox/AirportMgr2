#pragma once
#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * Buy land (land purchase spec R6): every buyable tile past the edge as a ghost with its price; a click buys
 * the tile under the cursor. Unowned tiles that cannot be bought are not drawn - the void stays void.
 * ITS OWN MODE, not a gesture on the plain view, by the deliberate-mode ruling: buying spends real money.
 */
class AIRSIDE_API FLandBuyTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;
	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override {}
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const override;
	virtual bool IsIdle() const override { return true; }
};
