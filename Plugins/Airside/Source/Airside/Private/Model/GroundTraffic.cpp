// Dispatch, admission, redirect/retire, the tick, and the plan/step helpers everything else
// reads. The rest of UGroundTraffic lives in GroundTrafficClaims.cpp (the claim pass),
// GroundTrafficDeadlock.cpp (the wait-for graph and ReplanAt) and GroundTrafficRebuild.cpp
// (surviving a guideline rebuild). See Model/GroundTraffic.h for the map.

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/DeparturePlanner.h"
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

	const int32 Id = Admit(MoveTemp(Agent));

	// THE CLAIM IS RAISED HERE, NOT ON THE NEXT TICK'S CLAIM PASS - the same rule, and the
	// same reason, as the Taxiing -> Departing handover below in Advance. Waiting a frame
	// leaves the strip unheld with an aeroplane already committed to it, and DispatchArrival
	// is exactly the call that reads the table BETWEEN ticks (ArrivalPlanner::Plan, above):
	// a player pressing 7 twice in one frame had two aircraft cleared onto one runway, both
	// planned against a table that said it was free. Nothing about "the next Advance will fix
	// it" helps, because the second landing was already planned by then.
	//
	// FROM Plan.RunwayChain and not from the agent, which has just been moved into the array;
	// they are the same chain - see Agent.RunwayHeld above.
	//
	// THE RESULT IS NOT ACTED ON, for the reason HoldRunwayOnly gives: a table cannot tell an
	// aircraft on short final to stop, and the landing was already refused at the door if any
	// segment was held. Nor is it logged: unlike the departure handover, a Held here says
	// nothing new - ArrivalPlanner::Plan asked the same question of the same table three
	// statements ago and refused on it.
	for (const FRoadSegmentId Segment : Plan.RunwayChain)
	{
		FTrafficClaim Claim;
		Claim.AgentId = Id;
		Claim.Resource = FTrafficResource::OfSurface(Segment);
		Claim.bOccupied = true;
		Claim.Rank = TraversalPriority(ETraversalClass::Aircraft);
		FTrafficClaim Blocker;
		Occupancy.TryClaim(Claim, Blocker);
	}

	return Id;
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
		// WHERE THE ROUTE JOINS THE STRIP is where the roll starts. A route planned by
		// DeparturePlanner ends on the entry arc's node; a hand-drawn one ends wherever it
		// ends; either way the offset is measured from the route, not assumed to be the
		// threshold - which it was, so every departure teleported to the threshold and
		// spun round (samples/runway1.png, 2026-09-07).
		double EntryOffset = FMath::Clamp(
			FVector2D::DotProduct(Plan.Polyline.Last() - Threshold, Direction), 0.0, Length);
		// WHICH WAY TO ROLL. RunwayExtentAt hands back the threshold NEAREST the route's end,
		// which is right for a backtrack (taxied to an end, turn round, roll away from it)
		// and wrong for an intersection entry past the midpoint: the nearer threshold is
		// then the one AHEAD, and rolling toward it is rolling off the end. A mid-strip
		// entry rolls the way the taxi arrived - the arc delivered it aligned - and an end
		// entry, within the strip's own width of a threshold, rolls away from that end.
		double HalfWidth = 0.0;
		Network->IsPointOnRunway(Plan.Polyline.Last(), Seed, &HalfWidth);
		const bool bAtAnEnd = EntryOffset <= HalfWidth * 2.0 || Length - EntryOffset <= HalfWidth * 2.0;
		if (!bAtAnEnd && Plan.Polyline.Num() >= 2)
		{
			const FVector2D Arrived = Plan.Polyline.Last() - Plan.Polyline[Plan.Polyline.Num() - 2];
			if (FVector2D::DotProduct(Arrived, Direction) < 0.0)
			{
				Threshold = Threshold + Direction * Length;
				Direction = -Direction;
				EntryOffset = Length - EntryOffset;
			}
		}
		Agent.ArmDeparture(Threshold, Direction, Length, EntryOffset);

		// THE WHOLE CHAIN, not the seed segment: a runway is several segments by the time it
		// has exits, and a departure that held only the piece its taxi ended on would let a
		// second aircraft line up on the same strip further down. Recorded now rather than
		// looked up at the handover, because by then the graph may have been rebuilt.
		Agent.DepartureRunway = Network->RunwayChain(Seed);

		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Route ends on runway %s %.0f uu past the threshold: %.0f uu available, departure armed"),
			*RunwayDesignator::ToPairText(Direction), EntryOffset, Length - EntryOffset);
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

