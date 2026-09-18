#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"
#include "Tool/SnapGuideChain.h"
#include "Tool/ToolReadout.h"
#include "Tool/Selection.h"
#include "RoadBuildTool.generated.h"

/**
 * Everything a tool needs to decide what an input means.
 *
 * Target is IRoadEditTarget, not the concrete ARoadNetworkActor - see that header's
 * comment. Present/ already includes Tool/ headers for snap results, placement limits and
 * deletion plans, so a tool header including Present/ back for the actor's own type would
 * close a real cycle; going through the interface instead means Tool/*.cpp never includes
 * Present/RoadNetworkActor.h at all. A caller that owns the concrete actor - the game
 * driver, the editor mode, the tool tests - passes it in through an implicit upcast, so
 * this is a widening of what Target may point to, not a behaviour change.
 *
 * Nothing here is a pointer the tool keeps. A context is built fresh each frame by whoever
 * owns the input, so a tool can hold no stale view of the world between events.
 */
struct FToolContext
{
	/** The facade every mutation goes through, so every mutation is undoable. */
	IRoadEditTarget* Target = nullptr;

	/**
	 * Where the cursor meets the road plane. THE RAW HIT - never the snapped position.
	 *
	 * The two are different answers to different questions and must not be folded into
	 * one. Snap says where a ROAD NODE would go; Cursor says where the mouse is. A tool
	 * that does not build roads - the route tool picking a guideline node, the stand tool
	 * placing a pose, the apron tool closing an outline - wants the mouse, and reading a
	 * road-snapped value silently applies road-building semantics to work that has none.
	 *
	 * This is not hypothetical. Guideline nodes sit on a segment's CUT LINE, which is
	 * CutDistance from the road node, while a junction's snap reach is
	 * CutDistance + HalfWidth - strictly further. So once the snap covered the junction,
	 * hovering a guideline node moved Cursor onto the ROAD node CutDistance away, and the
	 * route tool's pick - which uses SnapRadius, a smaller number - could never hit
	 * anything again. Every junction-adjacent guideline node became unhoverable at once.
	 */
	FVector2D Cursor = FVector2D::ZeroVector;

	/** What the snap chain made of that position. Carried BESIDE Cursor, never into it. */
	FRoadSnapResult Snap;

	/**
	 * What the cursor is lined up with - resolved by the DRIVER, exactly like Snap above and
	 * for the same recorded reason.
	 *
	 * Both drivers build their context through FBuildSession::MakeContext, so one gesture
	 * cannot guide differently in PIE and in the editor mode - the bug FRoadSnapSettings
	 * records having shipped before the two were merged.
	 *
	 * IT IS ALSO THE ONLY PLACE HYSTERESIS CAN LIVE. BuildPreview and BuildReadout are both
	 * const and neither may remember last frame's winner; the session holds it instead (see
	 * FBuildSession::LastGuide) and the flicker rule in the snap-guides design depends on it.
	 */
	SnapGuide::FResult Guide;

	/**
	 * Where a MOVING point should go: the guide's answer when there is one, the raw cursor
	 * when there is not.
	 *
	 * The ternary written once rather than at every consumer - the same argument Network()
	 * below makes for its own null check. A tool that read Guide.Point unconditionally would
	 * park its geometry at the origin on every frame no guide was active.
	 */
	FVector2D GuidedCursor() const { return Guide.bActive ? Guide.Point : Cursor; }

	FRoadPlacementLimits Limits;

	/**
	 * How close, in uu, counts as "on" something - the same radius the snap chain uses.
	 *
	 * Carried so a tool that has to decide whether the cursor is back on a point it placed
	 * itself - an apron closing on its first corner - measures it the same way everything
	 * else does, rather than inventing a second notion of near.
	 */
	double SnapRadius = 150.0;

