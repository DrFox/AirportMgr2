#include "Model/DeparturePlanner.h"

#include "Model/AirsideCapability.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/TakeoffRun.h"

namespace DeparturePlanner
{
	FDeparturePlan Plan(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FVector2D& OnRunway, const FAirframe& Airframe, ETraversalClass Class)
	{
		FDeparturePlan Out;
		if (!Network.RunwayExtentAt(OnRunway, Out.End))
		{
			Out.Why = EDepartureRefusal::NoRunway;
			return Out;
		}

		// May it use this runway at all - the same first question ArrivalPlanner asks, for
		// the same reason: a refusal by surface or field length is permanent and must be
		// named before any entry is searched for, or "no route" would be reported for a
		// strip the aircraft could taxi to but never roll from.
		Out.Admission = RunwayAdmission::Check(Network, Out.End.Seed, Airframe, false);
		if (!Out.Admission.IsAdmitted())
		{
			Out.Why = EDepartureRefusal::NotAdmitted;
			return Out;
		}

		if (!Airframe.Chassis.Ground.IsSet() || !Airframe.Chassis.Ground.Takeoff.IsSet() || !Airframe.Climb.IsSet())
		{
			Out.Why = EDepartureRefusal::NoPerformance;
			return Out;
		}
		Out.Needed = FTakeoffRun::RequiredRoll(Airframe.Chassis.Ground, Airframe.Climb);

		// Every strip node from the threshold, nearest first. MinDistance 0: a node AT the
		// threshold is the backtrack's goal, and the first one past it with enough runway
		// left is the intersection departure's. Tested against Seed's OWN chain width per
		// segment (#87), not a HalfWidth measured across every runway on the airport.
		// Threshold/Direction are OUR OWN end, not re-derived from Seed - see
		// RunwayExitNodes's own comment: a seed has two ends and only we know which is meant.
		const TArray<FGuidelineNodeId> Candidates =
			Network.RunwayExitNodes(Out.End.Seed, Out.End.Threshold, Out.End.Direction, 0.0);

		auto OffsetOf = [&](FGuidelineNodeId Node)
		{
			const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
			return Found ? Out.End.OffsetOf(Found->Position) : 0.0;
		};

		// The two loops below differ in more than these two lines (see #103 review reply on
		// the item this replaces): loop 1 breaks early on insufficient runway and checks
		// arrival direction; loop 2 checks route validity first and continues rather than
		// breaks. Only the QUERY and the ACCEPTED-ENTRY shapes were actually identical
		// between them, so only those are factored out - neither loop's break/continue is
		// touched.
		auto TryRoute = [&](FGuidelineNodeId Candidate, ERouteErrand Errand)
		{
			// THE ERRAND, NOT AN AVOIDANCE. The two loops below differ in which one they
			// mean - a normal entry may never touch a strip, a backtrack exists to - and
			// naming the errand is what puts that difference in the one table rather than
			// in two arguments at two call sites.
			return RouteSearch::Find(Network,
				FRouteQuery::For(Errand, Start, Candidate, Airframe.Wingspan, Class));
		};

		auto Accept = [&](const FRoutePlan& Route, FGuidelineNodeId Candidate, double Offset, bool bBacktrack)
		{
			Out.Route = Route;
			Out.Entry = Candidate;
			Out.EntryOffset = Offset;
			Out.Available = Out.End.Length - Offset;
			Out.bBacktrack = bBacktrack;
			Out.Why = EDepartureRefusal::None;
		};

		// 1. INTERSECTION DEPARTURE. Runway edges excluded, so the taxi can only arrive by a
		//    turn path - and must arrive heading down the runway, or it is the hairpin the
		//    other way and no entry at all.
		for (const FGuidelineNodeId& Candidate : Candidates)
		{
			const double Offset = OffsetOf(Candidate);
			if (Out.End.Length - Offset < Out.Needed)
			{
				// Sorted from the threshold: everything after this has less runway still.
				break;
			}
			const FRoutePlan Route = TryRoute(Candidate, ERouteErrand::DepartureToEntry);
			if (!Route.IsValid() || Route.Polyline.Num() < 2)
			{
				continue;
			}
			const FVector2D LastSpan = Route.Polyline.Last() - Route.Polyline[Route.Polyline.Num() - 2];
			if (FVector2D::DotProduct(LastSpan, Out.End.Direction) <= 0.0)
			{
				continue;
			}
			Accept(Route, Candidate, Offset, /*bBacktrack*/ false);
			return Out;
		}

		// 2. BACKTRACK to the threshold, along the strip if that is the only way: the whole
		//    runway from its end, and a turn on the spot to face down it.
		for (const FGuidelineNodeId& Candidate : Candidates)
		{
			const FRoutePlan Route = TryRoute(Candidate, ERouteErrand::DepartureBacktrack);
			if (!Route.IsValid() || Route.Polyline.Num() < 2)
			{
				continue;
			}
			const double Offset = OffsetOf(Candidate);
			if (Out.End.Length - Offset < Out.Needed)
			{
				continue;
			}
			Accept(Route, Candidate, Offset, /*bBacktrack*/ true);
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
			const FVector2D Ends[2] = { R.End.Threshold + R.End.Direction * 10.0, R.End.FarEnd() - R.End.Direction * 10.0 };
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
				Plan.EntryOffset, Plan.Available, Plan.End.Length, Plan.Needed, Plan.Route.Length);
		}
		return TEXT("Departure: unknown");
	}
}
