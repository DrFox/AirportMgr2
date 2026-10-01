#pragma once

#include "CoreMinimal.h"

struct FRoutePlan;

// A ROUTE CHANGE IS ONE STATE CHANGE, and before issue #429 it was nine. Every operation that hands a live agent a new
// plan - a redirect, an extension, a splice for a recalled truck or a deadlock, a rebuild's rejoin or truncation, the
// drive-side flip, the player's rescue, a held taxi out planned again - re-derived the aftermath by hand: which claims
// to let go, whether the wait was over, whether the engine and the pose carried on, how the goal moved. Each picked a different
// subset, and the ops plugin and the game module composed a tenth and eleventh of their own from the agent's
// internals. The subsets are now NAMED here, and ONE function applies them in one order: FRoadAgent::ApplyRouteChange
// (the follower, the claims it gives back, the wait, the engine, the pose, a goal kept or re-pointed) under
// UGroundTraffic::ChangeRoute (a goal that MOVES, and its claim, which only the traffic can move). A new caller picks
// values; it does not get to pick steps.
//
// NOT A UENUM, any of them: nothing stores a route change - it is built on a caller's stack for one call - so there is
// nothing for UHT to reflect, and a UENUM would drag the generated header into every includer for no reader.

/** How the follower takes the new plan. Each value names the FRoadAgent operation it is and the pose it keeps. */
enum class ERouteMotion : uint8
{
	/**
	 * EVERY OLD STEP KEPT and the plan only grows (UGroundTraffic::ExtendRoute): FRouteFollower::Replace, so Travelled,
	 * Speed, Heading and a tow's chain carry on. THE WAIT STANDS - the route ahead of the agent is the route it had, so
	 * a refusal on it is still a refusal, its claims still describe it, and its stall clock is still counting the same
	 * wait. With Truncate, below, the motions that do not end it.
	 */
	Extend,

	/**
	 * THE ROUTE CUT SHORT ON ITS OWN LINE, a prefix of it kept in place (a rebuild's truncation, FPlanReResolver::
	 * ReResolvePlan): Replace, for the speed profile - the agent brakes to the new end instead of running off it.
	 * Extend's aftermath - nothing let go, the wait left standing - and for the rebuild's reason: it drops every
	 * guideline claim and re-arbitrates every agent in its own claim pass straight after, so a release or a cleared
	 * wait here would be undone before anything read it, and a reset stall clock would be the one thing that was not.
	 * ENFORCED BY: Airside.Model.RouteChange.TruncateKeepsTheWait (after OnGraphRebuilt the staged refusal is gone and
	 * the ground is claimed again), Airside.Model.Traffic.PushGroundFreed.RebuildKeepsTaxiingBlocker
	 */
	Truncate,

	/**
	 * THE STEPS AHEAD OF A NODE CHANGE, the line up to it is byte-identical (ReplanAt, RerouteAgent): Replace, as
	 * Extend. The wait ends - what it waited for is not on the route any more.
	 */
	Splice,

	/**
	 * FROM REST, InitialTravelled along the new plan (RedirectAgent, a held taxi out planned again): FRoadAgent::
	 * RestartTaxi. KeptHeading, when set, keeps the pose where it is facing (a tow's cab, a held aeroplane); unset,
	 * the pose falls back to the line's start. THE ENGINE CARRIES ON (#107): running, at governed RPM; stopped, from
	 * the RPM it was spooling down through - RestartTaxi's own cold start is undone, because a route change is by
	 * definition an agent already out, and only a dispatch (StartTaxi, StartDrive) starts one cold.
	 */
	Restart,

	/**
	 * ONTO OTHER PAVEMENT, STILL ROLLING, at At and Travelled along the new plan's first step (a rebuild's rejoin, the
	 * drive-side flip, the player's rescue): FRoadAgent::RejoinTaxi. Speed, heading, engine spool and the rest of the
	 * pose kept; only the position moves.
	 */
	Rejoin,
};

/** What of the agent's old claims the change lets go NOW, rather than leaving them to the next claim pass. */
enum class ERouteRelease : uint8
{
	/**
	 * NOTHING. An extension's claims all still describe its route. The next claim pass's ReleaseExcept drops whatever
	 * the new route does not ask for in any case - this enum is about the frame BEFORE it, when a planner reading the
	 * table between ticks (DepartAgent, the stand retry, the ops drain) must not see ground held for a route nobody
	 * is driving.
	 */
	None,

	/**
	 * THE RESERVATIONS - the line ahead, made for a route that no longer exists. What the agent STANDS on is kept:
	 * a body is where it is whatever its plan says, and dropping an occupied strip showed a runway free to
	 * ArrivalPlanner with an aeroplane on it (FTrafficOccupancy::ReleaseReservations). A splice, a restart from where
	 * the agent stands.
	 */
	Reservations,

