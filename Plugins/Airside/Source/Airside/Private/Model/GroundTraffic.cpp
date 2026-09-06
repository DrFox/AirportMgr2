#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/RoadNetwork.h"
#include "Solve/RunwayDesignator.h"

double FTrafficRules::FootprintFor(ETraversalClass Class) const
{
	// Pedestrians and emergency vehicles take the vehicle figures: nothing authored says
	// otherwise yet, and a fire truck is nearer a van than an aeroplane.
	return Class == ETraversalClass::Aircraft ? AircraftFootprint : VehicleFootprint;
}

double FTrafficRules::GapFor(ETraversalClass Class) const
{
	return Class == ETraversalClass::Aircraft ? AircraftGap : VehicleGap;
}

namespace
{
	/**
	 * One thing an agent wants this tick, plus what the REFUSAL rule needs to know about it.
	 *
	 * The step geometry travels with the claim rather than being re-derived at the refusal,
	 * because the refusal has to answer "how far may this agent go" in ROUTE distance while
	 * the claim itself is in EDGE distance, and the map between them (which step, how long,
	 * which way round) is exactly these fields. Re-deriving it from the blocker would mean a
	 * second reading of the same step - the thing this codebase calls a second evaluator.
	 */
	struct FWantedClaim
	{
		/**
		* Which of spec §3.1's routes to the runway surface raised this claim, when it was
		* one of them. An ENUM and not two bools: the two can never both be true, and the
		* refusal below has to pick exactly one stop-point rule from it - see the codebase's
		* "a phase is an enum, never a set of bools".
		*/
		enum class ESurface : uint8
		{
			/** Not a surface claim at all - an edge or a node. */
			None,
			/** The step's edge derives from a runway: stop a GAP short of the edge's start,
			*  outside the strip. */
			RunwayEdge,
			/** The step's end node carries a hold bar: stop with the NOSE on the bar. */
			HoldShort,
		};

		FTrafficClaim Claim;

		/** Index into Plan.Steps this claim was raised for. Becomes FRoadAgent::BlockedStep. */
		int32 Step = INDEX_NONE;

		/** True for the node at the END of Step; false for an edge or the node it left. */
		bool bEndNode = false;

		/** True when Step is shorter than Footprint + Gap AND the agent has not entered it -
		 *  the box-junction entry case, which stops the agent short of the box's START. */
		bool bBoxEntry = false;

		double StepStart = 0.0;
		double StepEnd = 0.0;
		double EdgeLength = 0.0;
		bool bReversed = false;

		ESurface Surface = ESurface::None;

		/** The node carrying the bar, for the log line. Set only when Surface == HoldShort;
		*  the segment it protects is already on Claim.Resource. */
		FGuidelineNodeId HoldNode;
	};
}

int32 UGroundTraffic::DispatchArrival(const URoadNetwork& Network, const FVector2D& Near,
	const FAirframe& Airframe, double ShutdownPauseSeconds)
{
	// WHICH RUNWAY, WHICH EXIT, WHICH STAND - none of that needs a world, so issue #29 moved
	// it to Model/ArrivalPlanner. This is left with arming and logging the plan. The table
	// is handed in so a runway someone holds is refused HERE, with its own reason, rather
	// than by an aircraft that lands through another one.
	const FArrivalPlan Plan = ArrivalPlanner::Plan(Network, Near, Airframe, &Occupancy);

	// Reported whether or not this succeeds, because a refusal that does not say which of
	// these was the problem is a feature that "does nothing". Skipped only for NoRunway,
	// which found no runway at all - every other field here is meaningless until one is.
	if (Plan.Why != EArrivalRefusal::NoRunway)
	{
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Arrival: runway %s, %.0f uu long, %.0f needed to stop, %d usable exit(s), ")
			TEXT("%d stand(s) on the airport."),
			*RunwayDesignator::ToPairText(Plan.Direction), Plan.RunwayLength, Plan.Needed,
			Plan.ExitCount, Network.GetEntities().Num());
	}

	if (!Plan.IsValid())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("%s"), *ArrivalPlanner::DescribeRefusal(Plan));
		OnArrivalRefused.Broadcast(Plan.Why);
		return 0;
	}

	FRoadAgent Agent;
	if (!Agent.StartArrival(Plan.Threshold, Plan.Direction, Plan.RunwayLength, Airframe, Plan.VacateAt, Plan.TaxiIn))
	{
		// FLandingRun has already logged why. Nothing is admitted: an arrival that cannot be
		// flown must leave no aircraft in the world, rather than one frozen on final.
		return 0;
	}

	// FRoadAgent is world-free and cannot read the actor's UPROPERTY, so the pause is copied
	// in here - the only time the two ever need to meet.
	Agent.ShutdownPause = ShutdownPauseSeconds;
	Agent.Class = ETraversalClass::Aircraft;
	Agent.GoalNode = Plan.TaxiIn.Steps.Num() > 0 ? Plan.TaxiIn.Steps.Last().To : FGuidelineNodeId();

	// THE RUNWAY IS HELD FROM NOW. Claimed as occupied every tick by Advance while the phase
	// is Arriving; released at the Vacated handover. Held on the agent rather than looked
	// up again at release, because by then the graph may have been rebuilt.
	Agent.RunwayHeld = Plan.RunwayChain;

	// NO ZERO-SECOND POSE HERE, unlike DispatchAgent: FRoadAgent::StartArrival already ran
	// one and wrote LastMotion from the approach's own starting pose - see its comment - so
	// a second would be the second evaluator this codebase keeps out of motion.

	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Arrival on runway %s: %.0f uu available, %.0f needed, vacating at exit %d of %d, ")
		TEXT("taxiing %.0f uu to a stand."),
		*RunwayDesignator::ToPairText(Plan.Direction), Plan.RunwayLength, Plan.Needed,
		Plan.ExitOrdinal, Plan.ExitCount, Plan.TaxiIn.Length);

	return Admit(MoveTemp(Agent));
}

int32 UGroundTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FAirframe& Airframe, ETraversalClass Class, double ShutdownPauseSeconds)
{
	if (!Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return 0;
	}

	// NO WORLD TEST HERE ANY MORE. This once refused outside a game world on the grounds
	// that an editor world would SAVE the cubes - but they are spawned RF_Transient, so they
	// were never going to be saved and the guard was protecting against nothing. What it DID
	// do was make the tool look broken in the one place the build tools are actually used:
	// the ed mode, where pressing 4 and clicking twice drew a route and produced no cube,
	// with nothing on screen to say why. The question stopped being askable here at all when
	// the agent list moved to Model/ - see UAirsideTraffic::SpawnView, which is where a
	// missing world is now a warning rather than a refusal.
	//
	// See ARoadNetworkActor::ShouldTickIfViewportsOnly: an actor does not tick in an editor
	// world unless it asks to, so allowing the spawn without that would have left a cube
	// frozen at its start - which is a worse lie than no cube at all.

	FRoadAgent Agent;
	Agent.StartTaxi(Plan, Airframe);

	// FRoadAgent is world-free and cannot read the actor's UPROPERTY for itself, so the
	// pause is copied in at dispatch - the only time the two ever need to meet.
	Agent.ShutdownPause = ShutdownPauseSeconds;
	Agent.Class = Class;
	Agent.GoalNode = Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : FGuidelineNodeId();

	ArmDepartureIfRunway(Agent, Network, Plan);

	// Posed before its first tick: a zero-second Advance asks where the taxi starts without
	// moving it, so LastMotion is a full pose - heading included - by the time the view is
	// spawned off the Admit broadcast below. StartTaxi's fallback covers a plan too short.
	FAgentMotion Motion;
	Agent.Advance(0.0, Motion);

	return Admit(MoveTemp(Agent));
}

