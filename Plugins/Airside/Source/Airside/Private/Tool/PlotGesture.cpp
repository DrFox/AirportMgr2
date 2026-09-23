#include "Tool/PlotGesture.h"

#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RoadGeom.h"

namespace PlotGesture
{
	/**
	 * Is this segment a service road - something a truck may drive on?
	 *
	 * ASKED OF THE PROFILE'S TRAVERSAL CLASS, not of an ERoadKind on the segment, because
	 * the segment does not carry one: the kind is a CHOICE the facade resolves into a
	 * profile, and the profile is what survives onto the graph. Snapping to a taxiway would
	 * let the player build a depot that dispatches onto one.
	 */
	bool IsServiceRoad(const URoadNetwork& Network, FRoadSegmentId Id)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || Segment->Profile == nullptr)
		{
			return false;
		}

		// ASKED OF THE GUIDELINES THE PROFILE DECLARES, because "a truck may drive here" is
		// exactly what a GroundVehicle guideline means - and it is the same question
		// FAnchorLink asks of the graph when it joins a depot's pose. A cross-section with
		// no such line is a taxiway however it is labelled.
		for (const FProfileGuideline& Guideline : Segment->Profile->Guidelines)
		{
			if (Guideline.Class == ETraversalClass::GroundVehicle)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * How far off the centreline the plot's frontage sits, uu.
	 *
	 * A SEGMENT'S ENDS ARE NODE POSITIONS, so everything derived from them is on the road's
	 * CENTRELINE - and a plot anchored there is built over half the carriageway, which is
	 * what PIE showed on 2026-09-16. The frontage belongs against the kerb.
	 *
	 * THE SIDE'S OWN HALF WIDTH, not the larger of the two: a profile may be asymmetric
	 * (URoadProfile::CentrelineOffset), and taking the max would leave a strip of unbuilt
	 * ground on the narrow side that nothing explains.
	 */
	double KerbOffset(const URoadNetwork& Network, FRoadSegmentId Id, bool bLeftOfSegment)
	{
		const FRoadSegment* Segment = Network.GetSegment(Id);
		if (Segment == nullptr || Segment->Profile == nullptr)
		{
			return 0.0;
		}
		return bLeftOfSegment ? Segment->Profile->GetHalfWidthLeft()
			: Segment->Profile->GetHalfWidthRight();
	}

	/**
	 * A frontage length quantised to the plot's own steps.
	 *
	 * ROUNDED, NOT FLOORED, which is the difference between a grid that feels magnetic and
	 * one that feels grudging: floored, the cursor must travel a whole further step before
	 * the plot grows, so it always lags behind the hand.
	 *
	 * NOT the shed's own 4 m width (PlotFit::BayWidthUu, retired by issue #182). That number
	 * used to mean the plot's step as well - one number doing two jobs, which is how a plot
	 * could be drawn narrower than anything that could stand in it.
	 */
	double QuantisedFrontage(double Raw)
	{
		if (Raw <= MinFrontageUu)
		{
			return MinFrontageUu;
		}
		const double Steps = FMath::RoundToDouble((Raw - MinFrontageUu) / FrontageStepUu);
		return MinFrontageUu + Steps * FrontageStepUu;
	}

	/**
	 * Which anchor point a click would take on this road, and where it stands.
	 *
	 * ONE RULE, TWO CALLERS: OnClick takes it, and the Idle preview draws it heavier than its
	 * neighbours so the snap is VISIBLE before the click rather than discovered after. A
	 * second copy of this arithmetic is a dot that lights up in one place and anchors in
	 * another.
	 */
	int32 AnchorIndexAt(double SegmentT, double Length)
	{
		// FrontageStepUu, not the shed's 4 m width: an anchor on a 4 m grid under a frontage
		// growing in 5 m steps would let two plots drawn side by side never sit flush, which
		// is the entire reason the frontage has a quantum.
		const double AlongRoad = FMath::Clamp(SegmentT, 0.0, 1.0) * Length;
		const int32 Count = FMath::FloorToInt(Length / FrontageStepUu);
		return FMath::Clamp(FMath::RoundToInt(AlongRoad / FrontageStepUu), 0, Count);
	}

	/**
	 * How far along the road anchor N stands, uu.
	 *
	 * THE MULTIPLICATION LIVES HERE, once. Both callers used to do it themselves against the
	 * shed's own 4 m width (PlotFit::BayWidthUu, retired by issue #182), so moving the index
	 * to the 5 m step left them multiplying by 4 m - the anchor landed 2 km from the cursor
	 * and three tests failed for reasons that looked nothing like the cause. An index and its
	 * stride are one fact.
	 */
	double AnchorOffset(int32 Index)
	{
		return Index * FrontageStepUu;
	}

	/**
	 * The accepted road nearest the cursor - a service road, for the depot - and where along
	 * it the cursor falls.
	 *
	 * "SERVICE ROAD" IS NOW THE CALLER'S Accept, not a rule written in here: the depot tool
	 * passes IsServiceRoad and the stand tool IsTaxiway. The search is the same question
	 * either way - only which carriageway counts differs - and two copies of the loop would
	 * be two reaches that could drift apart.
	 *
	 * ASKED OF THE NETWORK, not read off FToolContext::Snap. That answers "what did the
	 * cursor HIT", which is only ever a segment while the cursor is over the carriageway -
	 * and this gesture is used from where the plot will stand, off the road entirely. A
	 * different question deserves its own search rather than a widened snap radius, which
	 * would change what every other tool's click means.
	 *
	 * NOT FAnchorLink, which was deleted for a related reason: it searched the LIVE graph to
	 * recover a placed depot's frontage, so a road laid later could move it. Searching at
	 * GESTURE time is the safe half - the answer is frozen into the outline at commit.
	 */
	bool NearestRoad(const URoadNetwork& Network, const FVector2D& Cursor,
		FRoadFilter Accept, FRoadSegmentId& OutSegment, double& OutT)
	{
		double BestSquared = AnchorReachUu * AnchorReachUu;
		bool bFound = false;

		const TArray<FRoadSegment>& Segments = Network.GetSegments();
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FRoadSegment& Segment = Segments[Index];
			const FRoadSegmentId Id = Network.SegmentIdAt(Index);
			if (!Segment.bAlive || !Accept(Network, Id))
			{
				continue;
			}

			const FRoadNode* A = Network.GetNode(Segment.A);
			const FRoadNode* B = Network.GetNode(Segment.B);
			if (A == nullptr || B == nullptr)
			{
				continue;
			}

			const double T = RoadGeom::ClosestPointOnSegment(A->Position, B->Position, Cursor);
			const double DistanceSquared =
				FVector2D::DistSquared(FMath::Lerp(A->Position, B->Position, T), Cursor);
			if (DistanceSquared > BestSquared)
			{
				continue;
			}

			BestSquared = DistanceSquared;
			OutSegment = Id;
			OutT = T;
			bFound = true;
		}
		return bFound;
	}
}
