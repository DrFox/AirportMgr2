#include "Model/ArrivalPlanner.h"

#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Profiles/RoadProfile.h"

namespace ArrivalPlanner
{
	FArrivalPlan Plan(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe)
	{
		FArrivalPlan Out;

		// 1. WHICH RUNWAY. Nearest threshold to the query point, which is the user's own choice
		//    of rule - there is no wind model, so nothing else could decide it.
		if (!Network.NearestRunwayThreshold(Near, Out.Threshold, Out.Direction, Out.RunwayLength))
		{
			Out.Why = EArrivalRefusal::NoRunway;
			return Out;
		}

		// The distance the model actually flies, plus its margin - see FLandingRun. The closed
		// form this replaced demanded 649 m of a 297 m landing and refused every runway on the
		// field, which is what "pressing 7 does nothing" turned out to be.
		Out.Needed = FLandingRun::RequiredLandingDistance(Airframe.Ground, Airframe.Climb, Airframe.Approach)
			* FLandingRun::LandingMargin;

		// The runway's own width bounds what counts as ON it, the same figure RunwayExtentAt
		// uses for its reach - so "on the runway" means one thing across the whole model.
		double HalfWidth = 0.0;
		for (const FRoadSegment& Segment : Network.GetSegments())
		{
			if (!Segment.bAlive)
			{
				continue;
			}
			const URoadProfile* SegmentProfile = Network.ProfileFor(Segment);
			if (SegmentProfile != nullptr && SegmentProfile->bContinuousThroughJunctions)
			{
				HalfWidth = FMath::Max(HalfWidth, SegmentProfile->GetTotalWidth() * 0.5);
			}
		}

		// 2. THE EARLIEST EXIT IT COULD TAKE, asked before anything is armed - the same
		//    discipline as a departure refusing a strip it cannot leave.
		//
		//    CALLED ONCE, and ALWAYS - even when the runway is already too short to matter -
		//    so ExitCount is populated on every path DispatchArrival logs from, RunwayTooShort
		//    included (it comes back 0 there: MinDistance Needed exceeds a too-short runway,
		//    so nothing qualifies, which is the right answer to report). DispatchArrival used
		//    to call this twice, the second time with MinDistance 0 purely to log how many
		//    nodes sat on the strip at all versus how many were far enough down to use. That
		//    count served a diagnostic log line, not a decision - Plan makes no decision from
		//    it - so it is dropped rather than paid for on every dispatch; a caller that wants
		//    it back can run the MinDistance-0 query itself, the same cheap filter this used
		//    to duplicate.
		const TArray<FGuidelineNodeId> Exits =
			Network.RunwayExitNodes(Out.Threshold, Out.Direction, Out.RunwayLength, HalfWidth, Out.Needed);
		Out.ExitCount = Exits.Num();

		if (Out.RunwayLength < Out.Needed)
		{
			Out.Why = EArrivalRefusal::RunwayTooShort;
			return Out;
		}
		if (Exits.Num() == 0)
		{
			Out.Why = EArrivalRefusal::NoExit;
			return Out;
		}

		// 3. WHICH STAND. Shortest route, the user's rule - and taken from the FIRST exit that
		//    reaches anything, because an aircraft takes the earliest turn-off it can rather
		//    than rolling to the end in search of a marginally shorter taxi.
		for (int32 Index = 0; Index < Exits.Num(); ++Index)
		{
			const FGuidelineNodeId& Candidate = Exits[Index];
			double BestLength = TNumericLimits<double>::Max();
			FRoutePlan BestForExit;

			for (const FEntityInstance& Stand : Network.GetEntities())
			{
				if (!Stand.bAlive || !Stand.PoseNode.IsSet())
				{
					continue;
				}

				FRouteQuery Query;
				Query.Start = Candidate;
				Query.Goal = Stand.PoseNode;
				Query.Class = ETraversalClass::Aircraft;
				Query.Wingspan = Airframe.Wingspan;

				const FRoutePlan Route = RouteSearch::Find(Network, Query);
				if (!Route.IsValid() || Route.Polyline.Num() < 2)
				{
					continue;
				}

				if (Route.Length < BestLength)
				{
					BestLength = Route.Length;
					BestForExit = Route;
				}
			}

			if (BestForExit.IsValid())
			{
				Out.TaxiIn = BestForExit;
				Out.Exit = Candidate;
				Out.ExitOrdinal = Index + 1;
				break;
			}
		}

		if (!Out.TaxiIn.IsValid())
		{
			Out.Why = EArrivalRefusal::NoRouteToStand;
			return Out;
		}

		// WHERE IT LEAVES THE RUNWAY, handed to the landing so the rollout carries on to the
		// taxiway at taxi speed instead of stopping wherever the braking ran out.
		Out.VacateAt = Out.RunwayLength;
		if (const FGuidelineNode* ExitNode = Network.GetGuidelineNode(Out.Exit))
		{
			Out.VacateAt = FVector2D::DotProduct(ExitNode->Position - Out.Threshold, Out.Direction);
		}

		Out.Why = EArrivalRefusal::None;
		return Out;
	}

	FString DescribeRefusal(const FArrivalPlan& Plan)
	{
		// Exactly the three refusal branches DispatchArrival used to choose between inline,
		// moved here so the actor logs from the plan it acted on rather than re-deriving why.
		switch (Plan.Why)
		{
		case EArrivalRefusal::NoRunway:
			return TEXT("No runway to land on - draw one first.");

		case EArrivalRefusal::RunwayTooShort:
			return FString::Printf(
				TEXT("Arrival refused: the runway is %.0f uu and this aircraft needs %.0f to ")
				TEXT("stop. Draw a longer runway."),
				Plan.RunwayLength, Plan.Needed);

		case EArrivalRefusal::NoExit:
			return FString::Printf(
				TEXT("Arrival refused: nothing joins the runway beyond %.0f uu, so there is ")
				TEXT("no exit this aircraft could take. Connect a taxiway further down it."),
				Plan.Needed);

		case EArrivalRefusal::NoRouteToStand:
			return FString::Printf(
				TEXT("Arrival refused: %d usable exit(s), but no route from any of them to a ")
				TEXT("stand. Check the taxiway reaches the stands."),
				Plan.ExitCount);

		case EArrivalRefusal::None:
		default:
			return FString();
		}
	}
}