void UGroundTraffic::ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const
{
	// CLEARED FIRST, and before every early return below. RedirectAgent sends an EXISTING
	// agent along a new plan: one that had been armed for a runway and is now sent to a
	// stand would otherwise keep the old chain and hold that runway against everybody for
	// the rest of the session.
	Agent.DepartureRunway.Reset();

	// DOES THIS ROUTE END ON A RUNWAY? Asked here rather than by the tool, because the answer
	// is a fact about the network and the last polyline point is the only thing that knows
	// where the route actually finished. A route that ends anywhere else simply taxis, which
	// is what every route did before departures existed.
	if (Network == nullptr || Plan.Polyline.Num() == 0 || !Agent.Airframe.Climb.IsSet())
	{
		return;
	}
	FVector2D Threshold;
	FVector2D Direction;
	double Length = 0.0;
	FRoadSegmentId Seed;
	if (Network->RunwayExtentAt(Plan.Polyline.Last(), Threshold, Direction, Length, &Seed))
	{
		Agent.ArmDeparture(Threshold, Direction, Length);

		// THE WHOLE CHAIN, not the seed segment: a runway is several segments by the time it
		// has exits, and a departure that held only the piece its taxi ended on would let a
		// second aircraft line up on the same strip further down. Recorded now rather than
		// looked up at the handover, because by then the graph may have been rebuilt.
		Agent.DepartureRunway = Network->RunwayChain(Seed);

		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Route ends on runway %s: %.0f uu available, departure armed"),
			*RunwayDesignator::ToPairText(Direction), Length);
	}
}

int32 UGroundTraffic::Admit(FRoadAgent&& Agent)
{
	Agent.Id = NextAgentId++;
	const EAgentPhase Born = Agent.Phase;
	const int32 Id = Agent.Id;
	Agents.Add(MoveTemp(Agent));

	// Broadcast AFTER the add, so a listener that spawns the view can find the agent it is
	// being told about - see UAirsideTraffic::SpawnView, which reads LastMotion off it.
	OnAgentPhaseChanged.Broadcast(Id, EAgentPhase::Gone, Born);
	return Id;
}

int32 UGroundTraffic::FindIndex(int32 AgentId) const
{
	return Agents.IndexOfByPredicate([AgentId](const FRoadAgent& A) { return A.Id == AgentId; });
}

const FRoadAgent* UGroundTraffic::FindAgent(int32 AgentId) const
{
	const int32 Index = FindIndex(AgentId);
	return Index == INDEX_NONE ? nullptr : &Agents[Index];
}

bool UGroundTraffic::RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || !Plan.IsValid() || Plan.Polyline.Num() < 2)
	{
		return false;
	}
	FRoadAgent& Agent = Agents[Index];
	const EAgentPhase Before = Agent.Phase;
	if (Before != EAgentPhase::Parked && Before != EAgentPhase::Taxiing)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RedirectAgent %d refused: agent is %s"),
			AgentId, *UEnum::GetValueAsString(Before));
		return false;
	}

	// The airframe is the agent's own - a redirect changes where it goes, not what it is.
	// Copied out first: StartTaxi assigns Airframe from its argument, and handing it a
	// reference to the very field it overwrites is a self-assignment nobody should rely on.
	const FAirframe Own = Agent.Airframe;
	Agent.StartTaxi(Plan, Own);

	// Class is NOT re-derived: a van redirected is still a van. StartTaxi rewrites the
	// follower and the airframe and nothing else, so the identity fields survive it; only
	// the goal moves, because that is the whole of what a redirect changes.
	Agent.GoalNode = Plan.Steps.Num() > 0 ? Plan.Steps.Last().To : FGuidelineNodeId();
	ArmDepartureIfRunway(Agent, Network, Plan);

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d redirected: %.0f uu"), AgentId, Plan.Length);
	if (Agent.Phase != Before)
	{
		OnAgentPhaseChanged.Broadcast(AgentId, Before, Agent.Phase);
	}
	return true;
}

bool UGroundTraffic::ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	FRoadAgent& Agent = Agents[Index];
	const FRoutePlan& Plan = Agent.Follower.Plan;

	// EVERY GUARD BEFORE ANYTHING IS WRITTEN, and that is the whole shape of this function:
	// the agent is not touched until both the search and the splice have succeeded, so a
	// refusal at any of them leaves it driving exactly the plan it had. An implementation
	// that replaced the follower first and repaired afterwards would leave a stuck agent
	// worse off than before it asked.
	if (Agent.Phase != EAgentPhase::Taxiing || !Plan.IsValid()
		|| SpliceStep < 0 || SpliceStep > Plan.Steps.Num())
	{
		return false;
	}

	// NEVER BEHIND THE AGENT. Travelled is preserved across the splice (that is the point of
	// Replace), so splicing at a step the agent has already driven past re-maps the same
	// route distance onto a DIFFERENT polyline - the agent would appear somewhere else on
	// the airport in one frame, which is the lateral teleport every "no jump" test exists to
	// catch. Refused rather than clamped: a caller asking to replan behind the agent has the
	// wrong node, and quietly moving its splice point would hide that.
	if (SpliceStep < CurrentStep(Plan, Agent.Follower.Travelled))
	{
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("ReplanAt %d refused: splice step %d is behind the agent, which is on step %d"),
			AgentId, SpliceStep, CurrentStep(Plan, Agent.Follower.Travelled));
		return false;
	}

	FRouteQuery Query;
	Query.Start = StepFromNode(Plan, SpliceStep);
	Query.Goal = Agent.GoalNode;
	Query.Class = Agent.Class;
	Query.Wingspan = Agent.Airframe.Wingspan;
	Query.BannedEdge = BannedEdge;

	// THE COST TERM IS THE POINT OF REPLANNING, not the ban. The ban removes the one edge
	// the caller knows is hopeless; the congestion cost is what stops the new route from
	// being the next queue along, which a plain shortest path would walk straight into.
	Query.Occupancy = &Occupancy;
	Query.QueryingAgent = AgentId;
	Query.CongestionWeight = Rules.CongestionWeight;

	const FRoutePlan Tail = RouteSearch::Find(Network, Query);
	if (!Tail.IsValid())
	{
		return false;
	}

	const FRoutePlan Spliced = RouteSearch::Splice(Plan, SpliceStep, Tail);
	if (!Spliced.IsValid())
	{
		return false;
	}

	// Read before Replace overwrites the plan under the reference, so the log line compares
	// the two journeys rather than one journey against itself.
	const double WasRemaining = Plan.Length - Agent.Follower.Travelled;

	// Replace, NOT Start: the line up to the splice is unchanged and the agent is part way
	// along it, so Travelled, Speed and Heading all survive. See FRouteFollower::Replace.
	Agent.Follower.Replace(Spliced);

	// THE RESERVATIONS, AND ONLY THOSE. They were made for a route that no longer exists past
	// the splice, so holding them would block the line the agent has just been re-routed away
	// from, for a journey nobody is making. What the agent is STANDING on is a different
	// thing entirely and survives: this was ReleaseAll, and a replanned aircraft standing on
	// a runway then showed the strip free to ArrivalPlanner for the frame before the next
	// Arbitrate - long enough to clear a landing onto it.
	//
	// THAT FRAME IS REAL AND IT IS SAFE. The resolver runs at the END of Advance, so the
	// agent holds no reservations until the next tick's claim pass, and in that window a
	// higher-ranked agent may take the line ahead of it. It is safe because the agent is by
	// construction STOPPED - it is a deadlocked waiter - and because the ground under it is
	// still claimed, so nobody can be granted a node or a strip its body is on. The rejected
	// alternative, re-claiming here, would be a second claim pass outside Arbitrate's order:
	// this agent would claim after everyone had moved, which is exactly the interleaving
	// Advance's header refuses.
	//
	// A stale OCCUPIED claim on an edge the new plan does not use is dropped by the next
	// ClaimAhead's ReleaseExcept, which keeps only what was asked for this pass.
	Occupancy.ReleaseReservations(AgentId);

	// The wait is over BY CONSTRUCTION - the thing it was waiting for is not on its route
	// any more - so the arbitration fields say so at once rather than a tick later. The
	// stall clock resets with them, or the deadlock pass that asked for this replan would
	// see the same stalled agent again on the very next tick and ask again.
	Agent.WaitingOn = 0;
	Agent.BlockedStep = INDEX_NONE;
	Agent.StalledSeconds = 0.0;

	// CrossingRunway IS DELIBERATELY LEFT ALONE. It says the agent's body is physically on a
	// strip, which is a fact about where the aeroplane IS, not about where it is going: a
	// replan cannot move it off the runway, and clearing it here would hand the strip back
	// with an aeroplane standing on it. ClaimAhead's geometric rule ends the crossing, and
	// it reads the SPLICED plan from the next tick on, which is the same line up to the
	// splice - so the tail-clear test it makes is the one it would have made anyway.

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d replanned at step %d: %.0f uu remaining -> %.0f"),
		AgentId, SpliceStep, WasRemaining, Spliced.Length - Agent.Follower.Travelled);
	return true;
}

