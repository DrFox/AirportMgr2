#pragma once

#include "CoreMinimal.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadHandles.h"
#include "Model/RoadTraffic.h"
#include "Solve/GuidelineGeom.h"

class URoadNetwork;
class URoadProfile;
struct FJunctionResult;
struct FRoadSolveResult;

/**
 * The three constructions RoadGuidelineBuilder lays for a turn between two arms at a
 * junction (2026-09-25 rulings): a CHORD (the general quadratic to the lane lines'
 * crossing), a BEND'S ARC concentric with the pavement's inner edge, or a WIDTH TAPER'S S.
 * Decided ONCE by ClassifyTurn, replacing the `BendArc.Num() > 0` / `bTaperS` flag pair
 * that used to be re-derived independently at piece emission and again 90 lines later in
 * the lock warning - the two had to be read as one shape by eye each time (issue #305).
 */
enum class ETurnShape : uint8
{
	Chord,
	BendArc,
	TaperS,
};

/**
 * What ClassifyTurn decided, and the geometry that decision needed to construct. Only the
 * fields matching Shape are meaningful; the rest are left at their default.
 */
struct FTurnGeometry
{
	ETurnShape Shape = ETurnShape::Chord;

	/** Shape == BendArc: the bend's arc, piece by piece (GuidelineGeom::BendLane / RampedBendLane). */
	TArray<GuidelineGeom::FArcPiece> BendArc;

	/**
	 * Shape == TaperS: the S's own midpoint node and second control point, plus the chord
	 * data (From/To/Travel) the lock warning measures the delivered radius against.
	 */
	FVector2D TaperMid = FVector2D::ZeroVector;
	FVector2D TaperControlOut = FVector2D::ZeroVector;
	FVector2D TaperFrom = FVector2D::ZeroVector;
	FVector2D TaperTo = FVector2D::ZeroVector;
	FVector2D TaperTravel = FVector2D::ZeroVector;
};

/**
 * Decides the one shape a turn between FromSeg and ToSeg at NodeId is laid as, and builds
 * the geometry that shape needs.
 *
 * Turn.Control is READ for the chord case (set by the caller to the lane lines' crossing,
 * or the node) and OVERWRITTEN when Shape comes back TaperS, to GuidelineGeom::LaneChange's
 * own ControlIn - exactly what the two inline blocks this replaces did, in the same order:
 * a bend's arc is attempted first and wins if BendLane/RampedBendLane produced any pieces,
 * then a width taper's S, and Chord is whatever is left when neither construction held.
 * The two are geometrically exclusive (a bend needs a real corner; a taper needs the arms
 * straight through) but this preserves the ORDER regardless, because that is what the code
 * it replaces did.
 * ENFORCED BY: Airside.Build.ClassifyTurn.* (a bend, a width taper, and a plain T-junction
 * fixture, one test per shape)
 */
AIRSIDE_API FTurnGeometry ClassifyTurn(const URoadNetwork& Network, const FRoadSolveResult& Solved,
	const FJunctionResult& Junction, int32 NodeIndex, FRoadNodeId NodeId,
	const TArray<FRoadSegmentId>& ArmSegments, FRoadSegmentId FromSeg, FRoadSegmentId ToSeg,
	const URoadProfile* FromProfile, const URoadProfile* ToProfile,
	ETraversalClass FromClass, ETraversalClass ToClass, FGuidelineEdge& Turn);