	/**
	 * Target's network, null-safe. A Ctrl-remove preview always needs Target non-null AND
	 * its network non-null before it can look anything up - that pair used to be
	 * `Context.Target != nullptr && Context.Target->GetNetwork() != nullptr` at every one
	 * of those call sites (#103); this is the null check, once.
	 */
	const URoadNetwork* Network() const { return Target != nullptr ? Target->GetNetwork() : nullptr; }

	/** Ctrl: the gesture means remove rather than build. */
	bool bRemoveModifier = false;

	/** Shift: the gesture means insert without starting anything. */
	bool bInsertModifier = false;

	/**
	 * The agent under the cursor IN SCREEN SPACE, or 0. Filled by the driver, which owns the
	 * camera: an aircraft on final is 2000 uu up and a road-plane cursor lands on the grass
	 * beneath it, so the plane hit can never say "that aeroplane". The tool takes the id and
	 * never sees a projection - which is what keeps FSelectTool in the plugin.
	 */
	int32 HoverAgent = 0;

	/**
	 * Where a selection is recorded. Points at FBuildSession::Selection; null in a driver or
	 * test that has no session, in which case the Select tool selects nothing and says so.
	 * A pointer rather than a copy because the tool WRITES it, and the panel reads the
	 * session's copy, so there must be exactly one.
	 */
	FSelection* Selection = nullptr;

	/**
	 * Fill the cursor and the snap together, from the raw plane hit.
	 *
	 * Exists so the two cannot be conflated by a driver writing the assignments itself.
	 * They were: one driver set Cursor to the raw hit and the other to Snap.Position, the
	 * two behaved differently, and only one of them was right. A single function both call
	 * is what stops that being a per-driver choice at all.
	 */
	void SetCursor(const FVector2D& PlaneHit, const FRoadSnapResult& InSnap)
	{
		Cursor = PlaneHit;
		Snap = InSnap;
	}
};

/**
 * How a piece of preview should read. NOT a colour.
 *
 * The tools live in the plugin and the palette belongs to presentation, so a tool says
 * what a thing MEANS and the overlay decides what that looks like. It is also what lets
 * the sink be implemented by something other than a HUD - a test counting markers, say.
 */
UENUM()
enum class EPreviewStyle : uint8
{
	/** Ordinary in-progress geometry. */
	Pending,

	/** Something the gesture would attach to. */
	Snap,

	/** Something the gesture would destroy. */
	Doomed,

	/** Something the gesture would create in place of what it destroys. */
	Heal,

	/** Something the gesture cannot do, with the reason. */
	Refused,

	/**
	 * The guideline graph itself: context, not intent.
	 *
	 * Every other style describes something the CURRENT GESTURE would do. This one is the
	 * world the gesture happens in, and it reads as background - because a route drawn
	 * over an invisible graph tells you a path exists but never why it went that way.
	 */
	Guideline,

	/** A route that was found, and would be driven. */
	Route,

	/** A runway-holding position: the bar across a taxiway end at a runway. Context, like Guideline; derived since 2026-09-07. */
	RunwayHoldingPosition,

	/** An intermediate holding position: the player's single dashed line at a taxiway junction. */
	IntermediateHoldingPosition,

	/** What a click would select right now: the pickable under the cursor. */
	Hover,

	/** What IS selected. Drawn every frame the selection stands, so it can be found again. */
	Selected,

	// --- GraphOverlay's own context styles ---------------------------------------------
	//
	// The committed road graph and its placed entities, like Guideline above: true whatever
	// the gesture, not something the current one would do. See GraphOverlay.h.

	/** A road node with no incident segments. It draws no pavement, so it needs its own mark. */
	NodeStub,

	/** One or two incident segments: a dead end, or a straight-through node. */
	NodeThrough,

	/** Three or more incident segments - a real junction, with a solved boundary. */
	NodeJunction,

	/**
	 * A placed entity's own committed pose - the thing it IS, not Pending's "a gesture would
	 * put one here". StandPreview::Describe marks the same position again as Pending,
	 * because that call is shared with an in-progress placement; GraphOverlay draws THIS
	 * marker afterwards and at a different radius (see ARoadBuildHUD::Marker's StandPose
	 * case) so the two remain distinguishable on screen instead of one ring simply
	 * overdrawing the other.
	 */
	StandPose,

