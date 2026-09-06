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
	// that READS that clock is still Task 8 and does not touch this order.
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

		// STOPPED AND WAITING, not merely stopped: an aircraft sitting out its shutdown pause
		// is not stalled, and neither is one crawling through a turn. All three conditions
		// together are what the deadlock pass (Task 8) means by a waiter, and the clock
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
		Agent.StopWithin = TNumericLimits<double>::Max();
		Agent.WaitingOn = 0;
		Agent.BlockedStep = INDEX_NONE;
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
		}

		// The runway surface an edge derived from a runway segment implies (spec §3.1) lands
		// in Task 6, with the hold-short node below it.

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
		}

		// A node carrying HoldShortFor claims the chain it protects, so the stop is at the
		// bar rather than at the runway edge (spec §3.1). Task 6.
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
	bool bHeld = false;

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
		if (Want.Claim.bOccupied && Want.Claim.Resource.Kind != ETrafficResourceKind::Edge
			&& WasWaitingOn != Blocker.AgentId)
		{
			UE_LOG(LogAirsideTraffic, Warning, TEXT("Agent %d overlaps agent %d on %s: both are standing on it"),
				Agent.Id, Blocker.AgentId, *Want.Claim.Resource.Describe());
		}

		if (bHeld)
		{
			// The first refusal in ROUTE ORDER decides where the agent stops; a later one
			// cannot move that stop point nearer, and it is the nearest that binds.
			continue;
		}

		bHeld = true;
		Agent.BlockedStep = Want.Step;

		if (Want.Claim.Resource.Kind == ETrafficResourceKind::Edge)
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
			Agent.StopWithin = 0.0;
		}

		// ON THE TRANSITION ONLY. Logged every tick this would be one line per agent per
		// frame, which is how a log stops being read at all.
		if (WasWaitingOn != Blocker.AgentId)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d stops %.0f uu short of %s held by agent %d"),
				Agent.Id, Agent.StopWithin, *Blocker.Resource.Describe(), Blocker.AgentId);
		}
		Agent.WaitingOn = Blocker.AgentId;

		// NO break: the loop above skips every RESERVATION past this point (claiming line
		// beyond a refusal would hold it against everybody for a journey the agent is not
		// making this tick) but must go on claiming the ground the agent is standing on.
	}

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
