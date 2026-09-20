#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Tool/EditTool.h"
#include "Tool/RoadBuildTool.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"
#include "Tool/Selection.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/SnapGuideSettings.h"

class URoadNetwork;
class IRoadEditTarget;

/**
 * Build or Edit - what a gesture MEANS, across every tool.
 *
 * AN ENUM AND NOT A bool (CLAUDE.md: a phase is an enum, never a set of bools). Two values
 * today; it leaves room for the third this design deliberately did not build - Upgrade,
 * repainting an existing road to the current width - without a second flag that could be
 * true at the same time as this one.
 *
 * ON THE SESSION, not on either driver, so PIE and URoadBuildEdMode cannot disagree about
 * it. FRoadSnapSettings' own header records what the two drivers holding private copies of
 * a shared decision cost the last time: the same click snapped differently depending only
 * on which one was open.
 */
enum class EGestureMode : uint8
{
	/** Tools lay things. The mode every session opens in. */
	Build,

	/** Tools lay nothing; FEditTool moves what is already placed. */
	Edit
};

/**
 * One selectable tool: the key that picks it, its display name, its tooltip, and how to
 * make one.
 *
 * A registration rather than a bare TFunction, because a tool needs a KEY and a NAME
 * before it needs to exist - the startup banner and the editor's command list both want
 * those without constructing six IBuildTools to ask.
 *
 * TOOLTIP JOINED THE OTHER TWO (issue #105 item 10): FRoadBuildEdModeCommands used to hand-
 * author one UI_COMMAND per tool beside this table, with the LABEL checked against this
 * one's Name by string at URoadBuildEdMode::Enter - a runtime check that the two lists still
 * agreed, rather than there being one list. FRoadBuildEdModeCommands now builds its
 * ToolCommands FROM this table directly (FUICommandInfo::MakeCommandInfo, not the UI_COMMAND
 * macro, which requires a compile-time named field per command), so Tooltip has to live
 * here too - the one thing the old hand-written commands carried that this table did not.
 */
struct FToolRegistration
{
	FKey Key;

	/**
	 * STABLE, unlocalised (PR #137 review, issue #105 item 10): FRoadBuildEdModeCommands used
	 * to build each command's FName straight from Name below, which is LOCTEXT and therefore
	 * whatever the current culture renders it as - baking a translation into the command id
	 * FUICommandInfo persists to the editor's per-user keybindings ini. A locale change would
	 * have orphaned every saved keybinding silently. This is the one field nothing localises.
	 */
	FName Id;

	FText Name;
	FText Tooltip;
	TFunction<TUniquePtr<IBuildTool>()> Make;

	/**
	 * What this tool exposes while the session's mode is Edit. None greys the toggle out.
	 *
	 * HERE rather than in a table inside FEditTool, because a tool and what it lets you
	 * edit are one fact about that tool - CLAUDE.md's "lists that must agree are ONE list",
	 * applied before the second list exists rather than after it has drifted. Read in
	 * exactly one place, FBuildSession::MakeContext, and asserted entry-by-entry by name in
	 * Airside.Tool.EditHandlesAreDeclaredForEveryRegistryEntry.
	 */
	EEditHandleKind EditHandles = EEditHandleKind::None;
};