	/** A resolved anchor: the guideline node a vehicle will actually route to on this stand. */
	ServiceAnchor,

	// --- The staged plot gesture's own two -----------------------------------------------
	//
	// PINNED AND PROVISIONAL SAY WHAT Pending CANNOT: whether the thing under them has
	// stopped moving. Pending means "this is what the click would do", which is true of an
	// edge being dragged AND of one already placed - and the player counts remaining corners
	// by exactly that difference.
	//
	// ADDED AT THE END rather than beside Pending where they would read better: this is a
	// UENUM, and renumbering it repoints any value already serialised against it.

	/** An edge the player has already placed. It will not move again this gesture. */
	Pinned,

	/** An edge that follows the cursor. Drawn dashed - see ARoadBuildHUD::IsDashed. */
	Provisional,

	/**
	 * What the cursor is lined up with: the dashed line to the thing it is squared to.
	 *
	 * NOT Provisional, which already means "this edge is still moving" - and the two are
	 * drawn in the same frame, touching the same corner. Two meanings, two styles; whether
	 * the overlay dashes both is its business, not the plugin's (snap-guides design section 6).
	 *
	 * AT THE END, like Pinned and Provisional above and for the same reason: this is a UENUM
	 * and renumbering it repoints any value already serialised against it.
	 */
	Guide,
};

/**
 * Where a tool draws its intent, in ROAD PLANE coordinates.
 *
 * Plane coordinates, never screen ones: projection is the overlay's job and depends on a
 * camera the plugin must know nothing about. Design spec 7.2's FPreviewSink.
 */
struct IToolPreviewSink
{
	virtual ~IToolPreviewSink() = default;

	virtual void Marker(const FVector2D& At, EPreviewStyle Style) = 0;
	virtual void Line(const FVector2D& From, const FVector2D& To, EPreviewStyle Style) = 0;

	/**
	 * A short mark ACROSS a direction - a cut line, a split point.
	 *
	 * The tool supplies where and along what; how long the mark is stays with the overlay,
	 * because a length that reads well is measured in pixels and the plugin has no camera
	 * to measure them against.
	 */
	virtual void CrossMark(const FVector2D& At, const FVector2D& Along, EPreviewStyle Style) = 0;
	virtual void Label(const FVector2D& At, const FString& Text, EPreviewStyle Style) = 0;

	/**
	 * Every consecutive pair as a Line, so a caller does not re-derive the "N-1 segments"
	 * loop - six call sites had (#103). NON-VIRTUAL: it is built entirely from Line above,
	 * so every sink implements it for free rather than each re-implementing the loop.
	 */
	void Polyline(TConstArrayView<FVector2D> Points, EPreviewStyle Style)
	{
		for (int32 Index = 1; Index < Points.Num(); ++Index)
		{
			Line(Points[Index - 1], Points[Index], Style);
		}
	}

	/** Polyline plus the closing edge back to the first point. */
	void Polygon(TConstArrayView<FVector2D> Points, EPreviewStyle Style)
	{
		Polyline(Points, Style);
		if (Points.Num() >= 2)
		{
			Line(Points.Last(), Points[0], Style);
		}
	}
};

/**
 * One selectable tool - Strategy, not State.
 *
 * Tools do NOT transition into one another: the player presses a number and picks one, so
 * there is no transition graph to model and a state machine over the SET of tools would be
 * describing something that does not exist. What each tool does INTERNALLY is a state
 * machine, and that lives inside the tool - see FRoadDrawTool.
 *
 * The owner interprets the mouse - what counts as a click rather than a drag, and how far a
 * press must travel - and calls the matching method. A tool decides what a gesture MEANS,
 * never what the raw input was.
 *
 * BuildPreview is const, per design spec 7.2, and that const is the point: a tool
 * physically cannot mutate the network while drawing what it would do. Preview and
 * mutation are separated by the type system rather than by discipline.
 */