	/**
	 * THE RESERVATIONS AND EVERY GUIDELINE CLAIM, occupied ones too: the agent is on other pavement now (a rejoin), or
	 * holds where a dead route left it. Only the runway surface its body is on stays (FTrafficOccupancy::
	 * ReleaseGuidelineClaimsOf, for the same ArrivalPlanner reason).
	 */
	ReservationsAndGuidelines,
};

/**
 * How the goal follows the new plan WHEN IT DOES NOT MOVE - the two things the agent can do to its own goal, and the
 * only two FRoadAgent::ApplyRouteChange takes.
 *
 * NO Move HERE, ON PURPOSE (#429 review). A goal that moves has a claim that moves with it - the old goal's let go and
 * a stand wait cleared (UGroundTraffic::ReleaseGoal), the new goal's departure armed and its claim raised (TakeGoal) -
 * and those are the traffic's. A Move value here would be a state the agent alone could be handed and could not honour;
 * it was, refused at run time with an Error, and every caller then discarded the bool that said so. Moving a goal IS
 * UGroundTraffic::ChangeRoute, so it has no value to pass.
 */
enum class ERouteGoal : uint8
{
	/** GoalNode untouched: the new plan ends where the old one did (ReplanAt searches to the agent's own goal). */
	Keep,

	/**
	 * GoalNode RE-POINTED at the new plan's end (FRoadAgent::SetGoalFrom), with no claim let go or taken: the same
	 * place, possibly under a new handle. A rebuild's rejoin and truncation - the rebuild drops every guideline claim
	 * and re-makes them all in its own claim pass, so a claim moved here would be moved twice.
	 * ENFORCED BY: Airside.Model.RouteChange.TruncateKeepsTheWait (the ground claimed again after OnGraphRebuilt)
	 */
	Repoint,
};

/**
 * One route change: the plan, how the follower takes it and what it lets go. NOT the goal - that is the call's: kept or
 * re-pointed by FRoadAgent::ApplyRouteChange (ERouteGoal), moved by UGroundTraffic::ChangeRoute. Built by the
 * factories below, one per motion, so a call site names the motion and the choices that motion leaves open, and has no
 * way to pass a field the motion does not read (a Rejoin's At to a splice, a Restart's heading to a rejoin).
 *
 * THE PLAN IS HELD BY REFERENCE: a change is built and applied inside one statement or one scope, on the caller's
 * stack, and a copy of a whole route per change would be the only cost the seam added.
 */
struct FRouteChange
{
	const FRoutePlan& Plan;
	ERouteMotion Motion;
	ERouteRelease Release;

	/**
	 * Restart and Rejoin: how far along Plan the agent starts. Extend: the DRIVEN HISTORY trimmed off the front of the
	 * plan (ExtendRoute's KeepBehind), which Travelled is rebased by after Replace. Splice, Truncate: unread - Travelled
	 * survives.
	 */
	double Travelled = 0.0;

	/** Rejoin: where on the new plan's first step the agent now is. */
	FVector2D At = FVector2D::ZeroVector;

	/** Restart: the heading the pose keeps (see ERouteMotion::Restart); unset falls back to the line's. */
	TOptional<double> KeptHeading;

	/** Every old step kept. DroppedHistory: see Travelled. ExtendRoute moves the goal to the extended end. */
	static FRouteChange Extend(const FRoutePlan& InPlan, double DroppedHistory)
	{
		FRouteChange Change{InPlan, ERouteMotion::Extend, ERouteRelease::None};
		Change.Travelled = DroppedHistory;
		return Change;
	}

	/** The route cut to a prefix of itself, in place. See ERouteMotion::Truncate; the rebuild re-points the goal. */
	static FRouteChange Truncate(const FRoutePlan& InPlan)
	{
		return FRouteChange{InPlan, ERouteMotion::Truncate, ERouteRelease::None};
	}

	/** The steps ahead of a node replaced; the reservations for them let go. */
	static FRouteChange Splice(const FRoutePlan& InPlan)
	{
		return FRouteChange{InPlan, ERouteMotion::Splice, ERouteRelease::Reservations};
	}

	/** From rest along InPlan. Release is the caller's: see ERouteRelease for which restart lets go of what. */
	static FRouteChange Restart(const FRoutePlan& InPlan, ERouteRelease InRelease, double InitialTravelled,
		TOptional<double> InKeptHeading)
	{
		FRouteChange Change{InPlan, ERouteMotion::Restart, InRelease};
		Change.Travelled = InitialTravelled;
		Change.KeptHeading = InKeptHeading;
		return Change;
	}

	/** Onto InPlan at InAt, InitialTravelled along its first step, still rolling; every guideline claim let go. */
	static FRouteChange Rejoin(const FRoutePlan& InPlan, double InitialTravelled, const FVector2D& InAt)
	{
		FRouteChange Change{InPlan, ERouteMotion::Rejoin, ERouteRelease::ReservationsAndGuidelines};
		Change.Travelled = InitialTravelled;
		Change.At = InAt;
		return Change;
	}
};