bool UGroundTraffic::RetireAgent(int32 AgentId)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	const EAgentPhase Before = Agents[Index].Phase;
	Agents.RemoveAt(Index);

	// The table outlives the agent unless somebody says so: a retired vehicle's reservations
	// would block the junction it was standing in for the rest of the session.
	Occupancy.ReleaseAll(AgentId);

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d retired"), AgentId);
	OnAgentPhaseChanged.Broadcast(AgentId, Before, EAgentPhase::Gone);
	return true;
}

void UGroundTraffic::ClearAgents()
{
	// Announced before Reset so a listener asking about the id still gets To == Gone for
	// every agent it was tracking, the same promise Advance's removal path makes.
	for (const FRoadAgent& Agent : Agents)
	{
		OnAgentPhaseChanged.Broadcast(Agent.Id, Agent.Phase, EAgentPhase::Gone);
	}

	Agents.Reset();
	Occupancy.Clear();
}

void UGroundTraffic::Advance(double DeltaSeconds, const URoadNetwork* Network)
{
	SimSeconds += DeltaSeconds;

	// CLAIMS FIRST, MOTION SECOND. Every agent's StopWithin is decided before any of them
	// moves, so a node taken by the first agent in the order is already taken when the last
	// one asks. Interleaving the two - arbitrate one agent, move it, arbitrate the next -
	// would let the agent at the end of the list drive into a junction that was free when
	// it was asked about and occupied by the time it got there.
	if (Network != nullptr)
	{
		Arbitrate(*Network);
	}

	// Every handover (arrive -> taxi -> depart -> gone, or arrive -> taxi -> park) is owned
	// by FRoadAgent::Advance - see its own comment. This loop is left with: advance, watch
	// the phase, accrue the stall clock, drop an agent once it says Gone. The deadlock pass
	// that READS that clock runs after it, below, and does not touch this order.
	for (int32 Index = Agents.Num() - 1; Index >= 0; --Index)
	{
		FRoadAgent& Agent = Agents[Index];
		const EAgentPhase Before = Agent.Phase;
		const int32 Id = Agent.Id;

		FAgentMotion Motion;
		if (!Agent.Advance(DeltaSeconds, Motion))
		{
			// Cleared or otherwise finished - the aircraft has gone, so everything it held
			// goes with it. An agent that stayed in the table would hold a runway nothing
			// could ever release.
			Occupancy.ReleaseAll(Id);
			Agents.RemoveAt(Index);
			// Broadcast AFTER the removal so a listener that asks GetAgentCount sees the
			// agent already gone, which is what "To == Gone" promises.
			OnAgentPhaseChanged.Broadcast(Id, Before, EAgentPhase::Gone);
			continue;
		}

		// THE RUNWAY CHANGES HANDS AT THE HANDOVER ITSELF, in the tick that made it.
		//
		// Waiting for the agent's next claim pass would leave a vacated runway held for a
		// whole frame, and every landing offered in that frame refused for nothing -
		// DispatchArrival refuses on ANY held claim (see ArrivalPlanner::Plan). Both
		// chains come off the AGENT rather than a fresh lookup, because by now the graph
		// may have been rebuilt under it; both were recorded at dispatch for that reason.
		if (Before == EAgentPhase::Arriving && Agent.Phase == EAgentPhase::Taxiing)
		{
			// VACATED HANDS THE RUNWAY TO THE CROSSING RULE, it does not give it back. An
			// aircraft that has just left the roll is standing at the runway exit with its
			// tail on the strip; releasing here (which is what this did before spec §3.1's
			// fourth route) let a landing be cleared onto it. ClaimAhead now holds the chain
			// occupied until the tail is geometrically clear, and logs the release there.
			//
			// The SEED, not the chain: ClaimAhead re-expands it every tick, so a rebuild
			// between here and the release cannot leave it holding segments that have gone.
			if (Agent.RunwayHeld.Num() > 0)
			{
				Agent.CrossingRunway = Agent.RunwayHeld[0];
			}
			Agent.RunwayHeld.Reset();
		}
		else if (Before == EAgentPhase::Taxiing && Agent.Phase == EAgentPhase::Departing)
		{
			// The taxi that armed this departure has reached the threshold: what it held as
			// a reservation over the runway edges it drove becomes the occupancy it keeps
			// until Gone.
			Agent.RunwayHeld = Agent.DepartureRunway;

			// CLAIMED HERE AND NOT ON THE NEXT TICK'S non-Taxiing pass. A route that ends on
			// a junction turn path carries no DerivedFrom, so nothing claimed the strip while
			// it taxied the last few metres onto it; waiting a frame would leave the runway
			// unheld with an aeroplane lining up on it, and a landing could be cleared into
			// that frame. The result is not acted on for the same reason ClaimAhead's
			// non-Taxiing branch ignores it: a table cannot tell an aircraft at the threshold
			// to stop, and who goes next is URunwaySequencer's question in M3.
			for (const FRoadSegmentId Segment : Agent.RunwayHeld)
			{
				FTrafficClaim Claim;
				Claim.AgentId = Id;
				Claim.Resource = FTrafficResource::OfSurface(Segment);
				Claim.bOccupied = true;
				Claim.Rank = TraversalPriority(Agent.Class);
				FTrafficClaim Blocker;

				// THE RESULT IS NOT ACTED ON, BUT IT IS NOT SWALLOWED EITHER. Nothing can
				// tell an aeroplane already at the threshold to stop, and who goes next is
				// URunwaySequencer's question in M3 - but a Held here means the one thing
				// this claim can ever reveal: somebody else is on the strip this departure
				// is about to roll down. That must leave a line, or the only evidence of it
				// is an aeroplane taking off through another.
				if (Occupancy.TryClaim(Claim, Blocker) != EClaimResult::Granted)
				{
					UE_LOG(LogAirsideTraffic, Warning,
						TEXT("Agent %d rolls for departure while agent %d holds runway segment %d"),
						Id, Blocker.AgentId, Segment.Index);
				}
			}
		}

		// STOPPED AND WAITING, not merely stopped: an aircraft sitting out its shutdown pause
		// is not stalled, and neither is one crawling through a turn. All three conditions
		// together are what ResolveDeadlocks means by a waiter, and the clock
		// resets the moment any of them stops holding, so a junction wait that clears on its
		// own leaves nothing behind.
		Agent.StalledSeconds = (Agent.Phase == EAgentPhase::Taxiing && Agent.WaitingOn != 0
			&& Agent.Follower.Speed < KINDA_SMALL_NUMBER)
			? Agent.StalledSeconds + DeltaSeconds
			: 0.0;

		if (Agent.Phase != Before)
		{
			OnAgentPhaseChanged.Broadcast(Id, Before, Agent.Phase);
		}
	}

	// LAST, after every agent has claimed, moved and announced. The wait-for edges it reads
	// are then one frame's worth rather than a mixture of two, and an agent it replans starts
	// the next tick at the top of Arbitrate - which is the pass that re-claims for the new
	// plan. Running it before the motion loop would resolve on the previous frame's stalls
	// and hand the follower a plan the arbiter had not yet been asked about.
	if (Network != nullptr)
	{
		ResolveDeadlocks(*Network);
	}
}

