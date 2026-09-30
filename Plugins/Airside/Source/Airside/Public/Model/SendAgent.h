#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"

// "SEND THIS AGENT TO G, FROM WHATEVER IT IS DOING NOW" - AND WHAT HAPPENED (issue #429, part 2). Part 1 made HOW a
// route changes one seam (Model/RouteChange.h, UGroundTraffic::ChangeRoute). What was left outside Airside was the
// choice of WHICH change: AirportOps' UJobBoard::DriveVehicleTo read the truck's Follower.Plan and Travelled to find
// the node ahead, picked RerouteAgent, a rescue or "let it finish" by phase and seeded the tow's judgement by hand;
// ReofferStands chose its verb by phase again (#435); and the player's Unstick copied the deadlock resolver's ban.
// Those choices are UGroundTraffic's now - SendAgentTo, ReplanAroundBlocker, ReplanFromNextNode, ReofferStand and
// RescueToStand - and a caller is told what happened in one of these small enums, which it maps to its own state and
// its own words with an EXHAUSTIVE switch (Model/ExhaustiveSwitch.h): a new outcome is a build error at every caller,
// not a silent default. JobBoard.h's boundary is the reason for the split: "Airside knows how a thing MOVES and must
// never learn what it is FOR" - so nothing here knows a depot, a job or a flight, and every UE_LOG line that names one
// stays with the caller, fed from these results.
//
// NOT UENUMs: nothing stores an outcome - it is read once by the caller that asked - so, as RouteChange.h's own, there
// is nothing for UHT to reflect.

/** What UGroundTraffic::SendAgentTo did, by the phase it found the agent in. See SendAgentTo for each row. */
enum class ESendOutcome : uint8
{
	/** Its goal already WAS the new one - a job re-bid back onto the leg it is on. Nothing changed. */
	AlreadyGoing,
	/** Turned ON THE MOVE, never stopped: a vehicle spliced at a node ahead (RerouteAgent), an aircraft's route extended
	 *  from where it ends (ExtendRoute). FSendAgentResult::TurnAt is the node the new route leaves the old one at. */
	Turned,
	/** Sent from where it STANDS - parked, or a waiter stranded where its wait began - by RedirectAgent. */
	Redirected,
	/** Stranded, and hopped onto pavement near it with a route to the goal (RescueStranded). */
	Rescued,
	/** Still on its route and no turn ahead held: it arrives at its old goal first, and the caller sends it on from
	 *  there. Nothing changed. */
	FinishesLeg,
	/** Reversing or manoeuvring (or on a route that is not drivable): it finishes that motion first, and the caller
	 *  sends it on from where it stops. Nothing changed. */
	FinishesMotion,
	/** Standing still, and no route from where it stands reaches the goal (or the only one found ungated would fold its
	 *  tow - FSendAgentResult::FoldWhy). Nothing changed. */
	NoRoute,
	/** Stranded, and no pavement within the rescue's reach runs its way with a route to the goal. Nothing changed. */
	NoPavement,
	/** No such agent, or one nothing may send anywhere - on a runway (Arriving, Departing) or gone. Nothing changed. */
	NotSendable
};

/**
 * Whether a route that exists only with the VEHICLE GATE OFF may be driven. A route the gate refuses as too narrow is,
 * with DriveAnyway, searched again ungated and driven - a body a little wide for a corner scuffs a kerb - UNLESS it
 * folds the tow (VehicleFit::MayDriveUngated): a trailer past square is a jack-knife, stopped dead holding the road.
 * WHO MAY is the caller's to say, not Airside's: AirportOps drives a truck HOME that way (retiring it at a stand costs a
 * truck on every such job) and never to a job (the job goes back to the board, where a vehicle that fits wins it).
 */
enum class ENarrowRoad : uint8
{
	Refuse,
	DriveAnyway
};

/** SendAgentTo's answer: what it did, and the facts a caller's own log line needs about how. */
struct FSendAgentResult
{
	ESendOutcome Outcome = ESendOutcome::NotSendable;

	/** Turned: the node its new route leaves the old one at. Unset otherwise. */
	FGuidelineNodeId TurnAt;

	/**
	 * Set when a gated search was refused as TOO NARROW and ENarrowRoad::DriveAnyway searched it again ungated - ONCE per
	 * call however many turn nodes were tried, as the caller's line is said once per recall. NarrowWhy is what refused it
	 * (FFitVerdict::Describe) and NarrowEdge where: the player's cue to widen that road. Whether the ungated route was
	 * then driven is Outcome's to say.
	 */
	bool bNarrow = false;
	FString NarrowWhy;
	FGuidelineEdgeId NarrowEdge;

	/** NoRoute only: the ungated route home was found and does not hold the tow - VehicleFit::MayDriveUngated's reason.
	 *  Empty when there was simply no route. */
	FString FoldWhy;
};

/** What UGroundTraffic::ReplanAroundBlocker did - the deadlock resolver's own per-agent step, for one agent. */
enum class EBlockerReplan : uint8
{
	/** Turned round what holds it: spliced at the refused step, what refused it banned. */
	Turned,
	/** Held where the resolver would turn it, and no route round the ban exists. Nothing changed. */
	NoWayRound,
	/** Not held where the resolver would turn it - not stopped at the node its refused step leaves from, not refused
	 *  at all, not turnable (a push, a reverse, a truck at its bay's reverse leg), or refused the runway it is going to.
	 *  Nothing changed. */
	NotAtItsBlock
};

/** What UGroundTraffic::ReplanFromNextNode did - a fresh search from the node ahead, no ban. */
enum class ENextNodeReplan : uint8
{
	/** A different route from the node ahead, under today's congestion. */
	Replanned,
	/** On its last step: there is no node ahead to replan from. Nothing changed. */
	LastStep,
	/** The search found the route it already has - or none, or it cannot be replanned. Nothing changed. */
	NoBetterRoute
};

/** What UGroundTraffic::ReofferStand / RescueToStand did for one aircraft. */
enum class EStandOffer : uint8
{
	/** A free stand was chosen and the aircraft is on its way to it (FStandOffer::Send says by which verb). */
	Sent,
	/** No free stand it fits is reachable from where the search started. Nothing changed. */
	NoFreeStand,
	/** RescueToStand only: no pavement close enough to start the search from, or to rejoin toward the stand. */
	NoPavement,
	/** A stand was chosen and the aircraft could not be sent to it (FStandOffer::Send says why). Nothing changed. */
	NotSent
};

/** A stand offer's answer: what happened, the stand, and - when it was offered one - SendAgentTo's own answer. */
struct FStandOffer
{
	EStandOffer Outcome = EStandOffer::NotSent;
	FGuidelineNodeId Stand;
	FSendAgentResult Send;
};
