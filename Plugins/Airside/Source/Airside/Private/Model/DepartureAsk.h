#pragma once

#include "CoreMinimal.h"
#include "Model/DeparturePlanner.h"
#include "Model/PushbackPlanner.h"
#include "Model/TaxiPlanner.h"

// MOVED OUT OF GroundTraffic.cpp (taxi planning PR 2, 2026-10-02), whole, so GroundTrafficPlanning.cpp - which books the
// taxi plan an ask carries - reads the same struct the ask fills. Private: nothing outside UGroundTraffic asks.

/**
 * What AskDeparture found: DepartAgent's decision before it logs or acts. Why is the refusal DepartAgent returns for
 * it - None for a departure it would start, straight out or by a push whose ground is free.
 */
struct FDepartureAsk
{
	FDeparturePlan Plan;
	/** The measured angle between the route's first tangent and the parked heading - DepartAgent's log line. */
	double OffDegrees = 180.0;
	bool bStraightOut = false;
	/** Planned only when the departure is valid and not straight out. */
	FPushbackPlan Push;
	/** The taxi plan the departure would be cleared on (spec 2026-10-02): the push its first window, or straight out. Planned only when all else allows it. */
	FTaxiPlan Taxi;
	/** All else allowed it and the taxi plan did not: Why is PushbackBlocked for the taxi table, not the push ground. */
	bool bNoTaxiPlan = false;
	/**
	 * Set when Taxi goes only AS FAR AS A HOLDING NODE short of the runway (a departure queueing): the entry it is
	 * for and the errand to plan the rest by, once the entry frees (UGroundTraffic::ExtendQueuedDepartures).
	 */
	FGuidelineNodeId QueueFor;
	ERouteErrand QueueErrand = ERouteErrand::Unset;
	EDepartureRefusal Why = EDepartureRefusal::None;
};