bool UGroundTraffic::CanReplanAtBlockedStep(const FRoadAgent& Agent) const
{
	if (Agent.Phase != EAgentPhase::Taxiing
		|| Agent.Follower.Speed >= KINDA_SMALL_NUMBER
		|| Agent.BlockedStep < 0)
	{
		return false;
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (Agent.BlockedStep >= Plan.Steps.Num())
	{
		// A BlockedStep that outran its plan - a replan between the refusal and here. Nothing
		// to ban and no node to turn at, so this agent is not a candidate this window.
		return false;
	}

	// AT THE NODE THE REFUSED STEP LEAVES FROM, which is where the alternatives are. Negative
	// means the agent is INSIDE the edge it was refused, and taking another edge out of that
	// node would mean reversing - out of M2 by spec §1. The upper bound is where the agent is
	// actually allowed to stop: the box-entry rule parks it a GAP short of the box's start,
	// and Travelled is the CENTRE, so its nose is half a footprint further on again.
	const double ToNode = StepStart(Plan, Agent.BlockedStep) - Agent.Follower.Travelled;
	return ToNode >= -KINDA_SMALL_NUMBER
		&& ToNode <= Rules.GapFor(Agent.Class) + Rules.FootprintFor(Agent.Class) * 0.5;
}

void UGroundTraffic::ResolveDeadlocks(const URoadNetwork& Network)
{
	// ONE EDGE PER STALLED WAITER. StalledSeconds only accrues while an agent is Taxiing,
	// stopped and naming a blocker (see Advance), and ClaimAhead clears WaitingOn the moment
	// an agent stops taxiing - so a parked or retired agent cannot contribute an edge, and a
	// cycle through one is not representable rather than merely unlikely.
	TMap<int32, int32> Waiting;
	for (const FRoadAgent& Agent : Agents)
	{
		if (Agent.StalledSeconds > Rules.StallSeconds && Agent.WaitingOn != 0)
		{
			Waiting.Add(Agent.Id, Agent.WaitingOn);
		}
	}
	if (Waiting.Num() == 0)
	{
		return;
	}

	// Every agent whose walk has been made, this tick. Out-degree is one, so the walk from an
	// agent already walked would follow the identical chain and find the identical cycle:
	// skipping it is what makes this pass linear rather than quadratic, and what stops one
	// cycle being handled once per member.
	TSet<int32> Visited;

	for (const TPair<int32, int32>& Start : Waiting)
	{
		if (Visited.Contains(Start.Key))
		{
			continue;
		}

		// THE WALK IS BOUNDED BY CONSTRUCTION: every step either stops or appends an agent
		// not already on the path, and Waiting is finite - so no counter is needed and none
		// is used, which is better than a guard whose limit would be a second rule about how
		// big a jam may be. Position is the path membership test and the cycle's start index
		// in one, because the cycle is the path FROM the revisited agent onward, not all of it
		// (an agent can wait on a jam it is not part of).
		TArray<int32> Path;
		TMap<int32, int32> Position;
		int32 At = Start.Key;
		int32 CycleAt = INDEX_NONE;
		while (true)
		{
			if (const int32* Where = Position.Find(At))
			{
				CycleAt = *Where;
				break;
			}
			if (Visited.Contains(At))
			{
				break;
			}
			const int32* Next = Waiting.Find(At);
			if (Next == nullptr)
			{
				// Waiting on somebody who is not a stalled waiter - a moving agent, or one
				// that has not been stopped long enough. That is a queue, not a deadlock.
				break;
			}
			Position.Add(At, Path.Num());
			Path.Add(At);
			At = *Next;
		}
		for (const int32 Walked : Path)
		{
			Visited.Add(Walked);
		}
		if (CycleAt == INDEX_NONE)
		{
			continue;
		}

		TArray<int32> Cycle;
		Cycle.Append(Path.GetData() + CycleAt, Path.Num() - CycleAt);

		// THE RETRY WINDOW, and it is the whole of "logged once". A cycle is re-detected on
		// every tick for as long as it lasts; what makes it one report is that nothing is
		// done - not even a log line - until some member has gone Rules.RetrySeconds without
		// an attempt. Some member and not every member, because a cycle that has just gained
		// a fresh waiter deserves the same retry the old members were already due.
		bool bDue = false;
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgent(Id);
			bDue = bDue || (Member != nullptr && Member->LastResolveAttempt <= SimSeconds - Rules.RetrySeconds);
		}
		if (!bDue)
		{
			continue;
		}

		// THE LOWEST-RANKED WAITER THAT CAN ACTUALLY TURN. Lowest class priority first, so a
		// van goes round rather than an aeroplane; ties to the HIGHEST id, which is the later
		// arrival - the one with least of its journey already made.
		int32 Candidate = 0;
		int32 CandidateRank = 0;
		bool bAllAircraft = true;
		FString Members;
		for (const int32 Id : Cycle)
		{
			const FRoadAgent* Member = FindAgent(Id);
			if (Member == nullptr)
			{
				continue;
			}
			Members += Members.IsEmpty() ? FString::Printf(TEXT("%d"), Id) : FString::Printf(TEXT(", %d"), Id);
			bAllAircraft = bAllAircraft && Member->Class == ETraversalClass::Aircraft;

			if (!CanReplanAtBlockedStep(*Member))
			{
				continue;
			}
			const int32 Rank = TraversalPriority(Member->Class);
			if (Candidate == 0 || Rank < CandidateRank || (Rank == CandidateRank && Id > Candidate))
			{
				Candidate = Id;
				CandidateRank = Rank;
			}
		}

		// STAMPED ON EVERY MEMBER, WHATEVER HAPPENS NEXT, and before the replan rather than
		// after it: the stamp is what schedules the next attempt, and a cycle whose replan
		// fails must not be re-tried on the very next tick for ever. A successful replan
		// clears the stalled member's clock anyway (see ReplanAt).
		for (const int32 Id : Cycle)
		{
			const int32 Index = FindIndex(Id);
			if (Index != INDEX_NONE)
			{
				Agents[Index].LastResolveAttempt = SimSeconds;
			}
		}

		bool bResolved = false;
		if (Candidate != 0)
		{
			// READ BEFORE THE REPLAN, because ReplanAt rewrites the plan and clears
			// BlockedStep - the ban would be read off the new plan otherwise, banning an edge
			// of the route that was just chosen.
			const FRoadAgent* Turner = FindAgent(Candidate);
			const int32 Step = Turner->BlockedStep;
			const FGuidelineEdgeId Banned = Turner->Follower.Plan.Steps[Step].Edge;
			bResolved = ReplanAt(Candidate, Network, Step, Banned);
			if (bResolved)
			{
				LastResolvedAgent = Candidate;
			}
		}

		// ALL-AIRCRAFT CYCLES ARE A DESIGN PROBLEM, NOT A TRAFFIC ONE - the input to the
		// build-tool warning of the systems map §6 - so they are raised to Warning whatever
		// the outcome. A cycle nobody can break is a Warning either way.
		if (bResolved)
		{
			if (bAllAircraft)
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("All-aircraft Deadlock among agents [%s] resolved: agent %d replans"),
					*Members, Candidate);
			}
			else
			{
				UE_LOG(LogAirsideTraffic, Log, TEXT("Deadlock among agents [%s] resolved: agent %d replans"),
					*Members, Candidate);
			}
		}
		else
		{
			UE_LOG(LogAirsideTraffic, Warning, TEXT("%sDeadlock among agents [%s]: no member can turn; retrying in %.0f s"),
				bAllAircraft ? TEXT("All-aircraft ") : TEXT(""), *Members, Rules.RetrySeconds);
		}

		// COUNTED WHEN LOGGED, keyed by the lowest member id: the same ring re-formed later is
		// the same jam to a player reading the log, and a cycle re-detected inside its retry
		// window never got here at all.
		int32 Key = Cycle[0];
		for (const int32 Id : Cycle)
		{
			Key = FMath::Min(Key, Id);
		}
		CyclesSeen.Add(Key);
	}
}

int32 UGroundTraffic::CurrentStep(const FRoutePlan& Plan, double Travelled)
{
	for (int32 Index = 0; Index < Plan.Steps.Num(); ++Index)
	{
		if (Travelled < Plan.Steps[Index].EndDistance)
		{
			return Index;
		}
	}

	// Past the end of the last step - an agent that has arrived, or one a rounding error
	// put a fraction beyond its final EndDistance. The last step is still the one it is on;
	// returning Steps.Num() would index off the end at every call site.
	return FMath::Max(0, Plan.Steps.Num() - 1);
}

double UGroundTraffic::StepStart(const FRoutePlan& Plan, int32 Step)
{
	return Step <= 0 ? 0.0 : Plan.Steps[Step - 1].EndDistance;
}

FGuidelineNodeId UGroundTraffic::StepFromNode(const FRoutePlan& Plan, int32 Step)
{
	return Step <= 0 ? Plan.Start : Plan.Steps[Step - 1].To;
}

int32 UGroundTraffic::RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class) const
{
	const FGuidelineNode* Found = Network.GetGuidelineNode(Node);
	if (Found != nullptr && Found->PriorityOverride.Num() > 0)
	{
		// Scaled by ten so an authored order can never tie with a default one - a tie keeps
		// the holder, and an authored "vehicles first" that tied would mean nothing. A class
		// the author left out of the list ranks below everything named in it.
		const int32 Index = Found->PriorityOverride.Find(Class);
		return Index == INDEX_NONE ? 0 : 10 * (Found->PriorityOverride.Num() - Index);
	}
	return TraversalPriority(Class);
}