struct AIRSIDE_API IBuildTool
{
	virtual ~IBuildTool() = default;

	/** Shown in the overlay so the active mode is never a thing you have to remember. */
	virtual FText GetDisplayName() const = 0;

	/** A press and release that never travelled. */
	virtual void OnClick(const FToolContext& Context) = 0;

	/** Right click: step back out of whatever is part-drawn. */
	virtual void OnCancel(const FToolContext& Context) = 0;

	virtual void OnDragBegin(const FToolContext& Context) {}
	virtual void OnDrag(const FToolContext& Context) {}
	virtual void OnDragEnd(const FToolContext& Context) {}

	/** Every frame, for previews that cost real work to build. */
	virtual void Tick(const FToolContext& Context) {}

	/**
	 * Leaving this tool for another. Part-drawn work is abandoned here rather than left
	 * to reappear when the tool is selected again, which would be a click landing on a
	 * chain the player started minutes ago and has forgotten.
	 */
	virtual void OnDeactivate(const FToolContext& Context) {}

	/**
	 * The active tool's OWN key pressed again, or its bar button clicked while lit.
	 *
	 * A tool with a choice to cycle does it here - the runway tool's width, surface and
	 * approach - and reads Context's modifiers to tell which. This is the seam that was
	 * missing: FRunwayTool::NextWidth existed through two milestones with NO CALLER,
	 * because pressing 6 while the runway tool was active fell into SelectTool's "already
	 * active, do nothing" branch and nothing else had a place to call it from. A default
	 * that does nothing keeps every other tool as it was.
	 */
	virtual void OnReselect(const FToolContext& Context) {}

	/**
	 * What this tool is dragging, and against what, for the guide chain. False means "no
	 * gesture is in progress", and the driver then resolves no guide at all.
	 *
	 * CONST AND CONTEXT-FREE, read off what the tool has already pinned. It cannot take an
	 * FToolContext because the driver calls it WHILE BUILDING ONE - and it should not want
	 * to: where the cursor is now is the chain's input, not the anchor's.
	 *
	 * RETURNS BOOL rather than setting a flag inside FGuideAnchor, so a caller branches on
	 * the return - CLAUDE.md's rule about honouring anything that fills an out-parameter.
	 *
	 * Silent by default, like BuildReadout below: eight tools implement this interface and
	 * stage 1 of the snap-guides design gives an anchor to exactly one of them.
	 */
	virtual bool DescribeGuideAnchor(FGuideAnchor& Out) const { return false; }

	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const = 0;

	/**
	 * What to tell the player about this gesture - bays, cost, what is wrong. Silent by
	 * default.
	 *
	 * A SEPARATE VIRTUAL and not a second parameter on BuildPreview, which has eight
	 * implementors and two test doubles, NONE of which has a readout. Changing that signature
	 * would churn ten classes to add a parameter they ignore.
	 *
	 * The argument for emitting through a per-frame const call survives the split: both are
	 * const, both are called from the same place on the same frame, so a fact still cannot
	 * describe a different gesture from the geometry drawn beside it. Airside.Tool.
	 * ToolsAreSilentByDefault pins the default; the plot tool's own test pins the agreement.
	 */
	virtual void BuildReadout(const FToolContext& Context, IToolReadoutSink& Sink) const {}

	/**
	 * The player pressed Build. Does nothing by default, so no existing tool changes.
	 *
	 * SEPARATE FROM OnClick because a staged gesture's last click LOCKS rather than commits -
	 * the review beat between the two is where cost and warnings actually get read. It is
	 * also reachable at ANY moment, the Build button being a widget rather than a stage, so
	 * a tool that implements it must ignore it in every state but the last.
	 */
	virtual void OnCommit(const FToolContext& Context) {}

	/** True when nothing is part-drawn, so the owner can tell whether cancel means anything. */
	virtual bool IsIdle() const = 0;
};
