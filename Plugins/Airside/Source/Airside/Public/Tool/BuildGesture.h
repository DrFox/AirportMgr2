#pragma once

#include "CoreMinimal.h"

/**
 * What FBuildGesture::Move just did to a held press.
 *
 * ONE ENUM: a press cannot be both "still a click" and "now a drag" on the same frame, so
 * there is no pair of bools to disagree about it (CLAUDE.md, "a phase is an enum, never a
 * set of bools").
 */
enum class EGestureStep : uint8
{
	/** Not past the threshold yet - still might be a click. */
	None,
	/** Crossed the threshold this call. The caller opens whatever "a drag began" means to it. */
	DragBegan,
	/** Already dragging; keep feeding the drag. */
	Dragging
};

/** What FBuildGesture::Release just resolved a press into. */
enum class EGestureEnd : uint8
{
	/** Never travelled past the threshold: a plain click. */
	Click,
	/** Was dragging: the drag ends here. */
	DragEnd,
	/** Released without ever having been pressed. Nothing to do. */
	Nothing
};

/**
 * The press/drag/release recogniser both build drivers ran a private copy of - see issue
 * #92. `Source/AirportMgr/RoadBuildController.cpp` and
 * `Plugins/Airside/Source/AirsideEditor/Private/RoadBuildEditorTool.cpp` each carried their
 * own bDown/bDragging/PressScreen and their own "moved past the threshold" arithmetic,
 * `Tool/BuildSession.h`'s own comment already noted this drift happening once before.
 *
 * World-free on purpose: it knows nothing about a mouse, a ray, or a tool, only about
 * SCREEN positions and a threshold - the same shape both drivers already reduced their
 * input to before touching it. Each driver keeps only the host I/O: reading the mouse,
 * opening a transaction, calling into IBuildTool.
 */
class AIRSIDE_API FBuildGesture
{
public:
	/** Left button down: remember where. Decides nothing - that waits for Move or Release. */
	void Press(const FVector2D& Screen);

	/**
	 * The press has moved to Screen. Promotes a held press to a drag once it has travelled
	 * past Threshold pixels; returns None while a press is not yet a drag (including when
	 * nothing is pressed at all), DragBegan the one call that crosses the line, and Dragging
	 * on every call after that.
	 */
	EGestureStep Move(const FVector2D& Screen, double Threshold);

	/**
	 * Button up. Click when the press never became a drag, DragEnd when it had, Nothing when
	 * there was no press to release (a stray mouse-up, or a second Release call).
	 */
	EGestureEnd Release();

	/** Escape, or a terminated drag sequence: drop the gesture without resolving it either way. */
	void Cancel();

	/** True from DragBegan until Release or Cancel. For a caller that needs to ask mid-gesture
	 *  rather than act only on Move's return value - see OnTerminateDragSequence. */
	bool IsDragging() const { return bDragging; }

	/** True from Press until Release or Cancel. Lets a caller skip reading the mouse at all
	 *  on a tick where nothing is pressed, rather than feeding Move a position it will
	 *  discard - both drivers polled this every PlayerTick/OnUpdateHover. */
	bool IsPressed() const { return bDown; }

private:
	bool bDown = false;
	bool bDragging = false;
	FVector2D PressScreen = FVector2D::ZeroVector;
};