void UGroundTraffic::ClaimAhead(FRoadAgent& Agent, const URoadNetwork& Network)
{
	// NOT TAXIING: hold the runway and nothing else. An arrival on the roll and a departure
	// lining up own the strip; whatever either held on the taxiway before the handover is
	// released here, which is what makes "Vacated releases the chain" fall out of the tick
	// rather than needing a call of its own.
	if (Agent.Phase != EAgentPhase::Taxiing)
	{
		// CLEARED, or the arbitration fields say whatever they said on the last tick this
		// agent taxied. A parked aircraft still naming the vehicle it once queued behind
		// would feed Task 8's wait-for graph an edge out of an agent that is not waiting for
		// anything, and a cycle through it would be a phantom nobody could resolve.
		Agent.StopWithin = TNumericLimits<double>::Max();
		Agent.WaitingOn = 0;
		Agent.BlockedStep = INDEX_NONE;
		Agent.LastOverlaps.Reset();

		// A CROSSING IS A TAXIING IDEA. Whatever phase this is now, RunwayHeld governs what
		// it holds, so the field is cleared rather than left to name a chain nothing will
		// ever release - and the release is announced, because the claim was.
		if (Agent.CrossingRunway.IsSet())
		{
			Agent.CrossingRunway = FRoadSegmentId();
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Agent.Id);
		}

		TArray<FTrafficResource> Surfaces;
		Surfaces.Reserve(Agent.RunwayHeld.Num());
		for (const FRoadSegmentId Segment : Agent.RunwayHeld)
		{
			Surfaces.Add(FTrafficResource::OfSurface(Segment));
		}
		Occupancy.ReleaseExcept(Agent.Id, Surfaces);

		for (const FTrafficResource& Resource : Surfaces)
		{
			FTrafficClaim Claim;
			Claim.AgentId = Agent.Id;
			Claim.Resource = Resource;
			Claim.bOccupied = true;
			Claim.Rank = TraversalPriority(Agent.Class);

			// The result is not acted on: an aircraft already rolling cannot be told to stop
			// by a table, and DispatchArrival refused the landing at the door if the chain
			// was held (see ArrivalPlanner::Plan). Who is allowed onto a runway NEXT is
			// URunwaySequencer's question in M3, asked before anything is dispatched.
			FTrafficClaim Blocker;
			Occupancy.TryClaim(Claim, Blocker);
		}
		return;
	}

	const FRoutePlan& Plan = Agent.Follower.Plan;
	if (!Plan.IsValid() || Plan.Steps.Num() == 0)
	{
		// Nothing to walk, so nothing to hold. Released rather than left alone: an agent
		// whose plan was replaced by an invalid one would otherwise keep its last claims
		// for the rest of the session.
		const TArray<FTrafficResource> Nothing;
		Occupancy.ReleaseExcept(Agent.Id, Nothing);

		// AND THE CROSSING WITH THEM, exactly as the non-Taxiing branch above does it. A
		// crossing is expressed as claims the loop below re-raises every tick, and this
		// return skips that loop - so leaving the field set would name a chain nothing will
		// ever release, and ClaimAhead's header promise that a crossing cannot be stranded
		// would be false for the one agent whose plan went bad under it.
		if (Agent.CrossingRunway.IsSet())
		{
			Agent.CrossingRunway = FRoadSegmentId();
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Agent.Id);
		}

		Agent.StopWithin = TNumericLimits<double>::Max();
		Agent.WaitingOn = 0;
		Agent.BlockedStep = INDEX_NONE;
		Agent.LastOverlaps.Reset();
		return;
	}

	const double T = Agent.Follower.Travelled;
	const double F = Rules.FootprintFor(Agent.Class);
	const double G = Rules.GapFor(Agent.Class);

	// The follower's own braking figure, not the rules': the window has to be the distance
	// THIS airframe needs, or an agent reserves less line than it can stop in.
	const double Decel = FMath::Max(KINDA_SMALL_NUMBER, Agent.Follower.Ground.Taxi.Decel);
	const double Window = Agent.Follower.Speed * Agent.Follower.Speed / (2.0 * Decel) + G;

	// HALF the footprint each way, because Travelled is the CENTRE, and the window is
	// measured from the NOSE - which is what FTrafficRules::AircraftGap already says the gap
	// is ("clear line kept ahead of the nose"), and what makes spec §3.2's "a stopped agent
	// still holds Footprint + Gap" arithmetically true: F/2 + F/2 + G.
	//
	// THE REJECTED ALTERNATIVE IS Head = T + Window, spec §3.1's wording read without §3.2.
	// It is not cosmetic. A stopped agent then holds exactly up to the boundary it was told
	// to stop G short of - the two TOUCH, half-open intervals do not conflict when they
	// touch, so it is GRANTED, accelerates, grows its window by the very next tick, is
	// refused, brakes to a stop, and is granted again, for ever. Measured, both ways, on the
	// head-on and node-yield fixtures: 2496 "Agent N resumes" lines in one test run without
	// the F/2, against 4 in the whole 99-test suite with it. That flicker also reset
	// StalledSeconds every other tick, which would have left Task 8's deadlock detection
	// unable to see a single stalled agent - the bug would have surfaced two tasks later,
	// as a resolver that never fires.
	const double Head = T + F * 0.5 + Window;
	const double Tail = T - F * 0.5;
	const int32 Current = CurrentStep(Plan, T);

	TArray<FWantedClaim> Pending;
	Pending.Reserve(4);

	/**
	 * A RESERVED SURFACE CLAIM MUST NEVER OVERWRITE AN OCCUPIED ONE ALREADY WANTED THIS PASS.
	 *
	 * TryClaim treats a same-agent claim on the same resource as an UPDATE and writes it
	 * over the old one WHOLESALE, bOccupied included. A crossing raises its chain occupied
	 * at the front of Pending (route zero below), and two things later in route order raise
	 * the SAME chain as a reservation: a hold-short bar on the FAR side of the crossing -
	 * which is how a crossing is actually painted, one bar each side - and a runway edge on
	 * a step the agent has not reached yet. Either one downgraded the crossing's occupancy
	 * to a reservation, and a reservation is exactly what a landing may preempt: the table
	 * would then clear an aircraft onto an aeroplane standing on the centreline.
	 *
	 * Skipped rather than re-ordered or merged. Route order is what the first-refusal rule
	 * reads, so the entries cannot move; and there is nothing to ask for anyway - the agent
	 * already holds that surface, more strongly than the entry being dropped would hold it.
	 * Occupied entries are never skipped by this, so an occupancy raised later still lands.
	 */
	auto WantedOccupied = [&Pending](const FTrafficResource& Resource)
	{
		return Pending.ContainsByPredicate([&Resource](const FWantedClaim& Already)
		{
			return Already.Claim.bOccupied && Already.Claim.Resource == Resource;
		});
	};

	// 0. THE RUNWAY THIS AGENT IS PHYSICALLY CROSSING - spec §3.1's fourth route, and the
	// one that closes the hole the other three leave. Once the tail passes a bar the bar
	// node leaves the window and its surface claim goes with it, while the agent is standing
	// on the centreline; and a junction's turn paths carry no DerivedFrom BY DESIGN, so the
	// crossing edges never imply the runway either. A landing could be cleared onto an
	// aircraft mid-crossing. So the crossing is held explicitly, from the bar until the tail
	// is geometrically clear.
	{
		const FGuidelineNodeId FromId = StepFromNode(Plan, Current);
		const FGuidelineNode* FromNode = Network.GetGuidelineNode(FromId);

		// SET when the bar is the node the CURRENT step left AND that step leads ONTO the
		// strip. The first half is exactly "the centre has passed a bar it was granted": the
		// agent is only ever on a step whose start it has reached, and it only reached this
		// one because the bar claim was granted.
		//
		// THE SECOND HALF IS SPEC §3.1'S "Refined 2026-09-06 during Task 7", and without it
		// this rule cannot tell a crossing being ENTERED from one being LEFT. A crossing is
		// painted with a bar on EACH side, so the far bar is also "a hold-short node the
		// agent has just passed": arming there re-armed the hold on the way out and kept the
		// runway occupied behind an aircraft that had already crossed, for the whole exit
		// leg - measured at 17000 uu, i.e. the hold never ended at all.
		//
		// TWO WAYS ONTO THE STRIP, because either can be the true one: the step's END node
		// lies on the runway (the ordinary crossing, whose middle node sits on the
		// centreline), or the agent's own CENTRE already does (a bar placed so close to the
		// strip that the body is on it before the next node is). The exit bar answers no to
		// both - its step leads away and its body is clear - and so arms nothing.
		bool bArmsCrossing = false;
		if (FromNode != nullptr && FromNode->HoldShortFor.IsSet())
		{
			const FRoadSegmentId Bar = FromNode->HoldShortFor;
			bArmsCrossing = Network.IsGuidelineNodeOnRunway(Plan.Steps[Current].To, Bar)
				|| Network.IsPointOnRunway(Agent.LastMotion.Position, Bar);
			if (bArmsCrossing)
			{
				Agent.CrossingRunway = Bar;
			}
		}

		// CLEARED BY GEOMETRY, and only once the TAIL is past the node the step left.
		//
		// REJECTED: releasing at the next node full stop. A runway node sits ON the strip -
		// the crossing's own middle node is the obvious case - so that hands the runway back
		// with half the aeroplane still on it, which is the very frame this rule exists for.
		//
		// REJECTED: waiting for a bar on the far side. A player may place one bar, or none,
		// and a hold that waits for a bar that does not exist never ends.
		//
		// So: off the strip, the node itself ends it. ON the strip, the tail must be a full
		// strip half width past that node, which is the widest the surface can be there.
		// Route distance is at least straight-line distance, so that bound is conservative
		// for a crossing; a route that runs ALONG a runway is held by DerivedFrom anyway.
		// SUPPRESSED ONLY WHILE THE HOLD IS BEING ARMED, not for every step out of a bar:
		// the flag is the ARMING one, so a step leaving the exit bar releases like any other
		// step, which is the whole of the Task 7 refinement above.
		if (Agent.CrossingRunway.IsSet() && !bArmsCrossing)
		{
			double ChainHalfWidth = 0.0;
			const bool bOnStrip = Network.IsGuidelineNodeOnRunway(FromId, Agent.CrossingRunway, &ChainHalfWidth);
			const double TailPast = (T - F * 0.5) - StepStart(Plan, Current);
			if (TailPast >= 0.0 && (!bOnStrip || TailPast > ChainHalfWidth))
			{
				Agent.CrossingRunway = FRoadSegmentId();

				// THE RELEASE LINE LIVES HERE, not at the Vacated handover where it started:
				// vacating no longer gives the runway back, it hands it to this rule, and a
				// line claiming otherwise would be a log that lies. Transition by
				// construction - CrossingRunway is unset immediately after.
				UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Agent.Id);
			}
		}

		// AT THE FRONT of Pending, so a refusal here binds before anything further along the
		// route: the agent is standing on this surface, and nothing it might be told about a
		// node ahead can matter more than that.
		if (Agent.CrossingRunway.IsSet())
		{
			TArray<FRoadSegmentId> Chain = Network.RunwayChain(Agent.CrossingRunway);
			if (Chain.Num() == 0)
			{
				Chain.Add(Agent.CrossingRunway);
			}
			for (const FRoadSegmentId Segment : Chain)
			{
				FWantedClaim Crossing;
				Crossing.Claim.AgentId = Agent.Id;
				Crossing.Claim.Resource = FTrafficResource::OfSurface(Segment);

				// OCCUPIED, unlike the bar's reservation: the aeroplane is ON the strip now,
				// and spec §3.3 says nobody is evicted from ground they are standing on.
				Crossing.Claim.bOccupied = true;
				Crossing.Claim.Rank = TraversalPriority(Agent.Class);
				Crossing.Step = Current;
				Crossing.StepStart = StepStart(Plan, Current);
				Crossing.StepEnd = Plan.Steps[Current].EndDistance;
				Pending.Add(Crossing);
			}
		}
	}

	// THE ENTRY RULE FIRES FOR ONE BOX PER PASS - the first the window reaches that the agent
	// has not entered. See the loop below and ClaimAhead's header for the trace that rejected
	// chaining it through every consecutive box.
	bool bBoxEntryTaken = false;

	// 1. THE NODE THE CURRENT STEP LEFT, while the centre is still within half a footprint
	// of it. Without this an agent that had just crossed a junction would release it with
	// its tail still inside, and the next claimant would drive into that tail.
	if (T - StepStart(Plan, Current) < F * 0.5)
	{
		const FGuidelineNodeId From = StepFromNode(Plan, Current);
		FWantedClaim Want;
		Want.Claim.AgentId = Agent.Id;
		Want.Claim.Resource = FTrafficResource::OfNode(From);
		Want.Claim.bOccupied = true;
		Want.Claim.Rank = RankAt(Network, From, Agent.Class);
		Want.Step = Current;
		Want.StepStart = StepStart(Plan, Current);
		Want.StepEnd = Plan.Steps[Current].EndDistance;
		Pending.Add(Want);
	}

	// 2. Forward along the steps while the window still has distance left.
	for (int32 Index = Current; Index < Plan.Steps.Num(); ++Index)
	{
		const FRouteStep& Step = Plan.Steps[Index];
		const double Start = StepStart(Plan, Index);
		if (Start >= Head)
		{
			break;
		}
		const double End = Step.EndDistance;
		const double Length = End - Start;

		// A BOX: an edge too short to stand on without still blocking the node behind it,
		// which is what every junction turn path is. Spec §3.1.
		const bool bBox = Length < F + G;

		// ENTRY ONLY, AND THE FIRST BOX ONLY.
		//
		// Entry, because once the agent is INSIDE the box (T past its start) a refused end
		// node stops it short of that node like any other. The FIRST, because the window
		// routinely spans several boxes - three 400 uu junction paths sit inside a taxiing
		// van's window - and demanding the far end of every one of them is spec §3.1's
		// rejected alternative: the agent then refuses to move until a node two junctions
		// ahead is free, and the agent holding that node is waiting on it. Measured on
		// Airside.Model.Traffic.BoxEntryFirstOnly: chaining offered a stop point 400 uu short
		// of where the held node actually was, keeping the van out of an empty box.
		const bool bBoxEntry = bBox && T <= Start && !bBoxEntryTaken;
		bBoxEntryTaken = bBoxEntryTaken || bBoxEntry;

		const double Lo = FMath::Max(Tail, Start);
		const double Hi = FMath::Min(Head, End);
		if (Hi > Lo)
		{
			// Route distance to EDGE distance, measured from the edge's A end. A reversed
			// step is walked from B, so its interval mirrors through the edge length - and
			// the mirror swaps the ends, which is why From takes what Hi produced. Getting
			// this backwards would give From > To, and a half-open interval that way round
			// conflicts with nothing at all.
			double From = Lo - Start;
			double To = Hi - Start;
			if (Step.bReversed)
			{
				From = Length - (Hi - Start);
				To = Length - (Lo - Start);
			}

			FWantedClaim Want;
			Want.Claim.AgentId = Agent.Id;
			Want.Claim.Resource = FTrafficResource::OfEdge(Step.Edge);
			Want.Claim.From = From;
			Want.Claim.To = To;

			// OCCUPIED on the step the agent is standing on - never preemptable, because
			// nobody may be evicted from ground they are on. Everything beyond is a
			// reservation a higher rank may take.
			Want.Claim.bOccupied = (Index == Current);

			// The node an edge leads INTO governs it: an authored "vehicles first" at a
			// junction has to reach the approach, not just the junction itself. The step
			// being stood on is ranked at the node it left, which is the one it is in.
			Want.Claim.Rank = Index == Current
				? RankAt(Network, StepFromNode(Plan, Current), Agent.Class)
				: RankAt(Network, Step.To, Agent.Class);

			Want.Step = Index;
			Want.StepStart = Start;
			Want.StepEnd = End;
			Want.EdgeLength = Length;
			Want.bReversed = Step.bReversed;
			Pending.Add(Want);

			// THE RUNWAY UNDER THE EDGE - spec §3.1's first route to the surface rule.
			// Raised HERE, immediately after the edge it belongs to, so Pending stays in
			// ROUTE ORDER: the first refusal in that order is what decides where the agent
			// stops, and a surface entry filed out of order would offer a stop point for
			// line the agent reaches later than one it reaches sooner.
			//
			// THE WHOLE CHAIN, not the one segment this guideline was derived from: a runway
			// is several segments once it has exits, and holding only the piece under this
			// edge would let a second aircraft onto the same strip further down. The same
			// reason DispatchArrival and ArmDepartureIfRunway both record chains.
			const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Step.Edge);
			if (Edge != nullptr && Edge->DerivedFrom.IsSet() && Network.IsRunwaySegment(Edge->DerivedFrom))
			{
				for (const FRoadSegmentId Segment : Network.RunwayChain(Edge->DerivedFrom))
				{
					// COPIED FROM THE EDGE CLAIM, rank included: the surface is claimed
					// BECAUSE of that edge, and ranking the two differently would let an
					// agent win the runway and lose the line onto it, or the reverse.
					FWantedClaim OnRunway = Want;
					OnRunway.Claim.Resource = FTrafficResource::OfSurface(Segment);

					// A surface is held whole, so the interval fields mean nothing on it -
					// cleared rather than left carrying the edge's, where a later reader
					// would take them for a real extent.
					OnRunway.Claim.From = 0.0;
					OnRunway.Claim.To = 0.0;

					// OCCUPIED ONLY ON THE STEP THE AGENT IS STANDING ON, the same rule the
					// edge claim above uses and for the same reason: an aircraft actually on
					// the strip may not be evicted from it, while one merely approaching
					// holds a reservation a landing's occupancy can take.
					OnRunway.Claim.bOccupied = (Index == Current);
					OnRunway.Surface = FWantedClaim::ESurface::RunwayEdge;

					// See WantedOccupied: a reservation on a chain this agent is already
					// standing on would be written over its own occupancy.
					if (!OnRunway.Claim.bOccupied && WantedOccupied(OnRunway.Claim.Resource))
					{
						continue;
					}
					Pending.Add(OnRunway);
				}
			}
		}

		if (End < Head || bBoxEntry)
		{
			FWantedClaim Want;
			Want.Claim.AgentId = Agent.Id;
			Want.Claim.Resource = FTrafficResource::OfNode(Step.To);

			// Occupied only while the CENTRE is within half a footprint of the node, which
			// is the same test the left-behind node above uses, read forwards.
			Want.Claim.bOccupied = FMath::Abs(End - T) < F * 0.5;
			Want.Claim.Rank = RankAt(Network, Step.To, Agent.Class);
			Want.Step = Index;
			Want.bEndNode = true;
			Want.bBoxEntry = bBoxEntry;
			Want.StepStart = Start;
			Want.StepEnd = End;
			Want.EdgeLength = Length;
			Want.bReversed = Step.bReversed;
			Pending.Add(Want);

			// A NODE CARRYING A BAR CLAIMS THE RUNWAY IT PROTECTS - spec §3.1's second
			// route to the surface rule - so the stop happens AT THE BAR rather than at the
			// runway edge. On a taxiway that crosses a runway the crossing guideline is
			// derived from the TAXIWAY, so the rule above sees nothing and the bar is the
			// only thing between the agent and the strip.
			//
			// INSIDE the node-claim block, and so only once the window has reached the bar.
			// The alternative - claiming it for every step this loop visits - reserves the
			// strip from the far side of the airport, and ArrivalPlanner refuses a landing on
			// ANY held claim, reserved or not: an aircraft merely ROUTED past a bar would
			// close the runway for the whole of its taxi. Once End < Head the agent is within
			// braking distance plus a gap of the bar, which is when it needs the answer.
			//
			// APPLIES TO EVERY CLASS. A van crossing a live runway is the case a bar is for;
			// nothing here reads Agent.Class.
			const FGuidelineNode* Node = Network.GetGuidelineNode(Step.To);
			if (Node != nullptr && Node->HoldShortFor.IsSet())
			{
				// The chain, expanded HERE rather than stored at the bar, so an exit added to
				// a runway after the bar was placed still protects the whole strip - see
				// URoadNetwork::RunwayChain. Empty means the segment is no longer a runway
				// (the profile changed under the mark) and the named segment alone is still
				// honoured: a bar that silently stopped protecting anything is worse than one
				// protecting a piece of what it used to.
				TArray<FRoadSegmentId> Chain = Network.RunwayChain(Node->HoldShortFor);
				if (Chain.Num() == 0)
				{
					Chain.Add(Node->HoldShortFor);
				}

				for (const FRoadSegmentId Segment : Chain)
				{
					FWantedClaim Bar;
					Bar.Claim.AgentId = Agent.Id;
					Bar.Claim.Resource = FTrafficResource::OfSurface(Segment);

					// RESERVED, NEVER OCCUPIED - nobody is ever occupied THROUGH a bar. An
					// agent still short of one is by definition not on the runway, and an
					// occupied claim cannot be preempted, so a queue at the bar would lock
					// the strip against the very landing the bar exists to protect.
					Bar.Claim.bOccupied = false;
					Bar.Claim.Rank = RankAt(Network, Step.To, Agent.Class);
					Bar.Step = Index;
					Bar.StepStart = Start;
					Bar.StepEnd = End;
					Bar.EdgeLength = Length;
					Bar.bReversed = Step.bReversed;
					Bar.Surface = FWantedClaim::ESurface::HoldShort;
					Bar.HoldNode = Step.To;

					// THE FAR BAR OF A CROSSING, and the reason WantedOccupied exists: the
					// agent is already ON this strip, holding it occupied, so there is
					// nothing for a bar to protect it from and everything for the bar's
					// reservation to spoil. Airside.Model.Traffic.CrossingHoldsRunway
					// measures it with a bar on each side of the runway.
					if (WantedOccupied(Bar.Claim.Resource))
					{
						continue;
					}
					Pending.Add(Bar);
				}
			}
		}
	}

	// WHAT WAS ACTUALLY CLAIMED, not what was wanted: the loop below stops reserving at the
	// first refusal, so a resource further along the route was never asked for this pass and
	// keeping the agent's stale hold on it would block everybody else for line the agent has
	// just been told it cannot reach. Ground the agent is STANDING on is kept whether or not
	// the claim was granted - it is standing there either way.
	//
	// Released AFTER the claims rather than before them, which is safe and was checked: the
	// table skips an agent's own claims when it looks for conflicts, and no other agent
	// claims between this ReleaseExcept and the ones above - Arbitrate runs one agent at a
	// time. Releasing everything and re-claiming is still rejected (ClaimAhead's header):
	// this drops only what was not asked for.
	TArray<FTrafficResource> Wanted;
	Wanted.Reserve(Pending.Num());

	const int32 WasWaitingOn = Agent.WaitingOn;
	const int32 WasBlockedStep = Agent.BlockedStep;
	bool bHeld = false;

	// Everyone this pass overlapped, for the Warning's throttle. See FRoadAgent::LastOverlaps.
	TArray<int32> OverlapsThisPass;

	for (const FWantedClaim& Want : Pending)
	{
		// PAST THE FIRST REFUSAL, only ground the agent occupies is still claimed. Skipping
		// its own occupancies here was a defect: an agent refused a node it was STANDING on
		// abandoned the rest of its own body, and the agent that had merely reserved that
		// node drove into it.
		if (bHeld && !Want.Claim.bOccupied)
		{
			continue;
		}

		FTrafficClaim Blocker;
		const bool bGranted = Occupancy.TryClaim(Want.Claim, Blocker) == EClaimResult::Granted;
		if (bGranted || Want.Claim.bOccupied)
		{
			Wanted.Add(Want.Claim.Resource);
		}
		if (bGranted)
		{
			continue;
		}

		// AN OCCUPIED CLAIM CAN ONLY BE REFUSED BY ANOTHER OCCUPANT (see TryClaim), and on a
		// NODE or a SURFACE that means two bodies in one place - worth saying out loud, once
		// per new blocker. On an EDGE it does not: one claim per agent per edge means the
		// interval carries the agent's whole window as well as its body, so two of them
		// overlapping is the ordinary head-on and queueing case, and warning about it would
		// fire on every stopped queue on the airport.
		if (Want.Claim.bOccupied && Want.Claim.Resource.Kind != ETrafficResourceKind::Edge)
		{
			// THROTTLED ON THE OVERLAP'S OWN HISTORY, not on WaitingOn. WaitingOn names
			// the FIRST refusal in route order, so an overlap that was not the first
			// refusal never matched it and this Warning fired on every single tick for as
			// long as the overlap lasted - which is how a log stops being read at all.
			if (!Agent.LastOverlaps.Contains(Blocker.AgentId))
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d overlaps agent %d on %s: both are standing on it"),
					Agent.Id, Blocker.AgentId, *Want.Claim.Resource.Describe());
			}
			OverlapsThisPass.AddUnique(Blocker.AgentId);
		}

		if (bHeld)
		{
			// THE FIRST REFUSAL IN ROUTE ORDER DECIDES, which is not always the NEAREST
			// stop point - that claim was true until surfaces arrived and is not any
			// more. A step now contributes an edge entry AND a surface entry, in that
			// order, and the edge's refusal stops the agent at the blocker's boundary
			// INSIDE the step while the surface's would have stopped it a gap short of
			// the step's start, which is nearer.
			//
			// ACCEPTED, and not re-ordered to take the minimum: the edge refusal means an
			// aircraft is already on that line, so the agent queues up behind it on the
			// strip rather than waiting off it. That is worse for the runway and better
			// for the queue, and it is what the same rule already does at every other
			// shared edge on the airport - a crossing agent holds the surface OCCUPIED
			// (route four above), so the aircraft it queued behind is one the table
			// already knows about, not a landing that could be cleared onto it.
			continue;
		}

		bHeld = true;
		Agent.BlockedStep = Want.Step;

		if (Want.Surface == FWantedClaim::ESurface::RunwayEdge)
		{
			// A GAP SHORT OF WHERE THE RUNWAY BEGINS - the step's START, not its end.
			// The refused thing is the strip this edge lies on, so stopping short of the
			// edge's far end would leave the agent standing on the runway it was just
			// refused. On the step it is already standing on this is 0, which is the only
			// honest answer: it cannot stop short of where it already is.
			Agent.StopWithin = FMath::Max(0.0, Want.StepStart - T - G);
		}
		else if (Want.Surface == FWantedClaim::ESurface::HoldShort)
		{
			// THE NOSE STOPS ON THE BAR, which is why this is the one refusal that does
			// not subtract the gap: a bar is the position an aircraft is required to hold
			// AT, and stopping G short would leave it short of the mark the player
			// painted. Travelled is the CENTRE, so the nose is at T + F/2.
			//
			// A BAR AT THE FAR END OF A BOX STEP THEREFORE BEATS THE BOX-ENTRY RULE,
			// which would have stopped the agent a gap short of the box's START. That is
			// deliberate and not an oversight: the box rule guesses where an agent can
			// safely wait, and the bar is where the player SAID it waits. Both entries
			// are in Pending and the bar's is second, so this only differs from the box
			// answer when the box's end node was granted and the surface was not - i.e.
			// when the strip is busy but the junction is not, which is the case the
			// player painted the line for.
			Agent.StopWithin = FMath::Max(0.0, Want.StepEnd - T - F * 0.5);
		}
		else if (Want.Claim.Resource.Kind == ETrafficResourceKind::Edge)
		{
			// The blocker's NEAREST boundary ahead, back in route distance. On a reversed
			// step the agent is walking the edge from B, so the near end of the blocker's
			// interval is its To, mirrored.
			const double Boundary = Want.bReversed
				? Want.StepStart + (Want.EdgeLength - Blocker.To)
				: Want.StepStart + Blocker.From;
			Agent.StopWithin = FMath::Max(0.0, Boundary - T - G);
		}
		else if (Want.bEndNode)
		{
			// Refused at the ENTRY to a box: stop a gap short of the box's START, outside
			// the junction, where this agent can still turn - never inside it, where nobody
			// can and where it would block the node behind it as well.
			Agent.StopWithin = Want.bBoxEntry
				? FMath::Max(0.0, Want.StepStart - T - G)
				: FMath::Max(0.0, Want.StepEnd - T - G);
		}
		else
		{
			// The node the agent is STANDING on, refused. It cannot stop short of where it
			// already is, so the only honest answer is "do not move".
			//
			// ROUTINE, not exceptional: it fires whenever two agents' bodies are within half
			// a footprint of one node - a follower dispatched from the stand its leader has
			// not yet cleared (Airside.Model.Traffic.CarFollowing does exactly that), an
			// agent redirected onto a node somebody is crossing, or an aircraft parked on a
			// node a van drives over. It also fires for the node an agent has just left,
			// which is why the tail claim exists at all.
			//
			// AND IT CATCHES THE CROSSING SURFACE TOO, which is a SURFACE with no ESurface
			// tag: route zero's claim says "my body is on this strip", so a refusal there is
			// the same statement as a refused node - somebody else is standing where this
			// agent already is - and 0 is the same honest answer. The tagged RunwayEdge rule
			// above must not take it: that one stops an agent a gap short of the step's
			// start, which for ground the agent is already on would be a stop point behind it.
			Agent.StopWithin = 0.0;
		}

		// ON THE TRANSITION ONLY. Logged every tick this would be one line per agent per
		// frame, which is how a log stops being read at all.
		//
		// A BAR GETS ITS OWN LINE INSTEAD OF THE GENERIC ONE - spec §10 lists "hold-short
		// reached" separately from "stopped for a resource" - because it names the node
		// and the segment as well as the holder, and one event must not produce two
		// lines. Its transition test is the blocker OR the step: an agent already waiting
		// on the same holder for something else is still newly held HERE, and BlockedStep
		// is what changed when it became so.
		if (Want.Surface == FWantedClaim::ESurface::HoldShort)
		{
			if (WasWaitingOn != Blocker.AgentId || WasBlockedStep != Want.Step)
			{
				UE_LOG(LogAirsideTraffic, Log,
					TEXT("Agent %d holding short at node %d for runway segment %d held by agent %d"),
					Agent.Id, Want.HoldNode.Index, Want.Claim.Resource.Surface.Index, Blocker.AgentId);
			}
		}
		else if (WasWaitingOn != Blocker.AgentId)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d stops %.0f uu short of %s held by agent %d"),
				Agent.Id, Agent.StopWithin, *Blocker.Resource.Describe(), Blocker.AgentId);
		}
		Agent.WaitingOn = Blocker.AgentId;

		// NO break: the loop above skips every RESERVATION past this point (claiming line
		// beyond a refusal would hold it against everybody for a journey the agent is not
		// making this tick) but must go on claiming the ground the agent is standing on.
	}

	Agent.LastOverlaps = MoveTemp(OverlapsThisPass);
	Occupancy.ReleaseExcept(Agent.Id, Wanted);

	if (!bHeld)
	{
		Agent.StopWithin = TNumericLimits<double>::Max();
		Agent.WaitingOn = 0;
		Agent.BlockedStep = INDEX_NONE;
		if (WasWaitingOn != 0)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d resumes"), Agent.Id);
		}
	}
}