/**
 * The tools both drivers offer: Select (4), Taxiway (1), Apron (2), Stand (3), Guidelines
 * (5), Runway (6), Holding point (8), Road (9), Fuel depot (0). Index 0 is Select, the
 * default state.
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

	/** Which guide sources are live. From ARoadNetworkActor, like Snap above and for the
	 *  same reason: the two drivers must agree about what is switched on. */
	FSnapGuideSettings GuideSources;

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
	/** Builds Tools from ToolRegistry(), in registry order. Index 0 - Select - starts active. */
	FBuildSession();

	/** The tool the number keys selected, or null before any tool has been made. */
	IBuildTool* GetActiveTool() const;

	/** Registry index of the active tool. What the bar lights; what SelectTool takes.
	 *  UNAFFECTED BY THE MODE: the lit tool still says which handles Edit exposes, so the
	 *  bar keeps showing Taxiway lit while Edit is held over it. */
	int32 GetActiveToolIndex() const { return ActiveTool; }

	/** Build or Edit. A session opens in Build: a mode that survived construction would be
	 *  a drag the player never asked to be able to make. */
	EGestureMode GetGestureMode() const { return Mode; }

	/**
	 * Switch between building and editing, deactivating the outgoing tool first so nothing
	 * is left part-drawn to reappear - the same argument SelectTool makes for the same call.
	 *
	 * DeactivateContext is the CALLER's, for the reason SelectTool's own comment gives: the
	 * session holds no IRoadEditTarget and has nothing to build one from.
	 */
	void SetGestureMode(EGestureMode InMode, const FToolContext& DeactivateContext = FToolContext());

	/** How many tools this session holds. For tests: must equal ToolRegistry().Num(). */
	int32 NumTools() const { return Tools.Num(); }

	/** What the Select tool has picked. Read by the panel and the HUD; written only through
	 *  FToolContext::Selection, which MakeContext points here. */
	const FSelection& GetSelection() const { return Selection; }

	/**
	 * Switches the active tool, deactivating the outgoing one first so nothing is left
	 * part-drawn to reappear on the next selection.
	 *
	 * DeactivateContext is the CALLER's context, not one built here: the session holds no
	 * IRoadEditTarget of its own, so it has nothing to build one from. Every IBuildTool's
	 * OnDeactivate already guards Context.Target for exactly this reason - passing a
	 * default-constructed FToolContext is safe when there is nothing more specific yet.
	 *
	 * Selecting the tool that is ALREADY active is not a no-op: it is IBuildTool::OnReselect,
	 * with the same context (its modifiers say what to cycle). See that method for why.
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
	 *
	 * HoverAgent is the driver's screen-space pick, 0 when none - see FToolContext::HoverAgent.
	 */
	FToolContext MakeContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
		const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
		bool bSuspendGuides = false, int32 HoverAgent = 0) const;

	/**
	 * Right click (or Escape, in the editor): step back out of whatever is part-drawn. With
	 * nothing part-drawn in a build tool, put the tool down and return to Select (index 0).
	 */
	void CancelActiveGesture(const FToolContext& Context);

	/**
	 * Remember a road-plane position a driver's ray/plane test actually resolved.
	 *
	 * The one home for what used to be two separate copies - `ARoadBuildController::
	 * LastPlaneHit` and `URoadBuildEditorTool::HoverPosition` - see issue #92. Both drivers
	 * need SOME position on a frame whose own hit test refuses (a click above the horizon,
	 * a ray parallel to the plane): the ghost and the snap chain run every tick and cannot
	 * simply skip the frame. Owned here rather than by either driver so a fallback the
	 * runtime learns and one the editor learns cannot silently diverge.
	 *
	 * const, like MakeContext: the callers that learn a plane hit (CursorOnRoadPlane,
	 * RayToPlane) are themselves const reporting methods, not decisions - see
	 * FBuildSession's own `mutable Selection` for the same reasoning applied here.
	 */
	void RecordPlaneHit(const FVector2D& Hit) const { LastPlaneHitValue = Hit; }

	/** The last position RecordPlaneHit was given, or the origin before either driver has hit anything. */
	const FVector2D& LastPlaneHit() const { return LastPlaneHitValue; }

private:
	/**
	 * The selectable tools, in key order: index 0 is key 1.
	 *
	 * Strategy, not a state machine - see IBuildTool. A tool is picked, never transitioned
	 * into, so what lives here is a list and an index rather than a transition graph.
	 */
	TArray<TUniquePtr<IBuildTool>> Tools;

	int32 ActiveTool = 0;

	EGestureMode Mode = EGestureMode::Build;

	/**
	 * The tool that runs while Mode is Edit.
	 *
	 * A MEMBER rather than an entry in Tools: it is not selected by a number key and must
	 * not appear on the tool row - see FEditTool's own header. Held by value because it is
	 * exactly one tool with no alternative to swap in, so the TUniquePtr the registry needs
	 * would buy nothing here.
	 *
	 * mutable for the reason Selection is: GetActiveTool is const and must hand out a
	 * pointer the driver drives.
	 */
	mutable FEditTool EditTool;

	/**
	 * mutable: MakeContext is const (see FBuildSessionTunables for why that was fought for)
	 * and must hand out a pointer the tool can write through. The selection is tool state
	 * parked on the session so it outlives the tool being active.
	 */
	mutable FSelection Selection;

	/**
	 * Rule 1 then rule 2, in that order. Not a UPROPERTY: it owns its rules through
	 * TUniquePtr and holds no state worth saving, only the ordering.
	 */
	FRoadSnapChain SnapChain;

	/**
	 * Extending then World. Not a UPROPERTY, like SnapChain above: it owns its sources
	 * through TUniquePtr and holds no state worth saving, only the ordering.
	 */
	FSnapGuideChain GuideChain;

	/**
	 * THE PREVIOUS WINNER - the one piece of state the flicker rule needs, and the reason
	 * the DRIVER resolves the guide rather than the tool. IBuildTool::BuildPreview and
	 * BuildReadout are both const and could not hold it; a member on the tool would also put
	 * it on the wrong side of the two-driver split, where PIE and the editor mode each kept
	 * their own copy of a number and drifted.
	 *
	 * mutable for the same reason Selection and LastPlaneHitValue are: MakeContext is const,
	 * deliberately (see FBuildSessionTunables), and this is a reported answer, not a decision.
	 *
	 * CLEARED WHENEVER NO TOOL OFFERS AN ANCHOR, so a winner cannot outlive the gesture that
	 * earned it and then be HELD into the next one by the hysteresis rule itself.
	 */
	mutable SnapGuide::FResult LastGuide;

	/** See RecordPlaneHit/LastPlaneHit. mutable for the same reason Selection is. */
	mutable FVector2D LastPlaneHitValue = FVector2D::ZeroVector;
};
