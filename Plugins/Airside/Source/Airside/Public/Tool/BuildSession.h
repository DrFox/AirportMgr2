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
#include "BuildSession.generated.h"

class URoadNetwork;
class IRoadEditTarget;
class FBuildSession;

/**
 * What a gesture MEANS right now, across every tool. Exactly one of these is true.
 *
 * ONE ENUM, NOT A SET OF FLAGS (CLAUDE.md: a phase is an enum, never a set of bools), and
 * this enum has already been split once and had to be put back. Remove and Insert lived
 * here as EClickModifier - whose own test says "ONE ENUM, NOT TWO BOOLS: the illegal
 * 'remove and insert at once' is unrepresentable" - and Edit arrived beside it as a
 * SECOND field on a different object. Remove and Edit could then both be lit, the bar drew
 * both, and Edit silently won because it decides which tool runs at all. Reported from
 * play, 2026-09-20. Merging them is what makes "one mode at a time" a fact about the type
 * rather than a rule two setters have to remember about each other.
 *
 * ON THE SESSION, not on either driver, so PIE and URoadBuildEdMode cannot disagree about
 * it - FRoadSnapSettings' header records what private per-driver copies of a shared
 * decision cost the last time. Remove and Insert moved here from ARoadBuildController for
 * that reason as well as this one: the editor mode had no sticky modifier at all, and now
 * gets one for free.
 *
 * STICKY, and the held keys are separate. Ctrl and Shift still mean Remove and Insert while
 * held - MakeContext ORs the two - because a modifier you hold and a mode you enter are
 * different gestures for different lengths of work.
 *
 * A UENUM PURELY SO THE COVERAGE TEST CAN COUNT IT. Nothing here needs Blueprint or
 * serialisation; what it needs is for AirportMgr.Actions.EveryGestureModeIsHandled to walk
 * the members through reflection rather than up to a hand-written bound. FRoadBuildHUDLooksTest
 * has that hand-written bound and says so - "THIS BOUND IS THE FIFTH LIST a new style has to
 * appear in, and the only one nothing else would have caught". This enum does not repeat it.
 */
UENUM()
enum class EGestureMode : uint8
{
	/** Tools lay things. The mode every session opens in. */
	Build,

	/** Ctrl, made sticky: a gesture removes rather than builds. */
	Remove,

	/** Shift, made sticky: a gesture inserts without starting anything. */
	Insert,

	/** Tools lay NOTHING; FEditTool moves what is already placed. */
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

