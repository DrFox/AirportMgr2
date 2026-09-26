#include "Solve/LinkGeom.h"

#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

bool LinkGeom::ChooseDeparture(TConstArrayView<FDepartureCandidate> Candidates,
	const FVector2D& Toward, FVector2D& OutAlong)
{
	bool bFound = false;
	bool bBestIsBend = false;
	double BestToward = 0.0;
	double BestRoom = 0.0;

	for (const FDepartureCandidate& Candidate : Candidates)
	{
		// THE ORDER OF THE THREE TESTS IS THE RULE ITSELF: toward the road first, then the
		// corner side, then room. The second and third only ever decide a tie in the first,
		// because a bend's two senses' dot products are exact opposites - see this function's
		// own header comment.
		const double Dot = FVector2D::DotProduct(Candidate.Along, Toward);
		const double Margin = Dot - BestToward;
		const bool bBetter = !bFound
			|| Margin > UE_DOUBLE_KINDA_SMALL_NUMBER
			|| (FMath::Abs(Margin) <= UE_DOUBLE_KINDA_SMALL_NUMBER
				&& (Candidate.bBend != bBestIsBend ? Candidate.bBend : Candidate.Room > BestRoom));
		if (bBetter)
		{
			OutAlong = Candidate.Along;
			BestRoom = Candidate.Room;
			BestToward = Dot;
			bBestIsBend = Candidate.bBend;
			bFound = true;
		}
	}
	return bFound;
}

