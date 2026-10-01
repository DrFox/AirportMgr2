// The ONE retry pass for every stopped waiter (FRoadAgent::GetWait) and its two arms: RetryWaiters, ReplanHeldTaxiOut and
// ReofferWaiter - issue #444.
//
// A SIXTH TRANSLATION UNIT rather than more of GroundTraffic.cpp (see GroundTraffic.h's file map, and Check-Architecture
// rule 77's line budget, which GroundTraffic.cpp met when #444 merged its two retry loops into one pass): the retry is
// one responsibility - who is waiting, for what, and the one move each wait is retried with - and reads the class only
// through the operations GroundTraffic.cpp owns (ChangeRoute, ReleaseGoal, TakeGoal) and GroundTrafficSend.cpp's
// ReofferStand. AdvanceOnce calls it, last in each step.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/DeparturePlanner.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteChange.h"
#include "Model/RouteSearch.h"
#include "Model/SendAgent.h"

// EVERY WAIT NAMED in the switch below: a wait added to EAgentWait is a build error here until it has an arm.
// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error over this function)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
void UGroundTraffic::RetryWaiters(const URoadNetwork& Network)
{
	// ONE SCAN, ONE SWITCH ON THE WAIT (issue #444): two loops used to scan every agent for a flag each. BY ID, collected
	// before any arm runs - a stand's SendAgentTo can announce, and a listener may retire any agent and shift the array.
	// ENFORCED BY: Airside.Model.Traffic.ReofferStandsRetireReentrancy
	TArray<int32, TInlineAllocator<8>> TaxiOutHolds;
	TArray<int32, TInlineAllocator<8>> StandWaiters;
	for (const FRoadAgent& Agent : Agents)
	{
		switch (Agent.GetWait())
		{
		case EAgentWait::None:
			break;
		case EAgentWait::ForTaxiOutRoute:
			TaxiOutHolds.Add(Agent.Id);
			break;
		case EAgentWait::ForStand:
			StandWaiters.Add(Agent.Id);
			break;
		}
	}

	// THE TAXI-OUT HOLDS FIRST, for parity with the two passes this replaced: the taxi-out replan ran at the top of the
	// step and the stand re-offer at its end, so a goal a taxi-out restart let go (ChangeRoute's ReleaseGoal raises
	// bStandsMayHaveFreed) was offered to the stand waiters in the SAME step. Taxi-out first keeps that: the stand arm
	// below reads the flag after it. The other order would leave the release raised past the clear below, and it would
	// be read a step later, not lost. Not while the graph is mid-edit: the route must be on the lines the player is
	// about to see.
	if (TaxiOutHolds.Num() > 0 && !Network.AreGuidelinesBehindRoad())
	{
		// A NEW NETWORK OBJECT forgets every refusal: its revisions count from its own zero, and one could match an
		// old refusal's number by coincidence.
		// ENFORCED BY: Airside.Model.Traffic.HeldTaxiOut.ANewNetworkAsksAgain
		if (TaxiOutGateNetwork.Get() != &Network)
		{
			for (FRoadAgent& Each : Agents)
			{
				Each.ForgetWaitRefusal();
			}
			TaxiOutGateNetwork = &Network;
		}
		const uint32 GraphNow = Network.GetGuidelineRevision();
		for (const int32 Id : TaxiOutHolds)
		{
			ReplanHeldTaxiOut(Id, Network, GraphNow);
		}
	}

	// THE STANDS, when one may have freed. ONE PASS, then the flag clears whether or not anyone was placed: a waiter
	// that still found nothing will be asked again the next time something frees, not every frame.
	if (!bStandsMayHaveFreed)
	{
		return;
	}
	bStandsMayHaveFreed = false;
	for (const int32 Id : StandWaiters)
	{
		ReofferWaiter(Id, Network);
	}
	// RedirectAgent re-raised the flag on releasing the old goal; nothing else has changed
	// since this pass started, so it is cleared again rather than costing an empty pass.
	bStandsMayHaveFreed = false;
}
AIRSIDE_EXHAUSTIVE_SWITCH_END

