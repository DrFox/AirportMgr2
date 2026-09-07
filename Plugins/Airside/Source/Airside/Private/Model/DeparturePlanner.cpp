#include "Model/DeparturePlanner.h"

#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TakeoffRun.h"
#include "Profiles/RoadProfile.h"

namespace DeparturePlanner
{
	FDeparturePlan Plan(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FVector2D& OnRunway, const FAirframe& Airframe, ETraversalClass Class)
	{
		FDeparturePlan Out;
		if (!Network.RunwayExtentAt(OnRunway, Out.Threshold, Out.Direction, Out.RunwayLength))
		{
			Out.Why = EDepartureRefusal::NoRunway;
			return Out;
		}
		if (!Airframe.Ground.IsSet() || !Airframe.Ground.Takeoff.IsSet() || !Airframe.Climb.IsSet())
		{
			Out.Why = EDepartureRefusal::NoPerformance;
			return Out;
		}
		Out.Needed = FTakeoffRun::RequiredRoll(Airframe.Ground, Airframe.Climb);

		// The strip's half width bounds which nodes count as on it - the widest continuous
		// profile, exactly as ArrivalPlanner reads it.
		double HalfWidth = 0.0;
		for (const FRoadSegment& Segment : Network.GetSegments())
		{
			if (!Segment.bAlive) { continue; }
			const URoadProfile* Profile = Network.ProfileFor(Segment);
			if (Profile != nullptr && Profile->bContinuousThroughJunctions)
			{
				HalfWidth = FMath::Max(HalfWidth, Profile->GetTotalWidth() * 0.5);
			}
		}
		// Every strip node from the threshold, nearest first. MinDistance 0: a node AT the
		// threshold is the backtrack's goal, and the first one past it with enough runway
		// left is the intersection departure's.
		const TArray<FGuidelineNodeId> Candidates =
			Network.RunwayExitNodes(Out.Threshold, Out.Direction, Out.RunwayLength, HalfWidth, 0.0);

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
			Query.bAvoidRunways = true;
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
			Query.bAvoidRunways = false;
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

	FString Describe(const FDeparturePlan& Plan)
	{
		switch (Plan.Why)
		{
		case EDepartureRefusal::NoRunway:      return TEXT("Departure refused: not on a runway.");
		case EDepartureRefusal::NoPerformance: return TEXT("Departure refused: the airframe has no take-off or climb performance.");
		case EDepartureRefusal::NoRoute:
			return FString::Printf(TEXT("Departure refused: no taxi route reaches the runway with %.0f uu left to roll."), Plan.Needed);
		case EDepartureRefusal::None:
			return FString::Printf(TEXT("Departure: %s entry %.0f uu past the threshold, %.0f uu available of %.0f, %.0f needed, taxiing %.0f uu."),
				Plan.bBacktrack ? TEXT("backtrack to the") : TEXT("intersection"),
				Plan.EntryOffset, Plan.Available, Plan.RunwayLength, Plan.Needed, Plan.Route.Length);
		}
		return TEXT("Departure: unknown");
	}
}
