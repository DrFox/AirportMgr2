#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadBuildTool.h"

/**
 * The default state: click a thing, see its facts, press a verb. Registry index 0, so the
 * session opens here and an idle build tool's cancel returns here (FBuildSession).
 *
 * The one tool that BUILDS NOTHING, inheriting that title from the Route tool it replaced.
 * The Route tool asked the airport a question by making it drive; this asks by pointing.
 *
 * PICKING IS SPLIT BY WHAT CAN BE HIT ON THE PLANE. Stands are on the plane and FindEntityAt
 * finds them from Context.Cursor. Aircraft may not be - one on final is 2000 uu up - so the
 * driver projects them and hands the nearest in Context.HoverAgent; this tool never sees a
 * camera. Aircraft beats stand: a parked aircraft covers its stand and the smaller target
 * should win, or the stand would be the only thing selectable once an aircraft is on it.
 *
 * Stateless apart from what it writes to Context.Selection: the selection belongs to the
 * session so the panel can read it after this tool has been deactivated by a build tool
 * (which clears it) or reactivated (which does not).
 */
class AIRSIDE_API FSelectTool : public IBuildTool
{
public:
	virtual FText GetDisplayName() const override;
	virtual void OnClick(const FToolContext& Context) override;
	virtual void OnCancel(const FToolContext& Context) override;
	virtual void Tick(const FToolContext& Context) override;
	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const override;

	/** Idle when nothing is selected, so a second cancel falls through to the driver. */
	virtual bool IsIdle() const override { return !bHasSelection; }

private:
	/** Mirror of Context.Selection->IsSet() from the last call, because IsIdle takes no context. */
	bool bHasSelection = false;

	/** Road-plane position of a selection, or false when it no longer exists. */
	static bool PositionOf(const FToolContext& Context, ESelectionKind Kind, int32 Id, FVector2D& Out);
};