void UGroundTraffic::ReplanHeldTaxiOut(int32 AgentId, const URoadNetwork& Network, uint32 GraphNow)
{
	// HOW FAR THE JOIN LEG MAY REACH, uu: 30 m. It is driven in a straight line over whatever
	// lies between, so it is for a push that ended a few metres off a line the player moved,
	// not for an aeroplane the edit left in a field - that one holds until a line reaches it.
	constexpr double TaxiOutJoinRadiusUu = 3000.0;

	// ASKED ONCE PER GRAPH, NOT EVERY SUBSTEP (ops batch 3 PR E): a refused hold remembers the guideline revision it
	// was refused at (FRoadAgent::WasWaitRefusedAt) and is asked again only once the graph has moved past it. It was
	// FindNearestNode + PlanAny every substep for as long as it held - minutes, if the player never fixed it.
	//
	// THE GRAPH ALONE, and why that is every input of the refusal (checked 2026-09-30): the nearest node and the
	// route are the guideline graph and the agent's pose, which does not move while it holds; PlanAny's runways,
	// their modes and in-use ends are runway facts, which in play change only through the facade, whose Topology
	// rebuild re-makes every derived edge and so moves GetGuidelineRevision; a drag leaves the guidelines behind the
	// road, and RetryWaiters skips this arm until it is dropped. NOT OCCUPANCY - the spec keyed it on a runway-freed count too, on
	// the premise that PlanAny refuses behind a busy runway; it does not: occupancy only RANKS a held runway below a
	// free one, and both departure errands are EOccupancyUse::Never. A key input that cannot change the answer would
	// only buy retries that fail.
	// ENFORCED BY: Airside.Model.Traffic.HeldTaxiOut.BusyRunwayIsNoRefusal (a held strip is still planned to);
	// AirportMgr.Inspector.Cache.RunwaySeesItsFacts (a facts change through the facade moves the guideline revision);
	// Check-Architecture rule 35 (facts-through-facade: nothing outside the facade and Testing/ writes a runway's facts)
	// (The network the revision belongs to is RetryWaiters' gate, above: a new network forgets every refusal.)
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return;
	}
	FRoadAgent& Agent = Agents[Index];
	// WAITING AND STOPPED WHERE THE WAIT APPLIES - a taxi out marked stale part way through a push is planned once the
	// push has ended, from where it ended.
	if (!Agent.IsHoldingForTaxiOut())
	{
		return;
	}
	const FAirframe* Aircraft = Agent.AsAircraft();
	if (Aircraft == nullptr)
	{
		return;
	}
	if (Agent.WasWaitRefusedAt(GraphNow))
	{
		return;
	}
	++TaxiOutReplanAttempts;   // See TaxiOutReplanAttemptsForTest.
	const FVector2D Here = Agent.LastMotion.Position;
	const FGuidelineNodeId From = RouteSearch::FindNearestNode(Network, Here, Agent.Class, TaxiOutJoinRadiusUu);
	FDeparturePlan Plan;
	if (From.IsSet())
	{
		// Occupancy, DepartAgent's reason. This agent holds no runway yet - it is holding
		// for taxi-out - so its own claims cannot make a strip look busy to itself.
		Plan = DeparturePlanner::PlanAny(Network, From, *Aircraft, Agent.Class, &Occupancy);
	}
	if (!From.IsSet() || !Plan.IsValid() || Plan.Route.Polyline.Num() == 0)
	{
		if (!Agent.HasSaidWait())
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d holds where its push ended: %s"), Agent.Id,
				From.IsSet() ? *DeparturePlanner::Describe(Plan) : TEXT("no taxi line within 30 m"));
			Agent.MarkWaitSaid();
		}
		Agent.MarkWaitRefusedAt(GraphNow);
		return;
	}

	// THE JOIN: the route starts at a node; the aeroplane is where its push ended. A leg
	// from here to that node is prepended so the follower drives it - the handover checks
	// the route begins HERE, and a route that began at the node would be a jump.
	FRoutePlan Route = Plan.Route;
	const double Leg = FVector2D::Distance(Here, Route.Polyline[0]);
	if (Leg > 1.0)
	{
		Route.Polyline.Insert(Here, 0);
		for (FRouteStep& Step : Route.Steps)
		{
			++Step.EndVertex;
			Step.EndDistance += Leg;
		}
		Route.Length += Leg;
	}
	// AT THE END OF A PUSH the handover starts it; A TAXIING aeroplane that held where a
	// stranded route left it restarts on it now, clear of the claims and queue position
	// the dead route held - the drive-side rejoin's same three releases.
	//
	// THE RESTART IS A ROUTE CHANGE (issue #429) and goes through the seam: from rest where it
	// holds, facing the way it holds (KeptHeading), its engine carried on, the goal moved with
	// its claim. ReservationsAndGuidelines, as before - unlike RedirectAgent's restart, which
	// keeps what the agent stands on: the rejoin's release was this site's stated reason, and a
	// change to it would be a behaviour change this refactor does not make (see the PR's table).
	// Traced, not measured: a hold on a STRANDED route holds no guideline claim by the time this
	// runs (the claim pass's dead-plan branch gave them back), so there the choice changes nothing;
	// a hold at the end of a TRUNCATED taxi out (the taxi-complete guard's "ended ... from its
	// runway entry") does hold the ground it stands on, and gives it up until the next claim pass.
	// The push's end changes no follower - AdoptTaxiOut only stores the route the handover
	// starts - so it keeps its own goal move, as DepartAgent's push does.
	// ENFORCED BY: Airside.Model.Traffic.HeldTaxiOut.RestartKeepsPoseAndEngine
	if (Agent.Phase == EAgentPhase::Manoeuvring)
	{
		Agent.AdoptTaxiOut(Route);
		// THE GOAL AND THE ARMING, as DepartAgent takes them - ReleaseGoal then TakeGoal.
		ReleaseGoal(Agent, Agent.Id);
		TakeGoal(Agent, Agent.Id, &Network, Route);
	}
	else
	{
		Agent.EndWait();
		ChangeRoute(Agent, FRouteChange::Restart(Route, ERouteRelease::ReservationsAndGuidelines, 0.0,
			Agent.LastMotion.Heading), &Network);
	}
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: way to the runway planned again from where it %s (%.0f uu join): %s"),
		Agent.Id, Agent.Phase == EAgentPhase::Manoeuvring ? TEXT("was pushed back to") : TEXT("held on the taxiway"),
		Leg, *DeparturePlanner::Describe(Plan));
}

