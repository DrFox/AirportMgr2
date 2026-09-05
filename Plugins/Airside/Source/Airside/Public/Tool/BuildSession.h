#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

class URoadNetwork;
class IRoadEditTarget;

/**
 * One selectable tool: the key that picks it, its display name, and how to make one.
 *
 * A registration rather than a bare TFunction, because a tool needs a KEY and a NAME
 * before it needs to exist - the startup banner and the editor's command list both want
 * those without constructing six IBuildTools to ask.
 */
struct FToolRegistration
{
	FKey Key;
	FText Name;
	TFunction<TUniquePtr<IBuildTool>()> Make;
};

/**
 * The tools both drivers offer, in key order 1..6: Road, Apron, Stand, Route, Guideline,
 * Runway.
 *
 * ONE table, read by both `ARoadBuildController` and `URoadBuildEditorTool` - see
 * CLAUDE.md's "check where a list is CONSUMED, not where it is declared". Before issue
 * #33 the game module kept a `Tools` array, a `ToolKeyCount`, six `BindKey` calls and a
 * hand-written banner that had to agree by hand, and the editor module kept a fourth,
 * shorter copy that had drifted to four tools out of six. A `TConstArrayView` over a
 * function-local static rather than a global `TArray`, so there is exactly one place the
 * table is built and no second owner that could be constructed in a different order.
 */
AIRSIDE_API TConstArrayView<FToolRegistration> ToolRegistry();

/**
 * The tunables a click is judged against, as one bundle rather than three separate
 * arguments to MakeContext.
 *
 * These used to live as mutable members on FBuildSession, pushed in by the caller just
 * before each MakeContext call - an ordering contract nothing enforced, and the reason
 * MakeToolContext/MakeContext/MakeContextAt/MakeHoverContext all had to lose their `const`
 * (they needed to write the members before reading them back). Passing them as one value
 * instead means MakeContext can stay const, and there is no "did you remember to push
 * first" question to get wrong.
 */
struct FBuildSessionTunables
{
	/** Radii and toggles the snap chain judges a click against. */
	FRoadSnapSettings Snap;

	/** Shortest segment and tightest turn a click may build. */
	FRoadPlacementLimits Limits;

	/**
	 * How close, in uu, the cursor counts as "on" something a tool is asking about.
	 *
	 * SEPARATE from Snap's own radii, which decide where a ROAD NODE lands - see
	 * ARoadBuildController::ToolPickRadius, whose comment this one keeps.
	 */
	double ToolPickRadius = 400.0;
};

/**
 * Owns the tool session both build drivers drive: which tools exist and which one is
 * active.
 *
 * Before issue #33 this state was `ARoadBuildController`'s alone, and `URoadBuildEditorTool`
 * carried its own second copy - a `switch` over four tools instead of the runtime's six, its
 * own `FRoadSnapChain`, and its own ad hoc `FToolContext` assembly. The two had already
 * drifted (no guideline tool, no runway tool, in the editor) before this existed to stop it.
 *
 * It knows nothing about cameras, input devices, or worlds - see `MakeContext`, which takes
 * the plane hit, the tunables and the modifier state as arguments rather than reading a
 * mouse or a `PlayerController` itself. That is what lets both an `APlayerController` and a
 * plain `UInteractiveTool` hold one.
 */
class AIRSIDE_API FBuildSession
{
public:
	/** Builds Tools from ToolRegistry(), in registry order. The first tool starts active. */
	FBuildSession();

	/** The tool the number keys selected, or null before any tool has been made. */
	IBuildTool* GetActiveTool() const;

	/** How many tools this session holds. For tests: must equal ToolRegistry().Num(). */
	int32 NumTools() const { return Tools.Num(); }

	/**
	 * Switches the active tool, deactivating the outgoing one first so nothing is left
	 * part-drawn to reappear on the next selection.
	 *
	 * DeactivateContext is the CALLER's context, not one built here: the session holds no
	 * IRoadEditTarget of its own, so it has nothing to build one from. Every IBuildTool's
	 * OnDeactivate already guards Context.Target for exactly this reason - passing a
	 * default-constructed FToolContext is safe when there is nothing more specific yet.
	 */
	void SelectTool(int32 Index, const FToolContext& DeactivateContext = FToolContext());

	/**
	 * What a click at PlaneHit would resolve to, over Network, judged by Snap.
	 *
	 * Before the first node exists there is no network to search - the facade builds one
	 * lazily inside PlaceNode. Free at the cursor is the right answer here, not a refusal:
	 * treating a missing network as failure would make the first click of every session do
	 * nothing at all. Always returns true; the bool stays for symmetry with the shape the
	 * two drivers used to call this as, and so a future rule that CAN refuse - an edit lock,
	 * say - has somewhere to return false from without changing every call site.
	 */
	bool ResolveSnap(const URoadNetwork* Network, const FVector2D& PlaneHit,
		const FRoadSnapSettings& Snap, FRoadSnapResult& Out) const;

	/**
	 * Everything a tool needs to judge PlaneHit, with the snap chain already run over it.
	 *
	 * Cursor is set to the RAW hit and Snap is carried BESIDE it, never folded into it - see
	 * FToolContext::SetCursor. A tool that does not build roads (the route tool picking a
	 * guideline node, the stand tool placing a pose) wants the raw mouse position, and
	 * handing it a road-snapped value silently applies road-building semantics to work that
	 * has none.
	 */
	FToolContext MakeContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
		const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier) const;

	/** Right click (or Escape, in the editor): step back out of whatever is part-drawn. */
	void CancelActiveGesture(const FToolContext& Context);

private:
	/**
	 * The selectable tools, in key order: index 0 is key 1.
	 *
	 * Strategy, not a state machine - see IBuildTool. A tool is picked, never transitioned
	 * into, so what lives here is a list and an index rather than a transition graph.
	 */
	TArray<TUniquePtr<IBuildTool>> Tools;

	int32 ActiveTool = 0;

	/**
	 * Rule 1 then rule 2, in that order. Not a UPROPERTY: it owns its rules through
	 * TUniquePtr and holds no state worth saving, only the ordering.
	 */
	FRoadSnapChain SnapChain;
};