LinkGeom::FLinkGeometry LinkGeom::Plan(const FLinkApproach& Approach)
{
	FLinkGeometry Out;

	// WHERE THE LEAD-IN RAY STRIKES IS THE CORNER, NOT THE JOIN - see FAnchorLink::Join's own
	// top comment for why the join has to MOVE off this point rather than land on it. Every
	// shape below starts from here and some move it again.
	Out.Param = Approach.Param;
	Out.Corner = GuidelineGeom::Eval(Approach.PositionA, Approach.Control, Approach.PositionB, Out.Param);
	Out.Dir = Approach.Dir;

	if (Approach.bRecomputeDirFromCorner)
	{
		// THE DIRECTION THE JOIN ACTUALLY NEEDS. A service link has no ray of its own - it was
		// found by DISTANCE - but the fillet and the two sweeps FAnchorLink::Join lays are all
		// written in terms of a direction of arrival, and this is it.
		const FVector2D Toward = Out.Corner - Approach.At;
		if (!Toward.IsNearlyZero())
		{
			Out.Dir = Toward.GetSafeNormal();
		}
	}

	// A LINE JOINS A LANE ALONG IT, NOT ACROSS IT.
	//
	// A SQUARE-ON ENTRANCE IS A TIGHT TURN WE BUILT. However well the router is taught to avoid
	// tight turns it can only choose among the ones that exist, and a lane whose entrances met
	// the road at a right angle left exactly one tight option on every stand - which is the one
	// it kept taking. Reported from play 2026-09-15, twice.
	//
	// TWO SHAPES, AND WHICH ONE IS A QUESTION ABOUT THE ROAD, not a preference. A road drawn
	// ALONGSIDE the stand is parallel to the lane and the two lines never meet, so the connector
	// is a lane change: an S of two curves, sized by GuidelineGeom::ShiftDeflectionFor. A road
	// drawn ACROSS the end of the stand CROSSES the lane's heading, and that wants the ordinary
	// thing - run on to where they meet and round the corner. The crossing is preferred wherever
	// it exists within this link's own reach and its corner fits in front of the entry, because
	// it leaves the lead-in dead straight and gives BOTH sweeps their radius rather than one: an
	// S has to slant onto the road, and turning the other way out of a slant is a U-turn that no
	// geometry fixes. Tried the other way round first, and a road across the nose came out with
	// a transition longer than the distance to the road and no link at all.
	if (Approach.bHasLaneAlong && Approach.LaneRadius > 0.0)
	{
		const FVector2D& Along = Approach.Along;

		// WHERE THE LANE'S OWN HEADING MEETS THE ROAD, if it does at all. Parallel lines give a
		// vanishing cross product and no crossing; a road BEHIND the entry gives a negative
		// distance, which is not a crossing this link can use either.
		const FVector2D RoadDir =
			GuidelineGeom::Tangent(Approach.PositionA, Approach.Control, Approach.PositionB, Out.Param);
		const double Converge = Along.X * RoadDir.Y - Along.Y * RoadDir.X;
		const FVector2D ToRoad = Out.Corner - Approach.At;
		const double MeetsAt = FMath::Abs(Converge) > UE_DOUBLE_KINDA_SMALL_NUMBER
			? (ToRoad.X * RoadDir.Y - ToRoad.Y * RoadDir.X) / Converge
			: -1.0;

		// AND WHETHER ITS CORNER FITS IN FRONT OF THE ENTRY. The turn onto the road is the
		// GENTLER of the two corners the connector makes with it - the one a truck joining the
		// traffic takes - and CornerRunFor says how much run that needs. The ACUTE angle
		// between the two lines, folded from AngleBetween's [0, pi] - rule 18.
		const double Crossing = RoadGeom::AngleBetween(Along, RoadDir);
		const double Turn = FMath::Min(Crossing, UE_DOUBLE_PI - Crossing);
		const double Needs = GuidelineGeom::CornerRunFor(Approach.LaneRadius, UE_DOUBLE_PI - Turn);

		// APPROACH.REACH IS DOING TWO JOBS HERE, and the second one is named so it is
		// deliberate. Its first is the player's knob for how far a stand may sit from its road.
		// Its second is this: a crossing further off than a link may reach is a crossing this
		// connector would have to run to along a line that is no longer the lane, so the lane
		// change is the better shape. Raising the knob therefore widens the crossing branch as
		// well as the search - which is right, since both answer "how far from its road may a
		// stand be", but it is a coupling to know about rather than to discover. There is no
		// second figure because a second figure is one more thing that can disagree with this
		// one.
		if (MeetsAt > 0.0 && MeetsAt <= Approach.Reach && Needs + Approach.WeldTolerance <= MeetsAt)
		{
			// THE CROSSING. Re-asked of the road rather than taken from the tangent line, so
			// the corner sits ON a road that bends; Dir then runs from the entry to it, and the
			// lead-in is straight - which is tangent to the lane at the entry by construction,
			// and needs no control point of its own to be so.
			Out.Shape = ELinkShape::Crossing;

			int32 Span = 0;
			double Fraction = 0.0;
			GuidelineGeom::NearestOnPolyline(Approach.Curve, Approach.At + Along * MeetsAt, Span, Fraction);
			Out.Param = GuidelineGeom::ParamAtSample(Span, Fraction, Approach.Curve.Num());
			Out.Corner =
				GuidelineGeom::Eval(Approach.PositionA, Approach.Control, Approach.PositionB, Out.Param);

			const FVector2D Leg = Out.Corner - Approach.At;
			if (!Leg.IsNearlyZero())
			{
				Out.Dir = Leg.GetSafeNormal();
			}
		}
		else
		{
			// THE LANE CHANGE. The entry cannot simply run on to the road - it is beside it, not
			// aimed at it - so the connector leaves at the deflection that clears the lock
			// across this gap and meets the road at the same angle on the other side.
			double Run = 0.0;
			const double Deflect = GuidelineGeom::ShiftDeflectionFor(Approach.LaneRadius, ToRoad.Size(), Run);
			if (Run > Approach.WeldTolerance)
			{
				Out.Shape = ELinkShape::LaneChange;

				// WHICH WAY IT BENDS is which side the road is on, and the cross product says
				// so: positive when the road lies to the left of the way this link leaves.
				const double Side = FMath::Sign(Along.X * ToRoad.Y - Along.Y * ToRoad.X);
				const FVector2D Turned = Along.GetRotated(FMath::RadiansToDegrees(Deflect) * Side);

				// AND THE ROAD IS MET WHERE THE TRANSITION ARRIVES, not where it is nearest.
				// Both curves get the same tangent length, so the meeting point sits two runs
				// along the aim - and re-asking the road for the point nearest THAT is what
				// keeps this honest when the road bends or ends: the fillet still works from a
				// point on the road, and Dir is taken from the control afterwards so the
				// lead-in stays tangent to whatever came back.
				const FVector2D Control = Approach.At + Along * Run;
				const FVector2D Aim = Control + Turned * (2.0 * Run);

				int32 Span = 0;
				double Fraction = 0.0;
				GuidelineGeom::NearestOnPolyline(Approach.Curve, Aim, Span, Fraction);
				Out.Param = GuidelineGeom::ParamAtSample(Span, Fraction, Approach.Curve.Num());
				Out.Corner = GuidelineGeom::Eval(
					Approach.PositionA, Approach.Control, Approach.PositionB, Out.Param);

				const FVector2D Leg = Out.Corner - Control;
				if (!Leg.IsNearlyZero())
				{
					Out.Control = Control;
					Out.Dir = Leg.GetSafeNormal();
					Out.LaneRun = Run;
				}
			}
			// ELSE: Run <= WeldTolerance - no usable lane change either, and Shape stays
			// StraightRay with Corner/Param/Dir exactly as the recompute-from-corner step above
			// left them, precisely as the ladder this replaced did when it fell all the way
			// through with nothing left to assign.
		}
	}

	return Out;
}
