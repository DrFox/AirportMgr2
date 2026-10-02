#pragma once

#include "CoreMinimal.h"
#include "Tool/OutlineDrawTool.h"

/**
 * What an apron's closing outline commits to, and what ctrl removes.
 *
 * The three things that differ between drawing an apron and drawing a depot's plot - see
 * IOutlineTarget, which was extracted from this tool when the second one arrived. The
 * gesture itself, the closing rule and the self-crossing rule are shared and live once.
 */
struct AIRSIDE_API FApronOutlineTarget final : public IOutlineTarget
{
	virtual bool Commit(const FToolContext& Context, const TArray<FVector2D>& Corners) const override;
	virtual bool RemoveUnderCursor(const FToolContext& Context) const override;
	virtual void PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const override;
	/** IRoadEditTarget::WhyApronRefused - AddApron asks the same at the click. */
	virtual FString WhyRefused(const FToolContext& Context, TArrayView<const FVector2D> Corners) const override;
};

/**
 * Drawing an apron: a polygon of pavement with no cross-section and no junctions.
 *
 * Its own tool rather than a mode of the road tool, because almost nothing is shared - an
 * apron has no profile, no bands, no solve, and its input is a closing polygon rather than
 * a chain. Selected with 2; roads are 1.
 *
 * The polygon GESTURE lives in FOutlineDrawTool, which this tool is the ONLY client of (#450).
 * ENFORCED BY: Check-Architecture rule 4 row 'IOutlineTarget / FOutlineDrawTool implementors'
 *
 * It was shared with FPlotDrawTool until that tool was deleted - a plot is now a four-corner rectangle, drawn by FPlotPlaceTool through
 * FStagedPlotTool. That is not a reversal of the paragraph above: what was shared is the input, which was the one thing
 * an apron and a plot had in common, and what stays separate is everything the outline then becomes.
 */
class AIRSIDE_API FApronDrawTool : public FOutlineDrawTool
{
public:
	virtual FText GetDisplayName() const override;

protected:
	virtual const IOutlineTarget& GetOutlineTarget() const override { return Target; }

private:
	FApronOutlineTarget Target;
};
