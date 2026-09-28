#pragma once

#include "CoreMinimal.h"
#include "Solve/GridSnap.h"
#include "Solve/GuideArbiter.h"
#include "Tool/SnapGuideSettings.h"

/**
 * What the grid followed this frame - the log line's "from <source>", and what a test asserts.
 * Declaration order is the precedence Resolve applies, World aside.
 */
enum class EGridFrameSource : uint8
{
	/** EGridOrientation::World, or no grid at all. */
	World,

	/** A guide winner that lies along a thing - see GridFrameSource::LineOfWinner. */
	Winner,

	/** The line the active tool says it attaches to - IBuildTool::DescribeGridLine. */
	ToolLine,

	/** FGuideAnchor::Reference through its Origin: the edge a gesture is extending. */
	Anchor,

	/** The segment the road snap landed on. */
	RoadSnap,

	/** Nothing to follow: the last frame followed. */
	Held
};

/**
 * Everything the grid could follow this frame, gathered by FBuildSession::MakeContext.
 *
 * PLAIN VALUES, NO NETWORK, so the precedence is testable with hand-built candidates and no
 * world - the session does the looking up, this does the deciding.
 */
struct FGridFrameInputs
{
	EGridOrientation Orientation = EGridOrientation::Follow;
	double StepUu = 0.0;

	/** The chain's result, or null when the chain did not run. */
	const SnapGuide::FResult* Guide = nullptr;

	bool bToolLine = false;
	FVector2D ToolThrough = FVector2D::ZeroVector;
	FVector2D ToolDirection = FVector2D::ZeroVector;

	bool bAnchor = false;
	FVector2D AnchorOrigin = FVector2D::ZeroVector;
	FVector2D AnchorReference = FVector2D::ZeroVector;

	bool bRoadSnap = false;
	FVector2D RoadA = FVector2D::ZeroVector;
	FVector2D RoadB = FVector2D::ZeroVector;
};

/**
 * Which grid frame applies - grid-follows-snap design section 2.
 *
 * FIRST MATCH WINS, most specific first: a guide the point is already on names the thing the
 * player is lining up with; the tool's own line names what the gesture attaches to; the anchor's
 * reference names the edge it grew from; the road snap is only where the cursor happens to be.
 * Nothing: the held frame, so the grid stays put as the cursor leaves a road (ruling A).
 */
namespace GridFrameSource
{
	/**
	 * The THING's line a winner lies along, or false when the winner is not about a thing's
	 * direction.
	 *
	 * NOT ALWAYS THE CANDIDATE'S OWN LINE. A Parallel candidate runs through the drag's origin;
	 * the road it is parallel to runs through ReferenceAt (the point the dashed line is drawn to),
	 * and the design puts a grid line on the ROAD. "Square to" carries the perpendicular, turned
	 * back here - folding alone would give the right axis but a line across the road through
	 * ReferenceAt, not along it.
	 *
	 * REFUSED: the 45/135 diagonals ("45 degrees to the taxiway"), AngledFrom, Extending and
	 * LevelWith - a grid turned to one would be turned off the thing meant - and the ThisGesture
	 * and World references, which are not things on the map.
	 */
	AIRSIDE_API bool LineOfWinner(const SnapGuide::FCandidate& Winner, FVector2D& OutThrough, FVector2D& OutDirection);

	/**
	 * The frame for this frame. Out always written; Held updated only when Follow found something
	 * (origin and axis - the step always comes from In, so a step change keeps the direction).
	 * World leaves Held alone, so switching back to Follow resumes where it was.
	 */
	AIRSIDE_API EGridFrameSource Resolve(const FGridFrameInputs& In, GridSnap::FGridFrame& InOutHeld,
		GridSnap::FGridFrame& Out);

	/** "winner", "tool line", ... for the log. */
	AIRSIDE_API const TCHAR* Name(EGridFrameSource Source);
}