void UGroundTraffic::Arbitrate(const URoadNetwork& Network)
{
	// BY RANK, NOT BY LIST ORDER. Indices rather than a sorted copy of the agents: the claim
	// pass writes to the agents, so a copy would be arbitrating over stale ones.
	TArray<int32> Order;
	Order.Reserve(Agents.Num());
	for (int32 Index = 0; Index < Agents.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.Sort([this](int32 Left, int32 Right)
	{
		const int32 LeftRank = TraversalPriority(Agents[Left].Class);
		const int32 RightRank = TraversalPriority(Agents[Right].Class);

		// Ties to the lower id, which is first-to-be-dispatched. The per-node
		// PriorityOverride does NOT reorder this pass: it changes who wins a contested
		// resource (see RankAt), not who is asked first, and a per-node rule cannot decide a
		// global order without asking every node about every agent.
		return LeftRank != RightRank ? LeftRank > RightRank : Agents[Left].Id < Agents[Right].Id;
	});

	for (const int32 Index : Order)
	{
		ClaimAhead(Agents[Index], Network);
	}

	// ONE RE-PASS over whoever lost a reservation to a higher rank during that pass. Without
	// it a preempted agent drives a whole frame on a reservation it no longer holds - into
	// the very node that was just taken from it.
	for (const int32 AgentId : Occupancy.TakePreempted())
	{
		const int32 Index = FindIndex(AgentId);
		if (Index != INDEX_NONE)
		{
			ClaimAhead(Agents[Index], Network);
		}
	}
}