void UGroundTraffic::ReofferWaiter(int32 Id, const URoadNetwork& Network)
{
	// A WAITER WITH A NODE TO SEARCH FROM, IN A PHASE A ROUTE CAN BE GIVEN TO - IsRedirectable, RedirectAgent's own
	// column (issue #444: the three phases were spelled here as well as there). Found again by id: an earlier waiter's
	// redirect may have announced, and a listener retired this one.
	const FRoadAgent* Agent = FindAgent(Id);
	if (Agent == nullptr || !Agent->IsWaitingFor(EAgentWait::ForStand) || !Agent->GoalNode.IsSet() || !Agent->IsRedirectable())
	{
		return;
	}
	// THE PHASE CHOOSES THE VERB (issue #435), and SendAgentTo is where it chooses since #429 - reached through
	// ReofferStand, the one waiter's move the player's Unstick makes too. The route to the stand starts at the
	// waiter's GoalNode, which for a TAXIING waiter is where its truncated route ENDS - a node ahead of the
	// aircraft. Handed to RedirectAgent that was a restart from rest at the new route's first point: the aircraft
	// jumped to the end of the route it had not driven (9271 uu on the first frame, measured 2026-09-30 on
	// Airside.Model.Traffic.ReofferTaxiingWaiterDoesNotJump). ExtendRoute splices the tail onto the live plan with
	// Travelled, Speed and Heading kept, and its precondition - the tail starts where the live plan ends - is the
	// very thing that made the route. A PARKED waiter is standing at GoalNode, so the restart is where it already is,
	// and RedirectAgent is the only verb that gets a stopped aircraft going: it keeps it. A STRANDED one is not
	// assumed to be at GoalNode - a second rebuild can strand it short of the node its first truncation left it - so
	// unless it is measured there (#396's exit) it is offered its stand from where it stands and rescued there (#429
	// review; ReofferStrandedWaiterDoesNotJump, ReofferWaiterStrandedAtItsExit).
	//
	// ReOffered, NOT Redirected: the flight board keeps it in its taxi IN whatever its stand does next (review M1).
	const FStandOffer Offer = ReofferStand(Id, Network, EAgentEvent::ReOffered);

	// A MOVING WAITER WHOSE EXTENSION WAS REFUSED (a tail that does not join, a plan that died between the search and
	// here) is never redirected instead - that is the teleport again, in the one case nothing has measured. It keeps
	// waiting, and the next freed stand asks again - or its own stop at the end of the route does (AdvanceOnce's
	// Parked case, #455), which is what asks it if no stand frees.
	// FinishesMotion TOO: a taxiing waiter whose route is not drivable any more keeps waiting as well, and said so
	// before #429 (ExtendRoute refused it with this line); SendAgentTo answers it FinishesMotion now.
	if (Offer.Outcome == EStandOffer::NotSent
		&& (Offer.Send.Outcome == ESendOutcome::FinishesLeg || Offer.Send.Outcome == ESendOutcome::FinishesMotion))
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d: a stand freed at node %d, but its route could not be extended from where it ends; it keeps waiting"),
			Id, Offer.Stand.Index);
	}
	if (Offer.Outcome == EStandOffer::Sent)
	{
		// NOT Agents[FindIndex(Id)] HERE (issue #193): RedirectAgent already ended
		// the stand wait itself, before its own OnAgentPhaseChanged broadcast - see its
		// comment. (ExtendRoute clears it through the same ReleaseGoal and broadcasts nothing.)
		// Re-deriving the index AFTER that call is exactly the bug this fix
		// removes: a synchronous listener on that broadcast may call RetireAgent and remove Id
		// from Agents (the contract AdvanceOnce states; Airside.Model.Traffic.
		// ReofferStandsRetireReentrancy is one), so FindIndex(Id) here would
		// return INDEX_NONE and Agents[INDEX_NONE] would be an out-of-bounds write. Id and
		// Offer.Stand.Index are plain values, not indices into Agents, so the log below is safe
		// whether or not the agent survived its own redirect.
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: a stand freed; sent to the stand at node %d"), Id, Offer.Stand.Index);
	}
}
