#include "Build/TurnShape.h"

#include "Build/RoadNetworkSolver.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/JunctionSolver.h"
#include "Solve/RoadGeom.h"

FTurnGeometry ClassifyTurn(const URoadNetwork& Network, const FRoadSolveResult& Solved,
	const FJunctionResult& Junction, int32 NodeIndex, FRoadNodeId NodeId,
	const TArray<FRoadSegmentId>& ArmSegments, FRoadSegmentId FromSeg, FRoadSegmentId ToSeg,
	const URoadProfile* FromProfile, const URoadProfile* ToProfile,
	ETraversalClass FromClass, ETraversalClass ToClass, FGuidelineEdge& Turn)
{
	// A BEND'S LANES FOLLOW ITS PAVEMENT (user ruling 2026-09-25). At a two-arm bend the
	// quadratic above runs cut to cut with its control on the lane lines' crossing -
	// one quadratic across the corner's whole sweep, which delivers only cos(sweep/2) of
	// the arc it approximates (0.707 at a right angle: 726 uu on a Narrow inner lane
	// whose arc is 1026) and bulges off it by 62 uu at the apex. The cut is the inner
	// fillet's tangent point (or, where the solver widened the inside, further out along
	// the arm), so each lane line touches a circle about that fillet's centre at the
	// lane's own distance from the inner edge - the lane concentric with the pavement's
	// inside. So the turn is that arc, laid in pieces (GuidelineGeom::BendLane), each
	// sampled once like any edge.
	//
	// CONCENTRIC WITH THE INNER EDGE, NOT THE OUTER, and not by preference: the two
	// fillets have the SAME radius, so their centres sit a road width apart on each axis
	// and no circle is concentric with both. About the outer fillet's centre the lanes
	// would use the outside of the bend - and turn at 306 / 606 uu on Narrow, 186 / 636
	// on Wide, below both design vehicles' locks, where they turn at 726 / 938 and
	// 853 / 1171 today (measured 2026-09-25). The ruling forbids a tighter turn.
	// SINCE 2026-09-25 THE OUTER EDGE FOLLOWS: the solver lays the outside as the arc about
	// this same centre at the inner radius plus the road width (SmoothBend,
	// RoadNetworkSolver.cpp), so both edges and every lane now share the one centre.
	//
	// ONLY WHERE THE CONSTRUCTION HOLDS: two arms, a service-road lane on both (a
	// taxiway's corner is authored for aircraft - PreferredFilletRadius - and stays as it
	// was), and both lane ends on the same circle and at its tangent points. Arms of two
	// widths put their lanes at different distances from the inner edge, so no one
	// circle touches both: those RAMP onto the wider arm's circle (below). 33d6f49b kept
	// the one quadratic there - "only one node around the corner", the user's variant (a).
	// ENFORCED BY: Airside.Build.BendLanes.ConcentricWithPavement, .NeverTighter, .EveryTierIsSmooth
	TArray<GuidelineGeom::FArcPiece> BendArc;
	if (ArmSegments.Num() == 2 && Junction.Corners.Num() == 2
		&& FromClass == ETraversalClass::GroundVehicle && ToClass == ETraversalClass::GroundVehicle)
	{
		const int32 InsideIndex = Junction.InnerCornerOfBend();
		const RoadGeom::FFillet* Inside = InsideIndex != INDEX_NONE ? &Junction.Corners[InsideIndex] : nullptr;
		if (Inside != nullptr)
		{
			const FVector2D PA = Network.GetGuidelineNode(Turn.A)->Position;
			const FVector2D PB = Network.GetGuidelineNode(Turn.B)->Position;
			const FVector2D ArriveDir = -Network.GetOutgoingTangent(FromSeg, NodeId).GetSafeNormal();
			const FVector2D LeaveDir = Network.GetOutgoingTangent(ToSeg, NodeId).GetSafeNormal();
			// GuidelineGeom::BendLane refuses what the construction cannot hold: lanes at
			// two distances from the edge, a tangent point behind a lane end, a turn not
			// round this corner. A widened bend's lane ends sit back from their tangent
			// points (the solver cut the arms back to hold the widening) and are joined
			// to the arc by straight leads.
			GuidelineGeom::BendLane(PA, ArriveDir, PB, LeaveDir, Inside->Centre, BendArc);
			// A WIDTH STEP RUNS AT THE WIDER WIDTH (2026-09-25): the narrower arm's lane end is
			// off the circle the wider arm's touches, so it ramps onto it along the bend's own
			// ramp clock - the one the solver laid both edges on (FBendOuter::Ramp), so the lanes
			// stay evenly spaced between them through the taper (GuidelineGeom::RampedBendLane).
			const double FromWidth = FromProfile->GetTotalWidth();
			const double ToWidth = ToProfile->GetTotalWidth();
			const FBendOuter* Bend = Solved.BendOuters.FindByPredicate(
				[NodeIndex](const FBendOuter& Candidate) { return Candidate.NodeIndex == NodeIndex && Candidate.bApplied; });
			const int32 FromArm = ArmSegments.IndexOfByKey(FromSeg);
			const int32 ToArm = ArmSegments.IndexOfByKey(ToSeg);
			if (BendArc.Num() == 0 && FromWidth != ToWidth && Bend != nullptr && FromArm != INDEX_NONE && ToArm != INDEX_NONE)
			{
				// The lanes' circle: the wider arm's lane line's distance from the centre.
				const FVector2D Wide = FromWidth > ToWidth ? PA : PB;
				const FVector2D WideDir = FromWidth > ToWidth ? ArriveDir : LeaveDir;
				GuidelineGeom::FRampedBend Ramped;
				Ramped.Centre = Inside->Centre;
				Ramped.Radius = FMath::Abs(FVector2D::CrossProduct(Inside->Centre - Wide, WideDir));
				Ramped.RefRadius = Bend->RefRadius;
				Ramped.LIn = Bend->Ramp[FromArm];
				Ramped.LOut = Bend->Ramp[ToArm];
				GuidelineGeom::RampedBendLane(PA, ArriveDir, PB, LeaveDir, Ramped, BendArc);
			}
		}
	}

	// A WIDTH TAPER IS DRIVEN ON AN S (user ruling 2026-09-25). At a straight-through
	// node of two arms whose lanes sit at different offsets, the solver has inset
	// both cuts (FRoadNetworkSolver's WidthTaperLength) and the lane ends face each
	// other across the taper, parallel and a lane-offset step apart. One quadratic
	// cannot join two parallel lines - the chord above was a straight diagonal with a
	// kink at each end, and with the cuts at the node it was a 90 degree jog. So the
	// turn is TWO edges meeting at the midpoint, GuidelineGeom's lane change: each a
	// symmetric quadratic of tangent s deflecting by b, with tan(b/2) = across/along
	// and 2 s sin b = across, so the pair runs exactly from end to end and is tangent
	// to both lanes and to each other. Each is sampled once by GuidelineGeom like any
	// edge, and measured like any turn - its MinRadius is what route search gates on.
	// Only at a TWO-arm node: a through road's width change inside a larger junction
	// keeps the chord, and is not this ruling's.
	// THE TRIGGER IS "THE LANES ARE OFFSET", NOT "THE WIDTHS DIFFER" - intended: what
	// the S removes is a lateral step in the LINE, whatever made it. So a same-width
	// profile change whose lanes sit at other offsets tapers too, and so does a
	// one-lane bidirectional road meeting a two-lane one (its centreline steps a
	// quarter-width onto each lane).
	// ENFORCED BY: Airside.Build.WidthTaper.SameWidthOffsetLanes, Airside.Build.WidthTaper.OneLaneMeetsTwo
	// ENFORCED BY: Airside.Build.WidthTaper.Drivable, Airside.Build.WidthTaper.LanesContinuous
	//
	// RUNS REGARDLESS OF THE BEND ABOVE, exactly as the two independent blocks this
	// function replaces did: the two constructions are geometrically exclusive (a bend
	// needs a real corner; a taper needs the arms straight through), so in practice at
	// most one ever produces geometry, and the shape decided below prefers BendArc.
	bool bTaperS = false;
	FVector2D TaperMid = FVector2D::ZeroVector;
	FVector2D TaperControlOut = FVector2D::ZeroVector;
	FVector2D TaperFrom = FVector2D::ZeroVector;
	FVector2D TaperTo = FVector2D::ZeroVector;
	FVector2D TaperTravel = FVector2D::ZeroVector;
	if (ArmSegments.Num() == 2
		&& RoadGeom::IsStraightThrough(RoadGeom::AngleBetween(
			Network.GetOutgoingTangent(FromSeg, NodeId), Network.GetOutgoingTangent(ToSeg, NodeId))))
	{
		// GuidelineGeom's ONE lane-change construction, the same the solver sized
		// the inset with (LaneChangeLength). Along <= 1 uu (a taper capped to
		// nothing) leaves the chord - see the spec's Open list.
		const FVector2D Travel = -Network.GetOutgoingTangent(FromSeg, NodeId).GetSafeNormal();
		FVector2D ControlIn, Mid, ControlOut;
		if (GuidelineGeom::LaneChange(Network.GetGuidelineNode(Turn.A)->Position,
			Network.GetGuidelineNode(Turn.B)->Position, Travel, ControlIn, Mid, ControlOut))
		{
			TaperMid = Mid;
			TaperFrom = Network.GetGuidelineNode(Turn.A)->Position;
			TaperTo = Network.GetGuidelineNode(Turn.B)->Position;
			TaperTravel = Travel;
			Turn.Control = ControlIn;
			TaperControlOut = ControlOut;
			bTaperS = true;
		}
	}

	// THE SHAPE, DECIDED ONCE: a bend's arc wins if it produced any pieces, then a width
	// taper's S, and a chord is whatever is left when neither construction held. Same
	// priority the two inline flags used to be read at, independently, by every caller.
	FTurnGeometry Result;
	Result.BendArc = MoveTemp(BendArc);
	if (Result.BendArc.Num() > 0)
	{
		Result.Shape = ETurnShape::BendArc;
	}
	else if (bTaperS)
	{
		Result.Shape = ETurnShape::TaperS;
		Result.TaperMid = TaperMid;
		Result.TaperControlOut = TaperControlOut;
		Result.TaperFrom = TaperFrom;
		Result.TaperTo = TaperTo;
		Result.TaperTravel = TaperTravel;
	}
	else
	{
		Result.Shape = ETurnShape::Chord;
	}
	return Result;
}
