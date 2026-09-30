// SendAgentTo, ReofferStand, RescueToStand and RemainingDriveSeconds - issue #429, part 2.
//
// "SEND THIS AGENT TO G FROM WHATEVER IT IS DOING NOW" USED TO BE ANSWERED OUTSIDE THIS CLASS. AirportOps'
// UJobBoard::DriveVehicleTo read a truck's Follower.Plan and Travelled to find the node ahead, chose RerouteAgent, a
// rescue, a wait or RedirectAgent by phase, and built the tow's seed; ReofferStands chose between ExtendRoute and
// RedirectAgent by phase again, and chose wrong once (#435, the teleport); the Unstick found its stand from the
// agent's route. Each is a choice about how a thing MOVES, which JobBoard.h's boundary gives to Airside - so it is
// made here, once, and callers are told what happened (Model/SendAgent.h) and say it in their own words.
//
// A FIFTH TRANSLATION UNIT rather than more of GroundTraffic.cpp (see GroundTraffic.h's file map): these are
// compositions OVER the operations that file owns - RerouteAgent, ExtendRoute, RedirectAgent, RescueStranded - and
// read nothing of this class a public caller could not, bar the occupancy table the stand choice asks.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/VehicleFit.h"

namespace
{
	/**
	 * How far from a stranded aircraft the node its stand search starts from may be, uu. The search
	 * only CHOOSES the stand; the route driven is RescueStranded's, from the pavement it hops onto -
	 * so this is generous (50 m) rather than tight: too tight and a stranded aircraft finds no stand
	 * at all, too loose and the choice favours a stand near some other node. Neither moves anything.
	 * (AirportOps' UAgentRescue's figure until #429 moved the choice here with its reason.)
	 */
	constexpr double StandSearchRadius = 5000.0;

	/**
	 * ONE SEARCH FROM Start ON THE CALLER'S TEMPLATE - and, when the vehicle gate refuses it as too narrow and the
	 * caller allows it (ENarrowRoad::DriveAnyway), the same search UNGATED. Said ONCE per SendAgentTo in Result, as
	 * DriveVehicleTo said it once per recall and not once per node tried. bOutUngated tells the caller the plan it got
	 * was never gated, so a caller from rest can judge the tow on it (VehicleFit::MayDriveUngated).
	 */
	FRoutePlan SearchFrom(const URoadNetwork& Network, FRouteQuery Query, FGuidelineNodeId Start, ENarrowRoad Narrow,
		FSendAgentResult& Result, bool& bOutUngated)
	{
		Query.Start = Start;
		FRoutePlan Plan = RouteSearch::Find(Network, Query);
		bOutUngated = false;
		if (Plan.Result == ERouteResult::TooNarrow && Narrow == ENarrowRoad::DriveAnyway)
		{
			if (!Result.bNarrow)
			{
				Result.bNarrow = true;
				Result.NarrowWhy = Plan.RejectedBy.Describe();
				Result.NarrowEdge = Plan.RejectedEdge;
			}
			Query.Vehicle = nullptr;
			Plan = RouteSearch::Find(Network, Query);
			bOutUngated = true;
		}
		return Plan;
	}

