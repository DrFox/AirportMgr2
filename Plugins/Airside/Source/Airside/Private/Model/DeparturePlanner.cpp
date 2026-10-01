#include "Model/DeparturePlanner.h"

#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RunwayQuery.h"
#include "Model/TakeoffRun.h"
#include "Model/TrafficOccupancy.h"

namespace DeparturePlanner
{
	FDeparturePlan Plan(const URoadNetwork& Network, FGuidelineNodeId Start,
		const FVector2D& OnRunway, const FAirframe& Airframe, ETraversalClass Class)
	{
		// OnRunway picks the RUNWAY; the END is the one in use (spec 2026-09-28-runway-in-use).
		// It used to be the threshold nearest OnRunway, and PlanAny asked both - so a departure
		// took whichever end gave the shorter taxi and met the landings coming the other way
		// (samples/deadlock.png).
		FDeparturePlan Out;
		if (!Network.InUseRunwayAt(OnRunway, Out.End))
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
		// THE FIELD LENGTH, NOT JUST THE ROLL, where one is published: admission judged the
		// whole strip against it, and an entry that left only the roll let an SR22 admitted to
		// a 430 m strip leave from 44 m in with 386 m to go (2026-09-27,
		// Airside.Model.DeparturePlanner.EntryLeavesTheFieldLength). The roll stays the floor
		// for a type with no published figure.
		Out.Needed = FMath::Max(FTakeoffRun::RequiredRoll(Airframe.Chassis.Ground, Airframe.Climb),
			Airframe.Requirements.TakeoffFieldLength);

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
			// NeedsPavement: a jet is not planned down a taxiway too weak for it (FRouteQuery::MinimumPavement).
			return RouteSearch::Find(Network,
				FRouteQuery::For(Errand, Start, Candidate, Airframe.Wingspan, Class)
					.NeedsPavement(Airframe.MinimumPavement));
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
			if (!Route.IsDrivable())
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
			if (!Route.IsDrivable())
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
		const FAirframe& Airframe, ETraversalClass Class, const FTrafficOccupancy* Occupancy)
	{
		// EVERY RUNWAY THAT TAKES DEPARTURES, each at its end in use: RunwayQuery::DepartureRunways, the
		// enumeration RunwayAdmission::CheckArrival's "can it leave again" half asks too (#433). It used to be
		// AirsideCapability::Summarise here and a use filter typed below, which CheckArrival did not have - so an
		// arrivals-only field admitted aircraft this function then refused for ever. RunwayCount is every runway,
		// whatever its use: what makes "none takes departures" the player's setting rather than a missing runway.
		int32 RunwayCount = 0;
		const TArray<FRunwayEnd> Runways = RunwayQuery::DepartureRunways(Network, &RunwayCount);

		// NOT ON A GRAPH MID-EDIT - see EDepartureRefusal::GraphBeingEdited. PlanAny, not Plan:
		// it is the entry DepartAgent uses, and refusing per runway would report the last
		// runway's reason for a fact about the whole graph.
		if (Network.AreGuidelinesBehindRoad())
		{
			FDeparturePlan Waiting;
			Waiting.Why = EDepartureRefusal::GraphBeingEdited;
			return Waiting;
		}

		FDeparturePlan Best;
		Best.Why = EDepartureRefusal::NoRunway;
		bool bHaveRefusal = false;
		FRunwayRank BestRank;

		// A RUNWAY SET TO ARRIVALS ONLY is not in Runways at all - the player's segregation, not a refusal to
		// report per strip.
		for (const FRunwayEnd& Runway : Runways)
		{
			// ONE POINT PER RUNWAY, just inside an end: RunwayExtentAt's proximity gate is against
			// the nearest segment end, so a midpoint on a long segment is "not on a runway". It
			// used to be a point inside EACH end, which is how a departure came to take off from
			// whichever end was nearer its stand; Plan now resolves the end in use itself, so
			// both probes would plan the same departure twice.
			const FVector2D OnRunway = Runway.PointAt(10.0);
			const FDeparturePlan Candidate = Plan(Network, Start, OnRunway, Airframe, Class);
			if (Candidate.IsValid())
			{
				// FREE, THEN DEDICATED, THEN SHORTEST - FRunwayRank, the comparison ArrivalPlanner::Plan makes
				// too (#433). Held is asked of the whole strip, the claim a departure makes at its handover
				// (GroundTraffic).
				// ENFORCED BY: Check-Architecture.ps1 rule 39 (both planners must call RunwayQuery::RankRunway).
				const FRunwayRank Rank = RunwayQuery::RankRunway(Network, Candidate.End, ERunwayTraffic::Departure,
					Occupancy, Candidate.Route.Length);
				if (!Best.IsValid() || Rank.Beats(BestRank))
				{
					Best = Candidate;
					BestRank = Rank;
				}
			}
			else if (!Best.IsValid() && !bHaveRefusal)
			{
				Best = Candidate;
				bHaveRefusal = true;
			}
		}
		if (Runways.IsEmpty() && RunwayCount > 0)
		{
			Best.Why = EDepartureRefusal::NoDepartureRunway;
		}
		return Best;
	}

	FString Describe(const FDeparturePlan& Plan)
	{
		// IN METRES (#497 review), as RunwayAdmission::Describe - the NotAdmitted branch below already hands it its figures, and a
		// departure's sentence reached the inspector and the log in uu beside it. What the strip HAS rounds down, what the
		// aircraft NEEDS rounds up (RunwayAdmission::HaveMetres), so "available" can never print at or above "needed" when short.
		switch (Plan.Why)
		{
		case EDepartureRefusal::NoRunway:      return TEXT("Departure refused: not on a runway.");
		case EDepartureRefusal::NoPerformance: return TEXT("Departure refused: the airframe has no take-off or climb performance.");
		case EDepartureRefusal::NoRoute:
			return FString::Printf(TEXT("Departure refused: no taxi route reaches the runway with %.0f m left to roll."),
				RunwayAdmission::NeedMetres(Plan.Needed));
		case EDepartureRefusal::NotAdmitted:
			return FString::Printf(TEXT("Departure refused: %s."), *RunwayAdmission::Describe(Plan.Admission));
		case EDepartureRefusal::NotParked:  return TEXT("Departure refused: the aircraft is not parked.");
		case EDepartureRefusal::GraphBeingEdited: return TEXT("Departure waiting: the airport is being edited.");
		case EDepartureRefusal::NoDepartureRunway:
			return TEXT("Departure refused: every runway is set to arrivals only - set one to departures or mixed.");
		case EDepartureRefusal::None:
			return FString::Printf(TEXT("Departure: %s entry %.0f m past the threshold, %.0f m available of %.0f, %.0f needed, taxiing %.0f m."),
				Plan.bBacktrack ? TEXT("backtrack to the") : TEXT("intersection"),
				FMath::RoundToDouble(Plan.EntryOffset / 100.0), RunwayAdmission::HaveMetres(Plan.Available),
				RunwayAdmission::HaveMetres(Plan.End.Length), RunwayAdmission::NeedMetres(Plan.Needed), FMath::RoundToDouble(Plan.Route.Length / 100.0));
		}
		return TEXT("Departure: unknown");
	}
}
