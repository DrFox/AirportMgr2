#pragma once

#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadEntity.h"
#include "Tool/OutlineDrawTool.h"

/**
 * What a depot plot's closing outline commits to, and what ctrl removes.
 *
 * The apron's sibling - see FApronOutlineTarget and IOutlineTarget. Holds the module mix
 * because a plot's commit needs it and the gesture does not: the state machine knows only
 * about corners, which is what let it be shared in the first place.
 */
struct AIRSIDE_API FPlotOutlineTarget final : public IOutlineTarget
{
	EPlaceableEntity Kind = EPlaceableEntity::FuelDepot;

	/**
	 * What to put in the bays, in bay order. Modules past the last bay are dropped by the
	 * facade, which says so - see URoadEditFacade::PlaceEntityInPlot.
	 *
	 * DEFAULTED TO THE CONCEPT SHEET'S DEPOT - one shed, one tank, one pump - so pressing
	 * the key and drawing a three-bay plot gives the Tier 1 depot as drawn, with no UI to
	 * choose a mix in yet. That UI is the expected next slice, not this one.
	 */
	TArray<EDepotModule> Modules = {
		EDepotModule::Shed, EDepotModule::Tank, EDepotModule::Pump };

	virtual bool Commit(const FToolContext& Context, const TArray<FVector2D>& Corners) const override;
	virtual bool RemoveUnderCursor(const FToolContext& Context) const override;
	virtual void PreviewRemoval(const FToolContext& Context, IToolPreviewSink& Sink) const override;
};

/**
 * Drawing a fuel depot as a PLOT: a polygon whose bays the installation fills.
 *
 * Replaces FStandPlaceTool on the depot's key. A depot is drawn now rather than stamped,
 * which is the whole point of the plot slice - see the design doc. The stand keeps the
 * press-drag-release gesture, because a stand has no plot: its extent is its design
 * aircraft's, and a rectangle round it would be a second opinion about how big it is.
 *
 * The polygon gesture itself is FOutlineDrawTool's, shared with the apron tool.
 */
class AIRSIDE_API FPlotDrawTool : public FOutlineDrawTool
{
public:
	explicit FPlotDrawTool(EPlaceableEntity Kind) { Target.Kind = Kind; }

	virtual FText GetDisplayName() const override;

	/** The mix the next commit will lay. For tests, and for the mix UI when it arrives. */
	void SetModules(const TArray<EDepotModule>& Modules) { Target.Modules = Modules; }

protected:
	virtual const IOutlineTarget& GetOutlineTarget() const override { return Target; }

private:
	FPlotOutlineTarget Target;
};
