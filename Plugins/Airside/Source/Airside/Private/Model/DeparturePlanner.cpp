#include "Model/DeparturePlanner.h"

#include "Model/AirsideCapability.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TakeoffRun.h"

namespace DeparturePlanner
{
	FDeparturePlan Plan(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FVector2D& OnRunway, const FAirframe& Airframe, ETraversalClass Class)
	{
		FDeparturePlan Out;
		FRoadSegmentId Seed;
		if (!Network.RunwayExtentAt(OnRunway, Out.Threshold, Out.Direction, Out.RunwayLength, &Seed))
		{
			Out.Why = EDepartureRefusal::NoRunway;
			return Out;
		}

		// May it use this runway at all - the same first question ArrivalPlanner asks, for
		// the same reason: a refusal by surface or field length is permanent and must be
		// named before any entry is searched for, or "no route" would be reported for a
		// strip the aircraft could taxi to but never roll from.
		Out.Admission = RunwayAdmission::Check(Network, Seed, Airframe, false);
		if (!Out.Admission.IsAdmitted())
		{
			Out.Why = EDepartureRefusal::NotAdmitted;
			return Out;
		}

		if (!Airframe.Ground.IsSet() || !Airframe.Ground.Takeoff.IsSet() || !Airframe.Climb.IsSet())
		{
			Out.Why = EDepartureRefusal::NoPerformance;
			return Out;
		}
		Out.Needed = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);

		// Every strip node from the threshold, nearest first. MinDistance 0: a node AT the
		// threshold is the backtrack's goal, and the first one past it with enough runway
		// left is the intersection departure's. Tested against Seed's OWN chain width per
		// segment (#87), not a HalfWidth measured across every runway on the airport.
		const TArray<FGuidelineNodeId> Candidates = Network.RunwayExitNodes(Seed, 0.0);

		auto OffsetOf = [&](FGuidelineNodeId Node)
		{
			const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
			return Found ? FVector2D::DotProduct(Found->Position - Out.Threshold, Out.Direction) : 0.0;
		};

		// 1. INTERSECTION DEPARTURE. Runway edges excluded, so the taxi can only arrive by a
		//    turn path - and must arrive heading down the runway, or it is the hairpin the
		//    other way and no entry at all.
		for (const FGuidelineNodeId& Candidate : Candidates)
		{
			const double Offset = OffsetOf(Candidate);
			if (Out.RunwayLength - Offset < Out.Needed)
			{
				// Sorted from the threshold: everything after this has less runway still.
				break;
			}
			FRouteQuery Query;
			Query.Start = Start;
			Query.Goal = Candidate;
			Query.Class = Class;
			Query.Wingspan = Airframe.Wingspan;
			Query.AvoidRunways = ERunwayAvoidance::All;
			const FRoutePlan Route = RouteSearch::Find(Network, Query);
			if (!Route.IsValid() || Route.Polyline.Num() < 2)
			{
				continue;
			}
			const FVector2D LastSpan = Route.Polyline.Last() - Route.Polyline[Route.Polyline.Num() - 2];
			if (FVector2D::DotProduct(LastSpan, Out.Direction) <= 0.0)
			{
				continue;
			}
			Out.Route = Route;
			Out.Entry = Candidate;
			Out.EntryOffset = Offset;
			Out.Available = Out.RunwayLength - Offset;
			Out.bBacktrack = false;
			Out.Why = EDepartureRefusal::None;
			return Out;
		}

		// 2. BACKTRACK to the threshold, along the strip if that is the only way: the whole
		//    runway from its end, and a turn on the spot to face down it.
		for (const FGuidelineNodeId& Candidate : Candidates)
		{
			FRouteQuery Query;
			Query.Start = Start;
			Query.Goal = Candidate;
			Query.Class = Class;
			Query.Wingspan = Airframe.Wingspan;
			Query.AvoidRunways = ERunwayAvoidance::None;
			const FRoutePlan Route = RouteSearch::Find(Network, Query);
			if (!Route.IsValid() || Route.Polyline.Num() < 2)
			{
				continue;
			}
			const double Offset = OffsetOf(Candidate);
			if (Out.RunwayLength - Offset < Out.Needed)
			{
				continue;
			}
			Out.Route = Route;
			Out.Entry = Candidate;
			Out.EntryOffset = Offset;
			Out.Available = Out.RunwayLength - Offset;
			Out.bBacktrack = true;
			Out.Why = EDepartureRefusal::None;
			return Out;
		}

		Out.Why = EDepartureRefusal::NoRoute;
		return Out;
	}

	FDeparturePlan PlanAny(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FAirframe& Airframe, ETraversalClass Class)
	{
		// The capability summary already enumerates chains once each, by threshold pair;
		// re-deriving that walk here would be a second enumerator to keep in step.
		const FAirsideCapability Cap = AirsideCapability::Summarise(Network);

		FDeparturePlan Best;
		Best.Why = EDepartureRefusal::NoRunway;
		bool bHaveRefusal = false;

		for (const FRunwaySummary& R : Cap.Runways)
		{
			// A point just inside EACH end: RunwayExtentAt's proximity gate is against the
			// nearest segment end, so a midpoint on a long segment is "not on a runway" and
			// the threshold it hands back is the one nearest the point asked about.
			const FVector2D Ends[2] = { R.Threshold + R.Direction * 10.0, R.Threshold + R.Direction * (R.Length - 10.0) };
			for (const FVector2D& OnRunway : Ends)
			{
				const FDeparturePlan Candidate = Plan(Network, Start, OnRunway, Airframe, Class);
				if (Candidate.IsValid())
				{
					if (!Best.IsValid() || Candidate.Route.Length < Best.Route.Length)
					{
						Best = Candidate;
					}
				}
				else if (!Best.IsValid() && !bHaveRefusal)
				{
					Best = Candidate;
					bHaveRefusal = true;
				}
			}
		}
		return Best;
	}

	FString Describe(const FDeparturePlan& Plan)
	{
		switch (Plan.Why)
		{
		case EDepartureRefusal::NoRunway:      return TEXT("Departure refused: not on a runway.");
		case EDepartureRefusal::NoPerformance: return TEXT("Departure refused: the airframe has no take-off or climb performance.");
		case EDepartureRefusal::NoRoute:
			return FString::Printf(TEXT("Departure refused: no taxi route reaches the runway with %.0f uu left to roll."), Plan.Needed);
		case EDepartureRefusal::NotAdmitted:
			return FString::Printf(TEXT("Departure refused: %s."), *RunwayAdmission::Describe(Plan.Admission));
		case EDepartureRefusal::NotParked:  return TEXT("Departure refused: the aircraft is not parked.");
		case EDepartureRefusal::None:
			return FString::Printf(TEXT("Departure: %s entry %.0f uu past the threshold, %.0f uu available of %.0f, %.0f needed, taxiing %.0f uu."),
				Plan.bBacktrack ? TEXT("backtrack to the") : TEXT("intersection"),
				Plan.EntryOffset, Plan.Available, Plan.RunwayLength, Plan.Needed, Plan.Route.Length);
		}
		return TEXT("Departure: unknown");
	}
}
