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

	/**
	 * Idle when nothing is selected, so a second cancel falls through to the driver.
	 *
	 * READS SelectionRef LIVE rather than a bool mirror updated on the last OnClick/OnCancel/
	 * Tick - see that field's comment for why a mirror could not be trusted here.
	 */
	virtual bool IsIdle() const override { return SelectionRef == nullptr || !SelectionRef->IsSet(); }

private:
	/**
	 * The session's own selection - FToolContext::Selection, which always points at
	 * FBuildSession::Selection - captured the first time a context carries one.
	 *
	 * NOT A SNAPSHOT, AND THAT IS THE POINT. IsIdle() takes no context (IBuildTool's contract,
	 * shared with every other tool), so a bool mirror can only be as fresh as the last OnClick,
	 * OnCancel or Tick that touched it - and IsIdle() is called from PLACES BEFORE THAT:
	 * IBuildTool::DescribeGuideAnchor calls it from inside FBuildSession::MakeContext, i.e.
	 * while THIS FRAME's context is still being built, before this frame's Tick has run. A
	 * mirror is one frame stale exactly there, which is invisible until something else - the
	 * inspector panel closing a stand's selection when it is deleted - changes Selection
	 * between two of this tool's own calls. FBuildSession::Selection is a field that outlives
	 * every frame rather than being rebuilt with the context, so the pointer stays valid for
	 * the tool's whole lifetime once set, and dereferencing it always reads today's truth.
	 */
	const FSelection* SelectionRef = nullptr;

	/** Road-plane position of a selection, or false when it no longer exists. */
	static bool PositionOf(const FToolContext& Context, ESelectionKind Kind, int32 Id, FVector2D& Out);
};
