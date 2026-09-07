#include "Model/ArrivalPlanner.h"

#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

namespace ArrivalPlanner
{
	FGuidelineNodeId ChooseStand(const URoadNetwork& Network, FGuidelineNodeId From,
		const FAirframe& Airframe, const FTrafficOccupancy* Occupancy, int32 ExcludingAgent,
		FRoutePlan* OutRoute, bool* bOutSawHeld)
	{
		FGuidelineNodeId Best;
		FRoutePlan BestRoute;
		double BestLength = TNumericLimits<double>::Max();
		bool bSawHeld = false;
		for (const FEntityInstance& Stand : Network.GetEntities())
		{
			if (!Stand.bAlive || !Stand.PoseNode.IsSet())
			{
				continue;
			}
			FRouteQuery Query;
			Query.Start = From;
			Query.Goal = Stand.PoseNode;
			Query.Class = ETraversalClass::Aircraft;
			Query.Wingspan = Airframe.Wingspan;
			Query.bAvoidRunways = true;
			const FRoutePlan Route = RouteSearch::Find(Network, Query);
			if (!Route.IsValid() || Route.Polyline.Num() < 2 || Route.Steps.Num() == 0)
			{
				continue;
			}
			// Held is asked AFTER reachability, so bSawHeld means "a stand this aircraft could
			// have used" - the only reading under which NoFreeStand is the right word.
			if (Occupancy != nullptr && Occupancy->IsHeld(FTrafficResource::OfNode(Stand.PoseNode), ExcludingAgent))
			{
				bSawHeld = true;
				continue;
			}
			if (Route.Length < BestLength)
			{
				BestLength = Route.Length;
				BestRoute = Route;
				Best = Stand.PoseNode;
			}
		}
		if (OutRoute != nullptr) { *OutRoute = BestRoute; }
		if (bOutSawHeld != nullptr) { *bOutSawHeld = bSawHeld; }
		return Best;
	}