	/**
	 * A VEHICLE ON THE ROAD TURNS WHERE IT IS (final review, 2026-09-27): its aircraft left, or was deleted, before
	 * it got there - or it is being sent on to a different job. The search used to start from its GOAL regardless -
	 * the service point, where the route away opens with the bay's reverse leg - and RedirectAgent then started the
	 * truck on that route's first point: a rigid truck jumped to the hydrant, and the tow's reverse was solved from a
	 * cab still out on the road (ReverseUnsolvable, "retired where it stands").
	 *
	 * SO IT TURNS WHERE IT IS: the route is searched from the node at the END of the step it is driving - ReplanAt's
	 * splice point, ahead of it - and spliced on through RerouteAgent, which keeps it moving and judges a tow's whole
	 * new route from the live chain. The tail is searched UNSEEDED, as FPlanReResolver::SpliceReplan's is: it starts
	 * where the chain is not yet. When that turn does not hold - a tow arriving at a junction cannot always take a
	 * hard turn back (measured 2026-09-27: a 90 degree fold at the far road's junction) - the next node on is tried,
	 * and so on up the route: each is one search, once per recall, at most one per step left - 15 steps depot to
	 * hydrant on the fuel fixture's A and C stands (measured 2026-09-27, AirportOps.Fuel.*RecalledMidRouteGetsHome's
	 * "recalled on step" line); the tow took 5.
	 *
	 * ON ITS LAST STEP, OR WHEN NO TURN HOLDS, IT FINISHES THE LEG and turns from the service point, the normal
	 * cycle's own path (the caller's, on its arrival): the last step ends AT the service point, so a turn from its end
	 * is the route onward from there anyway, and only the chain that arrives can solve its reverse. Better the rest
	 * of the way out than a retirement.
	 *
	 * A TOO-NARROW TAIL IS SEARCHED AGAIN UNGATED when the caller allows it, and RerouteAgent's own whole-route judge
	 * is what keeps a fold off the road.
	 * ENFORCED BY: AirportOps.Fuel.TowRecalledMidRouteGetsHome, AirportOps.Fuel.TruckRecalledMidRouteGetsHome,
	 * AirportOps.Fuel.TowRecalledOnItsLastLegGetsHome
	 */
	FSendAgentResult TurnOnTheRoad(UGroundTraffic& Traffic, const FRoadAgent& Agent, FGuidelineNodeId Goal,
		const FRouteQuery& Template, const URoadNetwork& Network, ENarrowRoad Narrow)
	{
		FSendAgentResult Result;
		const int32 AgentId = Agent.Id;
		// COPIED, because a successful RerouteAgent replaces the plan this would otherwise alias - and Agent with it.
		const FRoutePlan Out = Agent.Follower.Plan;
		const FGuidelineNodeId OutGoal = Agent.GoalNode;
		const int32 FirstKeep = UGroundTraffic::CurrentStep(Out, Agent.Follower.Travelled) + 1;

		FRouteQuery Query = Template;
		Query.Goal = Goal;
		Query.TowSeed.Reset();
		for (int32 KeepSteps = FirstKeep; KeepSteps < Out.Steps.Num(); ++KeepSteps)
		{
			const FGuidelineNodeId TurnAt = UGroundTraffic::StepFromNode(Out, KeepSteps);
			if (TurnAt == OutGoal)
			{
				break;
			}
			bool bUngated = false;
			const FRoutePlan Tail = SearchFrom(Network, Query, TurnAt, Narrow, Result, bUngated);
			if (Tail.IsValid() && Traffic.RerouteAgent(AgentId, &Network, KeepSteps, Tail))
			{
				Result.Outcome = ESendOutcome::Turned;
				Result.TurnAt = TurnAt;
				return Result;
			}
		}
		// STILL ITS LEG: it arrives at the old goal, and the caller sees a goal that is not the one it wanted and
		// sends it on from there, parked.
		Result.Outcome = ESendOutcome::FinishesLeg;
		return Result;
	}