bool UGroundTraffic::StrandForTest(int32 AgentId)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}

	// THE RESULT ONLY. Steps, Polyline and Travelled are left exactly as they are, because
	// the case being reproduced is a plan that has stopped describing the airport while the
	// agent is still standing where it was - not an agent that has been moved or emptied.
	// FRoutePlan::IsValid reads Result and nothing else, so this is the whole of it.
	Agents[Index].Follower.Plan.Result = ERouteResult::Unreachable;
	return true;
}

bool UGroundTraffic::BeginCrossingForTest(int32 AgentId, FRoadSegmentId RunwaySeed)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || Agents[Index].Phase != EAgentPhase::Taxiing)
	{
		return false;
	}

	// EXACTLY THE TWO FIELDS the Vacated handover writes (see Advance): the seed and the
	// phase. No claim is raised here - the next Arbitrate raises it, as it does in play.
	Agents[Index].CrossingRunway = RunwaySeed;
	Agents[Index].CrossingPhase = ECrossingPhase::OnStrip;
	return true;
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

EDepartureRefusal UGroundTraffic::DepartAgent(int32 AgentId, const URoadNetwork& Network)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || Agents[Index].Phase != EAgentPhase::Parked)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: %s"), AgentId,
			Index == INDEX_NONE ? TEXT("no such agent") : *UEnum::GetValueAsString(Agents[Index].Phase));
		return EDepartureRefusal::NotParked;
	}
	const FRoadAgent& Agent = Agents[Index];
	if (!Agent.GoalNode.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: parked at no node."), AgentId);
		return EDepartureRefusal::NoRoute;
	}

	// From where it PARKED - its goal node - not from its polyline position: the search is
	// over the graph and the pose node is the graph's name for this stand.
	const FDeparturePlan Plan = DeparturePlanner::PlanAny(Network, Agent.GoalNode, Agent.Airframe, Agent.Class);
	UE_LOG(LogAirsideTraffic, Log, TEXT("DepartAgent %d: %s"), AgentId, *DeparturePlanner::Describe(Plan));
	if (!Plan.IsValid())
	{
		return Plan.Why;
	}
	if (!RedirectAgent(AgentId, &Network, Plan.Route))
	{
		return EDepartureRefusal::NoRoute;
	}
	return EDepartureRefusal::None;
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

				// OnStrip AND NOT Committed: an aircraft that has just finished its landing
				// roll is ON the asphalt by definition, whatever the geometry of the exit it
				// is about to take says. Committed would make the hold wait for its centre to
				// be found on a strip it is already leaving.
				Agent.CrossingPhase = ECrossingPhase::OnStrip;
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

		// AIRBORNE: THE RUNWAY IS FREE. A departure held the strip from the handover until
		// Gone, and Gone is the top of the climb - 300 m up, most of a minute after the
		// wheels left. In play (2026-09-06) every arrival dispatched in that minute was
		// refused "the runway is in use" while the strip sat empty. The strip is what the
		// table protects, and the strip is clear once the aircraft is established in the
		// climb: the next arrival joins its approach minutes out (FLandingRun, 1.9 km on a
		// 100 m glideslope) and cannot touch down under a climbing aircraft. Wake and
		// separation between successive movements are URunwaySequencer's (M3), not this
		// table's. Everything the departure held goes - RunwayHeld, the crossing it passed
		// a bar to reach - and the fields are reset so HoldRunwayOnly claims nothing back.
		if (Agent.Phase == EAgentPhase::Departing
			&& Agent.Departure.Phase == ETakeoffPhase::Climb
			&& (Agent.RunwayHeld.Num() > 0 || Agent.CrossingPhase != ECrossingPhase::None))
		{
			Occupancy.ReleaseAll(Id);
			Agent.RunwayHeld.Reset();
			Agent.CrossingRunway = FRoadSegmentId();
			Agent.CrossingPhase = ECrossingPhase::None;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Id);
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