	FArrivalPlan Plan(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe,
		const FTrafficOccupancy* Occupancy)
	{
		FArrivalPlan Out;

		// 1. WHICH RUNWAY. Nearest threshold to the query point, which is the user's own choice
		//    of rule - there is no wind model, so nothing else could decide it.
		if (!Network.NearestRunwayThreshold(Near, Out.Threshold, Out.Direction, Out.RunwayLength, &Out.RunwaySegment))
		{
			Out.Why = EArrivalRefusal::NoRunway;
			return Out;
		}

		Out.RunwayChain = Network.RunwayChain(Out.RunwaySegment);

		// 1a. MAY IT USE THIS RUNWAY AT ALL. Surface, approach, published field length and
		//     width, in that order - before occupancy, because occupancy clears on its own
		//     and this never does: M3's sequencer will queue on RunwayOccupied, and it must
		//     not queue an airliner behind a Piper for a grass strip it can never land on.
		Out.Admission = RunwayAdmission::Check(Network, Out.RunwaySegment, Airframe, true);
		if (!Out.Admission.IsAdmitted())
		{
			Out.Why = EArrivalRefusal::NotAdmitted;
			return Out;
		}

		// Asked before the length and exit steps, because those cannot change while the
		// runway is busy and this can: a refusal that clears on its own is reported as
		// itself, not as whichever later step happened to fail too.
		if (Occupancy != nullptr)
		{
			for (const FRoadSegmentId& Segment : Out.RunwayChain)
			{
				if (Occupancy->IsHeld(FTrafficResource::OfSurface(Segment), 0))
				{
					Out.Why = EArrivalRefusal::RunwayOccupied;
					return Out;
				}
			}
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
		//
		//    From the UNMARGINED distance since the exit arcs (2026-09-06). Needed carries
		//    FLandingRun::LandingMargin, which is a refusal margin on the STRIP - a runway a
		//    quarter shorter than the roll is refused - not a statement about which turn-off
		//    is takeable: the aircraft is at taxi speed by the raw figure (measured: 29632 on
		//    an unbounded strip against 37039 needed), and an arc whose start lay between
		//    the two was skipped for the junction node behind it, which has no turn-off at
		//    all. The player watched the aircraft roll straight past the exit it had built.
		const double SlowedBy = Out.Needed / FLandingRun::LandingMargin;
		const TArray<FGuidelineNodeId> Exits =
			Network.RunwayExitNodes(Out.Threshold, Out.Direction, Out.RunwayLength, HalfWidth, SlowedBy);
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
		//
		//    AN EXIT IS WHERE THE AIRCRAFT LEAVES THE RUNWAY, so the taxi-in is searched with
		//    the runway's own edges off limits - the rule the deadlock replan already lives
		//    by. Without it a junction's own node-end, which has no turn-off since the exit
		//    arcs (the turns attach at the split nodes either side), "exited" by rolling on
		//    to the downstream split and hairpinning back - the aircraft the player saw roll
		//    straight past its exit - and an early exit lost to a later one because the
		//    SHORTEST route from it ran down the strip. With the strip excluded a node-end
		//    has no route at all and the earliest arc wins on its taxiways, as the rule says.
		//    A FORWARD turn-off (the first span of the route heading down the runway) beats
		//    a backtrack at any distance: an aircraft turns off ahead of itself if it can.
		FGuidelineNodeId FirstForward, FirstBacktrack;
		bool bSawHeldStand = false;
		FRoutePlan ForwardRoute, BacktrackRoute;
		int32 ForwardOrdinal = 0, BacktrackOrdinal = 0;
		for (int32 Index = 0; Index < Exits.Num() && !FirstForward.IsSet(); ++Index)
		{
			const FGuidelineNodeId& Candidate = Exits[Index];
			// The stand choice itself is ChooseStand's - one rule for the dispatch, the rebuild
			// and the re-offer - asked here per exit with this aircraft excluded from nothing
			// (0: it does not exist yet).
			FRoutePlan BestForExit;
			bool bHeldHere = false;
			ChooseStand(Network, Candidate, Airframe, Occupancy, 0, &BestForExit, &bHeldHere);
			bSawHeldStand = bSawHeldStand || bHeldHere;
			if (!BestForExit.IsValid())
			{
				continue;
			}
			const bool bForward =
				FVector2D::DotProduct(BestForExit.Polyline[1] - BestForExit.Polyline[0], Out.Direction) > 0.0;
			if (bForward)
			{
				FirstForward = Candidate;
				ForwardRoute = BestForExit;
				ForwardOrdinal = Index + 1;
			}
			else if (!FirstBacktrack.IsSet())
			{
				FirstBacktrack = Candidate;
				BacktrackRoute = BestForExit;
				BacktrackOrdinal = Index + 1;
			}
		}
		if (FirstForward.IsSet())
		{
			Out.TaxiIn = ForwardRoute;
			Out.Exit = FirstForward;
			Out.ExitOrdinal = ForwardOrdinal;
		}
		else if (FirstBacktrack.IsSet())
		{
			Out.TaxiIn = BacktrackRoute;
			Out.Exit = FirstBacktrack;
			Out.ExitOrdinal = BacktrackOrdinal;
		}

		if (!Out.TaxiIn.IsValid())
		{
			// Two refusals for two fixes: no stand reachable at all means build a taxiway; every
			// reachable stand held means wait, or build a stand.
			Out.Why = bSawHeldStand ? EArrivalRefusal::NoFreeStand : EArrivalRefusal::NoRouteToStand;
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

		case EArrivalRefusal::RunwayOccupied:
			return TEXT("Arrival refused: the runway is in use. Wait for it to clear.");

		case EArrivalRefusal::NotAdmitted:
			return FString::Printf(TEXT("Arrival refused: %s."), *RunwayAdmission::Describe(Plan.Admission));

		case EArrivalRefusal::NoFreeStand:
			return TEXT("Arrival refused: every stand it could reach is taken. Wait for one to free, or build another.");

		case EArrivalRefusal::None:
		default:
			return FString();
		}
	}
}