	/**
	 * A MOVING AIRCRAFT IS EXTENDED IN PLACE from where its route ends - a stand re-offer's taxi-in (issue #435):
	 * RedirectAgent restarts an aircraft from REST at the new route's first point, which for a moving one is the far
	 * end of the route ahead of it - a teleport - and RerouteAgent refuses aircraft (their runway and departure
	 * arming are RedirectAgent's and ReplanAt's). ExtendRoute keeps Travelled, speed and heading, and its precondition
	 * - the tail starts where the live plan ends - is its GOAL NODE, which is where this searches from; a tail that
	 * does not join is refused there, and the aircraft finishes the leg it is on.
	 *
	 * NEVER RedirectAgent AS THE FALLBACK when the extension is refused (a tail that does not join, a plan that died
	 * between the search and here): that is the teleport again, in the one case nothing has measured. It keeps
	 * going, and the caller asks again - for a stand re-offer, the next freed stand, or its own stop at the end of the
	 * route (AdvanceOnce's Parked case, #455), which is what asks it if no stand frees.
	 * ENFORCED BY: Airside.Model.Traffic.ReofferTaxiingWaiterDoesNotJump, Airside.Model.Traffic.ReofferRefusedExtensionKeepsWaiting
	 */
	FSendAgentResult ExtendToGoal(UGroundTraffic& Traffic, const FRoadAgent& Agent, FGuidelineNodeId Goal,
		const FRouteQuery& Template, const URoadNetwork& Network, ENarrowRoad Narrow)
	{
		FSendAgentResult Result;
		const int32 AgentId = Agent.Id;
		const FGuidelineNodeId From = Agent.GoalNode;
		FRouteQuery Query = Template;
		Query.Goal = Goal;
		Query.TowSeed.Reset();
		bool bUngated = false;
		const FRoutePlan Tail = SearchFrom(Network, Query, From, Narrow, Result, bUngated);
		if (Tail.IsValid() && Traffic.ExtendRoute(AgentId, &Network, Tail))
		{
			Result.Outcome = ESendOutcome::Turned;
			Result.TurnAt = From;
			return Result;
		}
		Result.Outcome = ESendOutcome::FinishesLeg;
		return Result;
	}

	/**
	 * FROM REST, WHERE IT STANDS: its goal node - the service point or stand it is parked on, or the node a stranded
	 * waiter's wait began at - and a redirect, which keeps the agent's id and view (RedirectAgent accepts Parked and
	 * Stranded: the handover its header describes a service composing).
	 *
	 * JUDGED FROM THE LIVE CHAIN AND CAB (2026-09-27): the route away from a stand OPENS with the bay's reverse leg,
	 * and VehicleFit::JudgePlan solves that reverse from where the tow is parked - its axles, its heading and its cab
	 * (FTowSeed::Origin), the pose FTowReverseRun will arm from - so a route the router admits is one the tow can back
	 * along. Unseeded, JudgePlan refuses a reverse-first plan outright. THE SAME FOR A JOB LEG as for the way home: a
	 * vehicle chained stand to stand backs off the first stand exactly as it would to go home.
	 * ENFORCED BY: AirportOps.Fuel.TowServesCodeB (the tow gets home)
	 *
	 * FRoadAgent::LiveTowSeedAtRest BUILDS THE SEED (issue #429, #313) - the axles, the cab's pose, at rest, and HOW
	 * FAR ALONG THE ROUTE THE STEERED AXLE ALREADY IS: zero at a service point, where it parked on the node the route
	 * starts from. Unset for anything with no trailer, as the old guard was; and for a folded tow, which a Parked
	 * truck never is (a fold holds its agent where it folded, short of any arrival).
	 *
	 * AN UNGATED ROUTE (ENarrowRoad::DriveAnyway) IS NEVER ONE THAT FOLDS ITS TRAILER (review of 9441ccf1). A body a
	 * little wide for a corner scuffs a kerb; a trailer past square is a jack-knife, and the agent stops dead where it
	 * folds, holding the road. So a TOW's ungated route is judged whole (VehicleFit::MayDriveUngated, the router's own
	 * check) and, if it folds, not driven: NoRoute, with the fold named for the caller.
	 * ENFORCED BY: AirportOps.Ops.FuelTowNeverDrivenHomeIntoAFold, AirportOps.Ops.FuelTruckGetsHomeWhenTooNarrow
	 */
	FSendAgentResult SendFromRest(UGroundTraffic& Traffic, const FRoadAgent& Agent, FGuidelineNodeId Goal,
		const FRouteQuery& Template, const URoadNetwork& Network, ENarrowRoad Narrow, EAgentEvent Cause)
	{
		FSendAgentResult Result;
		const int32 AgentId = Agent.Id;
		const FGuidelineNodeId From = Agent.GoalNode;
		FRouteQuery Query = Template;
		Query.Goal = Goal;
		Query.TowSeed = Agent.LiveTowSeedAtRest(Network.GetGuidelineNode(From));
		bool bUngated = false;
		FRoutePlan Plan = SearchFrom(Network, Query, From, Narrow, Result, bUngated);
		FString Why;
		if (bUngated && Plan.IsValid() && Template.Vehicle != nullptr
			&& !VehicleFit::MayDriveUngated(Plan, *Template.Vehicle, Network, &Why, Query.TowSeed.GetPtrOrNull()))
		{
			Result.FoldWhy = Why;
			Plan = FRoutePlan();
		}
		// Agent IS NOT READ AFTER THIS: RedirectAgent announces the phase change, and a listener may retire the agent
		// and shift the array the reference points into (the re-entrancy contract AdvanceOnce states).
		Result.Outcome = Plan.IsValid() && Traffic.RedirectAgent(AgentId, &Network, Plan, Cause)
			? ESendOutcome::Redirected : ESendOutcome::NoRoute;
		return Result;
	}
}

// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error over this function - a phase added to EAgentPhase is a
// build error here until it has a row)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
FSendAgentResult UGroundTraffic::SendAgentTo(int32 AgentId, FGuidelineNodeId Goal, const FRouteQuery& QueryTemplate,
	const URoadNetwork& Network, ENarrowRoad Narrow, EAgentEvent Cause)
{
	FSendAgentResult Result;
	const FRoadAgent* Agent = FindAgent(AgentId);
	if (Agent == nullptr)
	{
		return Result;
	}

	// ONE ROW PER PHASE, AND EVERY PHASE HAS ONE - see the declaration for the table and why each row is the verb it is.
	switch (Agent->Phase)
	{
	case EAgentPhase::Taxiing:
		if (!Agent->Follower.Plan.IsValid())
		{
			// A TAXI WITH NO DRIVABLE ROUTE is finishing whatever it is doing: the vehicle's old "any other motion"
			// arm, which this state fell into (DriveVehicleTo turned on the road only when its plan was valid).
			Result.Outcome = ESendOutcome::FinishesMotion;
			return Result;
		}
		if (Agent->GoalNode == Goal)
		{
			// ALREADY GOING THERE (a job re-bid back onto the leg it is on): nothing to turn.
			Result.Outcome = ESendOutcome::AlreadyGoing;
			return Result;
		}
		return Agent->AsVehicle() != nullptr ? TurnOnTheRoad(*this, *Agent, Goal, QueryTemplate, Network, Narrow)
			: ExtendToGoal(*this, *Agent, Goal, QueryTemplate, Network, Narrow);

	case EAgentPhase::Parked:
		return SendFromRest(*this, *Agent, Goal, QueryTemplate, Network, Narrow, Cause);

	case EAgentPhase::Stranded:
		// A STRANDED WAITER stands where its wait began - a stranded taxi-in waits at its exit for a stand (#396), and
		// its goal node is that exit - so a stand re-offer restarts it there, from rest, as it would a parked one.
		// ANY OTHER STRANDED AGENT is somewhere along a route that died under it, with no leg to finish: it would
		// "turn where it parks" never. It is rescued onto pavement toward Goal, or it is the caller's to decide what a
		// vehicle nothing can move is (2026-09-29).
		// ENFORCED BY: AirportOps.Model.AgentRescue.StrandedVehicleReleasesJobs
		if (Agent->bAwaitingStand)
		{
			return SendFromRest(*this, *Agent, Goal, QueryTemplate, Network, Narrow, Cause);
		}
		Result.Outcome = RescueStranded(AgentId, Network, Goal) ? ESendOutcome::Rescued : ESendOutcome::NoPavement;
		return Result;

	case EAgentPhase::Reversing:
	case EAgentPhase::Manoeuvring:
		// REVERSING (or any other motion that is not the road and not parked): RedirectAgent refuses it, so a
		// redirect from where it stands would fail and its caller retire it where it stood (final review #3,
		// 2026-09-28; the old SendTruckHome had the same Taxiing-only test). It FINISHES THE MOTION - backing off one
		// stand at the start of a chained leg, into a bay at the end of one - and the caller sends it on from where it
		// parks, exactly as the last-leg case above does.
		// ENFORCED BY: AirportOps.Fuel.TowRecalledWhileReversingGetsHome
		Result.Outcome = ESendOutcome::FinishesMotion;
		return Result;

	case EAgentPhase::Arriving:
	case EAgentPhase::Departing:
	case EAgentPhase::Gone:
		// ON A RUNWAY, OR GONE: the landing or the take-off owns it, and nothing a caller wants moves it first.
		return Result;
	}
	return Result;
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error: a new send outcome must say whether it placed the waiter)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
FStandOffer UGroundTraffic::ReofferStand(int32 AgentId, const URoadNetwork& Network, EAgentEvent Cause)
{
	FStandOffer Offer;
	const FRoadAgent* Agent = FindAgent(AgentId);
	if (Agent == nullptr || Agent->AsAircraft() == nullptr)
	{
		return Offer;
	}
	// COPIED, NOT HELD: SendAgentTo can announce a phase change (a redirect), and a listener may retire the agent and
	// shift the array the pointer is into.
	const FAirframe Airframe = *Agent->AsAircraft();
	const FGuidelineNodeId From = Agent->GoalNode;

	// THE STAND IS CHOSEN FROM ITS GOAL NODE - where a parked waiter stands, where a moving one's route ends - which
	// is where SendAgentTo will start its route from, whichever verb its phase takes.
	Offer.Stand = ArrivalPlanner::ChooseStand(Network, From, Airframe, &Occupancy, AgentId);
	if (!Offer.Stand.IsSet())
	{
		Offer.Outcome = EStandOffer::NoFreeStand;
		return Offer;
	}

	// THE TAXI-IN'S OWN QUERY - the errand, the span and the pavement ChooseStand searched with - so the route driven is
	// the one the stand was chosen by. Searched again by SendAgentTo rather than taken from ChooseStand's multi-goal
	// search: this runs only when a stand may have freed (or the player asks), and one Find per waiter then is the
	// price of one table choosing the verb.
	FRouteQuery TaxiIn = FRouteQuery::For(ERouteErrand::ArrivalTaxiIn, From, Offer.Stand, Airframe.Wingspan,
		ETraversalClass::Aircraft);
	TaxiIn.NeedsPavement(Airframe.MinimumPavement);
	Offer.Send = SendAgentTo(AgentId, Offer.Stand, TaxiIn, Network, ENarrowRoad::Refuse, Cause);

	switch (Offer.Send.Outcome)
	{
	case ESendOutcome::Turned:
	case ESendOutcome::Redirected:
	case ESendOutcome::Rescued:
		Offer.Outcome = EStandOffer::Sent;
		break;
	case ESendOutcome::AlreadyGoing:
	case ESendOutcome::FinishesLeg:
	case ESendOutcome::FinishesMotion:
	case ESendOutcome::NoRoute:
	case ESendOutcome::NoPavement:
	case ESendOutcome::NotSendable:
		Offer.Outcome = EStandOffer::NotSent;
		break;
	}
	return Offer;
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

FStandOffer UGroundTraffic::RescueToStand(int32 AgentId, const URoadNetwork& Network)
{
	FStandOffer Offer;
	const FRoadAgent* Agent = FindAgent(AgentId);
	if (Agent == nullptr || Agent->AsAircraft() == nullptr)
	{
		return Offer;
	}
	const FAirframe Airframe = *Agent->AsAircraft();

	// STRANDED: the stand is CHOSEN from a node near it, and DRIVEN to by the rescue from wherever it hops onto. THE
	// NODE AHEAD ON ITS OWN ROUTE when that still exists - the way it was facing, off any runway it was leaving - and
	// only else the nearest node: measured 2026-09-29, an aeroplane stranded just off a runway exit found the runway's
	// own node nearest (198 uu), and a taxi-in search avoids runway edges, so no stand was reachable from there at all.
	// See StandSearchRadius.
	const FRoutePlan& Plan = Agent->Follower.Plan;
	const int32 OnStep = CurrentStep(Plan, Agent->Follower.Travelled);
	FGuidelineNodeId Near = Plan.Steps.IsValidIndex(OnStep) && Network.GetGuidelineNode(Plan.Steps[OnStep].To) != nullptr
		? Plan.Steps[OnStep].To : FGuidelineNodeId();
	if (!Near.IsSet())
	{
		Near = RouteSearch::FindNearestNode(Network, Agent->LastMotion.Position, Agent->Class, StandSearchRadius);
	}
	if (!Near.IsSet())
	{
		Offer.Outcome = EStandOffer::NoPavement;
		return Offer;
	}
	bool bSawHeld = false;
	Offer.Stand = ArrivalPlanner::ChooseStand(Network, Near, Airframe, &Occupancy, AgentId, nullptr, &bSawHeld);
	if (!Offer.Stand.IsSet())
	{
		// WHICH NODE IT SEARCHED FROM, said: "no stand" from a stranded aeroplane is either the airport (every stand
		// held or too small) or the search origin, and only the log can tell them apart. (The Unstick's own line,
		// "Unstick: agent N ... refused: No free stand fits it", follows it from AirportOps.)
		const FGuidelineNode* FromNode = Network.GetGuidelineNode(Near);
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d found no stand searching from node %d, %.0f uu away (a held stand %s)"),
			AgentId, Near.Index, FromNode != nullptr ? FVector2D::Distance(FromNode->Position, Agent->LastMotion.Position) : -1.0,
			bSawHeld ? TEXT("was seen") : TEXT("was not seen"));
		Offer.Outcome = EStandOffer::NoFreeStand;
		return Offer;
	}

	// ALWAYS THE RESCUE, NEVER SendAgentTo's STRANDED-WAITER ROW: an aeroplane is stranded awaiting a stand when the
	// stand it was going to went with the taxiway under it, so its goal node may be the stand that is gone - a restart
	// from there would put it back where nothing is. RescueStranded hops it onto pavement where it actually stands.
	const bool bRescued = RescueStranded(AgentId, Network, Offer.Stand);
	Offer.Send.Outcome = bRescued ? ESendOutcome::Rescued : ESendOutcome::NoPavement;
	Offer.Outcome = bRescued ? EStandOffer::Sent : EStandOffer::NoPavement;
	return Offer;
}

double UGroundTraffic::RemainingDriveSeconds(int32 AgentId) const
{
	const FRoadAgent* Agent = FindAgent(AgentId);
	if (Agent == nullptr || !Agent->Follower.Plan.IsValid())
	{
		return 0.0;
	}
	// ITS OWN CRUISE, not a catalogue's: a vehicle already out is that vehicle until it is home, whatever its kind's
	// table says now (UJobBoard::DriveVehicleTo's rule for the same reason). The bid read its kind's SpeedCap, the same
	// figure for every vehicle dispatched from it.
	const double Left = FMath::Max(Agent->Follower.Plan.Length - Agent->Follower.Travelled, 0.0);
	return Left / FMath::Max(Agent->Chassis().Ground.Taxi.SpeedCap, 1.0);
}
