#include "Model/RoutePolicy.h"

FRoutePolicy FRoutePolicy::For(ERouteErrand Errand)
{
	auto Make = [](ERunwayAvoidance Avoidance, EOccupancyUse Occupancy, bool bPenalise)
	{
		FRoutePolicy Policy;
		Policy.Avoidance = Avoidance;
		Policy.Occupancy = Occupancy;
		Policy.bPenaliseRunways = bPenalise;
		return Policy;
	};

	switch (Errand)
	{
	// A SWITCH WITH NO DEFAULT, deliberately: adding an enumerator without a row here is a
	// compiler warning at this switch, which is a cheaper place to find out than the table
	// test and very much cheaper than play.
	case ERouteErrand::ArrivalTaxiIn:
		// As shipped. The query carries no occupancy - ChooseStand reads the table
		// separately, to skip a stand somebody is on, never to weight an edge. A stand
		// chosen by congestion would be re-chosen every tick until the aircraft committed.
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::DepartureToEntry:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::DepartureBacktrack:
		// NO PENALTY. The backtrack's whole purpose is the strip; charging it would make a
		// legal backtrack lose to nothing at all, since there is no alternative to lose to.
		return Make(ERunwayAvoidance::None, EOccupancyUse::Never, false);

	case ERouteErrand::PushbackClear:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::PushbackTaxiOut:
		// GAINS All. This site never declared a policy and got None by omission - it is one
		// of the two that reproduce the 2026-09-21 report, being the long taxi from where a
		// push ends to the runway entry.
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::Replan:
		return Make(ERunwayAvoidance::Held, EOccupancyUse::Required, true);

	case ERouteErrand::RebuildReResolve:
		// GAINS Held. ReResolvePlan set congestion but never avoidance.
		return Make(ERunwayAvoidance::Held, EOccupancyUse::Required, true);

	case ERouteErrand::VehicleToJob:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Required, false);

	case ERouteErrand::CandidateComparison:
		return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);

	case ERouteErrand::PlayerIssued:
		// NONE, NOT All, and this is the one row where graceful degradation does the work: a
		// player who clicks two points across a runway is stating an intent, and refusing it
		// outright reads as a broken tool. The penalty sends the route round when round
		// exists and takes the strip when it does not.
		return Make(ERunwayAvoidance::None, EOccupancyUse::Never, true);

	case ERouteErrand::GraphProbe:
		return Make(ERunwayAvoidance::None, EOccupancyUse::Never, false);

	case ERouteErrand::Unset:
		break;
	}

	// Unset, and anything a future cast smuggles past the switch. The MOST RESTRICTIVE row,
	// not the permissive one: this is reached before the search's own refusal has had a
	// chance to fire, and a policy read through some path that skipped that refusal still
	// must not put an aircraft on a strip.
	return Make(ERunwayAvoidance::All, EOccupancyUse::Never, false);
}
