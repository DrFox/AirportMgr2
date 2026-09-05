#pragma once

#include "CoreMinimal.h"
#include "Tool/RoadEditTarget.h"
#include "Tool/RoadPlacement.h"
#include "Tool/RoadSnap.h"

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

	FRoadPlacementLimits Limits;

	/**
	 * How close, in uu, counts as "on" something - the same radius the snap chain uses.
	 *
	 * Carried so a tool that has to decide whether the cursor is back on a point it placed
	 * itself - an apron closing on its first corner - measures it the same way everything
	 * else does, rather than inventing a second notion of near.
	 */
	double SnapRadius = 150.0;

	/** Ctrl: the gesture means remove rather than build. */
	bool bRemoveModifier = false;

	/** Shift: the gesture means insert without starting anything. */
	bool bInsertModifier = false;

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

	virtual void BuildPreview(const FToolContext& Context, IToolPreviewSink& Sink) const = 0;

	/** True when nothing is part-drawn, so the owner can tell whether cancel means anything. */
	virtual bool IsIdle() const = 0;
};