	/**
	 * Whether the committed graph's NODE RINGS are drawn while this tool is lit.
	 *
	 * OFF BY DEFAULT, because they are scaffolding: reported from play as "when a road is
	 * not on edit or create mode we should not see the white circle junction nodes". An
	 * airport you are looking at rather than building should read as an airport.
	 *
	 * TRUE ONLY WHERE A TOOL READS ROAD NODES, which is FRoadDrawTool's two entries and
	 * nothing else - they snap to a node to chain from it or close a junction, so the ring
	 * is the target you are aiming at. The tools that look like counterexamples are not:
	 * the holding-point and guideline tools pick GUIDELINE nodes, which GuidelineOverlay
	 * draws under its own G toggle, and the runway tool reads no snap at all.
	 *
	 * A SECOND FIELD BESIDE EditHandles rather than derived from it, because the two
	 * genuinely differ: Runway exposes an edit handle and needs no rings while building.
	 * Deriving one from the other would need an exception list, which is the second list
	 * this field exists to avoid.
	 */
	bool bShowsRoadNodes = false;
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
 * One verb that is not a tool: the sticky EGestureMode trio, Remove/Insert/Edit. Bundles what
 * FRoadBuildEdModeCommands needs to register a UI_COMMAND (Id/Name/Tooltip/Key) with what runs
 * it against the shared FBuildSession - ToolRegistry()'s own shape (Key/Id/Name/Tooltip plus a
 * maker), applied to the other thing a key can mean besides picking a tool.
 *
 * ISSUE #304: `grep GestureMode AirsideEditor/Private/*.cpp` was 0 - nothing in the editor
 * module ever called SetGestureMode/ToggleGestureMode, though BuildActions.cpp has carried
 * Remove/Insert/Edit as bar rows since the mode was unified onto FBuildSession (EGestureMode's
 * own header comment). This table is now the ONE list both BuildActions.cpp's three rows and
 * FRoadBuildEdModeCommands::RegisterCommands's loop are built from, so a fourth sticky mode
 * cannot be added to one and missed in the other - CLAUDE.md's "check where a list is
 * CONSUMED, not where it is declared", the exact shape ToolRegistry() already closed for tools.
 *
 * BUILD AND CANCEL ARE DELIBERATELY NOT HERE, though the issue's own review named them beside
 * these three. Both already have a correct, working command in BOTH drivers today
 * (FRoadBuildEdModeCommands::Build/CancelGesture, BuildActions.cpp's edit.build) - there is no
 * defect behind them to close. Cancel also has no ONE key to register: PIE binds it straight to
 * right-click (ARoadBuildController::SetupInputComponent), never through BuildActions() at all,
 * because the viewport's own context menu had already claimed right-click in the editor
 * (FRoadBuildEdModeCommands::CancelGesture's own comment) - a single FKey field here cannot
 * represent two drivers with two different gestures for the same verb without inventing the
 * exception list this table exists to avoid. Folding a working pair in to match a broken trio
 * would be churn with no bug behind it.
 *
 * GUIDELINES IS ALSO NOT HERE, and NOT because PIE lacks a keypress for it the way Build/Cancel
 * do - it has one: `bShowGuidelines` (ARoadBuildController.h, "Toggled by G"), flipped by
 * `OnToggleGuidelines()`, bound to G as BuildActions.cpp's `aircraft.guidelines`. The reason it
 * cannot join THIS table is where that flag LIVES: `bShowGuidelines` is a
 * `UPROPERTY(EditAnywhere)` field on `ARoadBuildController`, a class in the AirportMgr GAME
 * module - not on FBuildSession, which lives in Airside, the plugin `BuildVerbRegistry()` itself
 * lives in specifically so both drivers can read it without either depending on the other's
 * module (Airside must never depend on AirportMgr). An `Apply(FBuildSession&, ...)` entry has
 * nothing to flip for this verb: the flag it would need to reach is not reachable from here,
 * unlike Remove/Insert/Edit's EGestureMode, which the mode was moved ONTO the session for
 * exactly this reason (see EGestureMode's own header comment).
 *
 * ALSO UNLIKE THE OTHER THREE, the editor's own guideline overlay
 * (`RoadBuildEditorTool.cpp`'s `DrawPersistentState`) already has a deliberate, standing ruling
 * against a toggle - "a visibility change is not the place to take that on" - drawn ALWAYS ON.
 * So even a reachable flag would have nothing in the editor to drive; reaching it would mean
 * EITHER moving `bShowGuidelines` onto FBuildSession the way EGestureMode was moved (a real
 * option, but a separate change this issue's evidence never established a defect for and which
 * would also mean reopening DrawPersistentState's own ruling) OR generalising this table's
 * `Apply` beyond FBuildSession, which would blur the one property (both drivers, no cross-module
 * dependency) that makes it safe to share at all.
 */
struct FBuildVerbRegistration
{
	FKey Key;

	/** STABLE, unlocalised - see FToolRegistration::Id for why this is the one field nothing
	 *  localises: FUICommandInfo persists it to the editor's per-user keybindings ini. */
	FName Id;

	FText Name;
	FText Tooltip;

	/**
	 * What pressing this verb does to the shared session, given the caller's own context - each
	 * driver assembles an FToolContext its own way (a live mouse ray in PIE, a cached hover in
	 * the editor), so a context is the one thing this table cannot build for a caller.
	 */
	TFunction<void(FBuildSession&, const FToolContext&)> Apply;

	/** Lit on the bar, checked in the editor's palette: whether this verb is the session's
	 *  current state right now. Needs only the session - GetGestureMode() answers all three. */
	TFunction<bool(const FBuildSession&)> IsActive;

	/** Greyed on the bar when false. Also needs only the session, the same way
	 *  ARoadBuildController::ActiveToolHasEditHandles already reads nothing else. */
	TFunction<bool(const FBuildSession&)> IsEnabled;
};

AIRSIDE_API TConstArrayView<FBuildVerbRegistration> BuildVerbRegistry();

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

	/** Every letter's fleet envelope, copied straight onto FToolContext::Envelopes - see that
	 *  field's own comment. URoadEditFacade::MakeTunables resolves it (#292 review finding),
	 *  the same chokepoint Limits and Snap already go through. */
	FLetterEnvelopeTable Envelopes;

	/**
	 * How close, in uu, the cursor counts as "on" something a tool is asking about.
	 *
	 * SEPARATE from Snap's own radii, which decide where a ROAD NODE lands - see
	 * ARoadBuildController::ToolPickRadius, whose comment this one keeps.
	 */
	double ToolPickRadius = 400.0;

	/**
	 * Field by field, like FBuildSession's own FFrameContextKey it exists for (issue #303):
	 * GetFrameContext has to tell "the same tunables as last call" from "different", and every
	 * field here feeds MakeContext's output directly - Snap sizes the chain, GuideSources gates
	 * which columns the guide chain may propose, Limits is copied straight onto FToolContext -
	 * so all of them have to agree, not just whichever happened to vary the day this was
	 * written. No operator== on FRoadSnapSettings/FSnapGuideSettings/FRoadPlacementLimits
	 * themselves to reuse: they are plain USTRUCTs with no reflection-based equality, and a
	 * byte compare would read whatever padding a copy left behind.
	 */
	bool operator==(const FBuildSessionTunables& Other) const
	{
		return Snap.NodeRadius == Other.Snap.NodeRadius
			&& Snap.SegmentRadius == Other.Snap.SegmentRadius
			&& Snap.bSnapToSegments == Other.Snap.bSnapToSegments
			&& Snap.MinSplitFromEndpoint == Other.Snap.MinSplitFromEndpoint
			&& Snap.JunctionSnapFactor == Other.Snap.JunctionSnapFactor
			&& GuideSources.bExtending == Other.GuideSources.bExtending
			&& GuideSources.bLevelWith == Other.GuideSources.bLevelWith
			&& GuideSources.bParallel == Other.GuideSources.bParallel
			&& GuideSources.bCollinear == Other.GuideSources.bCollinear
			&& GuideSources.bAngledFrom == Other.GuideSources.bAngledFrom
			&& GuideSources.bMatchingGap == Other.GuideSources.bMatchingGap
			&& GuideSources.bTaxiway == Other.GuideSources.bTaxiway
			&& GuideSources.bServiceRoad == Other.GuideSources.bServiceRoad
			&& GuideSources.bRunway == Other.GuideSources.bRunway
			&& GuideSources.bApron == Other.GuideSources.bApron
			&& GuideSources.bStand == Other.GuideSources.bStand
			&& GuideSources.bWorld == Other.GuideSources.bWorld
			&& Limits.MinSegmentLength == Other.Limits.MinSegmentLength
			&& Limits.MinTurnDegrees == Other.Limits.MinTurnDegrees
			&& Limits.NewRoadHalfWidth == Other.Limits.NewRoadHalfWidth
			&& ToolPickRadius == Other.ToolPickRadius
			&& Envelopes == Other.Envelopes;
	}
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

	/**
	 * Whether the committed graph's node rings belong on screen right now.
	 *
	 * ON THE SESSION so both drivers agree - the HUD and the editor viewport drew this from
	 * their own unconditional flags before, which is how the editor came to show rings the
	 * runtime had learned to hide. See FToolRegistration::bShowsRoadNodes.
	 *
	 * EDIT ALWAYS WANTS THEM, whatever is lit: the mode is ABOUT the points, and the tool
	 * only says which of them are grabbable.
	 */
	bool WantsRoadNodesDrawn() const
	{
		if (Mode == EGestureMode::Edit)
		{
			return true;
		}
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		return Registry.IsValidIndex(ActiveTool) && Registry[ActiveTool].bShowsRoadNodes;
	}

	/** The one sticky mode. A session opens in Build: a mode that survived construction
	 *  would be a gesture the player never asked to be able to make. */
	EGestureMode GetGestureMode() const { return Mode; }

	/**
	 * Enter InMode, deactivating the outgoing tool first so nothing is left part-drawn to
	 * reappear - the same argument SelectTool makes for the same call.
	 *
	 * DeactivateContext is the CALLER's, for the reason SelectTool's own comment gives: the
	 * session holds no IRoadEditTarget and has nothing to build one from.
	 */
	void SetGestureMode(EGestureMode InMode, const FToolContext& DeactivateContext = FToolContext());

	/**
	 * Light InMode, or go back to Build if it was already lit - what every one of the three
	 * mode buttons on the bar does.
	 *
	 * ONE TOGGLE FOR ALL THREE, which is the point: mutual exclusion is not a rule anyone
	 * has to apply here, it is what assigning to a single field already means.
	 */
	void ToggleGestureMode(EGestureMode InMode, const FToolContext& DeactivateContext = FToolContext())
	{
		SetGestureMode(Mode == InMode ? EGestureMode::Build : InMode, DeactivateContext);
	}

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
	bool ResolveSnap(const URoadNetwork* Network, const FRoadSnapQuery& Query,
		const FRoadSnapSettings& Snap, FRoadSnapResult& Out) const;

	/**
	 * No exclusion - what a CLICK always means, and what both drivers call directly.
	 *
	 * Kept at its old signature so `ARoadBuildController::ResolveSnap` needed no change:
	 * CLAUDE.md's rule that every reachable entry point stays reachable at its old name.
	 */
	bool ResolveSnap(const URoadNetwork* Network, const FVector2D& PlaneHit,
		const FRoadSnapSettings& Snap, FRoadSnapResult& Out) const
	{
		FRoadSnapQuery Query;
		Query.Cursor = PlaneHit;
		return ResolveSnap(Network, Query, Snap, Out);
	}

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
	 * What MakeContext would return for these exact inputs, rebuilt only when they differ from
	 * the last call THROUGH THIS FUNCTION - issue #303.
	 *
	 * BEFORE THIS, PIE alone had a "build once" convention: ARoadBuildController::PlayerTick
	 * called MakeContext (through its own MakeToolContext) exactly once and shared the answer
	 * with CollectToolReadout, Tick and the HUD (issue #167). The editor mode has no equivalent
	 * "top of frame" hook - OnUpdateHover, Render and DrawHUD are three separate ITF callbacks
	 * with no shared moment - so URoadBuildEditorTool::MakeContextAt ran the whole snap + guide
	 * pipeline (a junction solve per live node outside its cheap-reject radius, then the guide
	 * chain over every source) up to three times for one cursor position every frame. Keying
	 * the cache on the ACTUAL inputs, rather than on which function called it, lets both drivers
	 * share the one cache: two calls with the same key are the same question, whichever driver
	 * or entry point asked it.
	 *
	 * A KEY MISS SIMPLY REBUILDS THROUGH MakeContext, so a moved cursor, a changed tunable, a
	 * different modifier or a different active tool is never served a stale answer - only an
	 * UNMOVED one gets cheaper. What the key CANNOT see is a network mutated with none of those
	 * changing (a click landing exactly on the cached cursor position) - see
	 * InvalidateFrameContextCache for how that gap is closed, the same way issue #190's
	 * FToolReadoutKey needed one.
	 */
	const FToolContext& GetFrameContext(IRoadEditTarget* Target, const FVector2D& PlaneHit,
		const FBuildSessionTunables& Tunables, bool bRemoveModifier, bool bInsertModifier,
		bool bSuspendGuides = false, int32 HoverAgent = 0) const;

	/**
	 * Forces the next GetFrameContext to rebuild regardless of its key.
	 *
	 * EVERYTHING THAT CAN CHANGE WHAT A CONTEXT MEANS WITHOUT MOVING ITS INPUTS has to call
	 * this - the same list ARoadBuildController::InvalidateToolReadoutCache already names for
	 * issue #190's cache, extended here to a second one it now also clears: a click, a drag
	 * step, a commit, a cancel, an undo or redo, a network cleared out from under the tool. A
	 * tool or mode SWITCH is usually caught already - GetFrameContext's key includes
	 * GetActiveTool() - except reselecting the tool already lit, which changes no pointer and
	 * still drops a sticky Remove/Insert modifier; PIE's own SelectTool comment names that case.
	 *
	 * NINE HAND-PLACED CALLS ON THE PIE SIDE, one per site with no test that a tenth is not
	 * missing - issue #303 leaves this exactly as risky as issue #190 already made it. Folding
	 * every controller call through one invalidating wrapper (WithTool in the issue's own
	 * wording) would make the list structural instead of hand-maintained; that is a separate
	 * seam with its own test, deliberately not part of this change.
	 */
	void InvalidateFrameContextCache() const { bHasFrameContextCache = false; }

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

	/**
	 * How many times MakeContext has actually run the snap + guide pipeline, for issue #167's
	 * composition test: PlayerTickForTest brackets a tick with this to prove PlayerTick built
	 * exactly one context and handed it to CollectToolReadout and Tick, rather than each
	 * calling MakeToolContext (and so this) on its own. Counts every call regardless of
	 * caller or driver, so a test reads the delta across the operation it is measuring rather
	 * than the raw total.
	 */
	int32 MakeContextCallCountForTest() const { return ContextBuildCountForTest; }

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

	/** See MakeContextCallCountForTest(). mutable for the same reason - MakeContext is const
	 *  and this counts real work it did, not a decision. */
	mutable int32 ContextBuildCountForTest = 0;

	/**
	 * Everything GetFrameContext's answer can depend on - issue #303, the frame-context twin of
	 * ARoadBuildController::FToolReadoutKey.
	 *
	 * TARGET AND TOOL BY POINTER, deliberately: MakeContext reads GetActiveTool() itself (the
	 * snap exclusion, EditHandles) rather than taking it as a parameter, so a tool or mode
	 * switch that leaves every OTHER argument the same still has to miss the cache. Target is
	 * an IRoadEditTarget*, not the URoadNetwork* underneath it, for the same reason MakeContext
	 * itself takes one - a swapped target is a swapped network even when both currently hold
	 * the same pointer value's contents.
	 */
	struct FFrameContextKey
	{
		const IRoadEditTarget* Target = nullptr;
		const IBuildTool* Tool = nullptr;
		FVector2D PlaneHit = FVector2D::ZeroVector;
		FBuildSessionTunables Tunables;
		bool bRemoveModifier = false;
		bool bInsertModifier = false;
		bool bSuspendGuides = false;
		int32 HoverAgent = 0;

		bool operator==(const FFrameContextKey& Other) const
		{
			return Target == Other.Target
				&& Tool == Other.Tool
				&& PlaneHit == Other.PlaneHit
				&& Tunables == Other.Tunables
				&& bRemoveModifier == Other.bRemoveModifier
				&& bInsertModifier == Other.bInsertModifier
				&& bSuspendGuides == Other.bSuspendGuides
				&& HoverAgent == Other.HoverAgent;
		}
	};

	/** Last frame's key, compared in GetFrameContext. Undefined content when
	 *  bHasFrameContextCache is false - see that field. */
	mutable FFrameContextKey LastFrameContextKey;

	/** False before the first successful build and right after InvalidateFrameContextCache -
	 *  same reason as ARoadBuildController::bHasReadoutKey: a fresh session or a just-
	 *  invalidated one must rebuild rather than compare against a LastFrameContextKey that
	 *  happens to read as a match by construction (every FVector2D defaults to zero). */
	mutable bool bHasFrameContextCache = false;

	/** See GetFrameContext(). mutable for the reason ContextBuildCountForTest is - MakeContext
	 *  is const and this remembers real work it did, not a decision. */
	mutable FToolContext CachedFrameContext;
};
