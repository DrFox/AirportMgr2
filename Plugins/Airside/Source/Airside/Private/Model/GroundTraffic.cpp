// Dispatch, admission, redirect/retire, the registry, the tick, events, and the plan/step
// helpers everything else reads. FClaimPass (TrafficClaims.cpp), FDeadlockResolver
// (GroundTrafficDeadlock.cpp) and FPlanReResolver (GroundTrafficRebuild.cpp) hold the rest -
// see Model/GroundTraffic.h for the map (issue #84 split these off by responsibility, not
// just by file).

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/AirsideCapability.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/DepartureAsk.h"
#include "Model/PushbackPlanner.h"
#include "Model/PushbackRun.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteChange.h"
#include "Model/RunwayQuery.h"
#include "Model/TaxiPlanning.h"
#include "Model/TrafficClaims.h"
#include "Model/TrafficContext.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RunwayDesignator.h"

// FTrafficRules::FootprintFor/GapFor/PushSpeedFor MOVED TO TrafficRules.cpp (issue #175),
// with the struct itself - Model/TrafficRules.h.

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
		// LIVE AIRCRAFT-ROLE ENTITIES, not GetEntities().Num() (issue #193): that count is the
		// whole slot array, dead ones included (RemoveEntity leaves a freed slot behind, same
		// as every other free-list table in this model) and depots included (their PoseRole is
		// a service role, not Aircraft - see FEntityInstance::PoseRole). A log line that says
		// "N stand(s) on the airport" and means "N slots ever allocated" is exactly the kind of
		// banner this project's own notes warn about: it reads as evidence of a count nothing
		// actually measured.
		int32 StandCount = 0;
		for (const FEntityInstance& Entity : Network.GetEntities())
		{
			if (Entity.bAlive && Entity.PoseRole == EServiceRole::Aircraft)
			{
				++StandCount;
			}
		}
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Arrival: runway %s, %.0f uu long, %.0f needed to stop, %d usable exit(s), ")
			TEXT("%d stand(s) on the airport."),
			*RunwayDesignator::ToPairText(Plan.End.Direction), Plan.End.Length, Plan.Needed,
			Plan.ExitCount, StandCount);
	}

	if (!Plan.IsValid())
	{
		const FString Sentence = ArrivalPlanner::DescribeRefusal(Plan);   // worded once, for the log and the listeners (#471)
		UE_LOG(LogAirsideTraffic, Warning, TEXT("%s"), *Sentence);
		OnArrivalRefused.Broadcast(Plan.Why, Sentence);
		return 0;
	}

	// A TAXI-IN PLAN, OR NO ARRIVAL (taxi planning, spec 2026-10-02 §1) - on the table as it stands, or by revoking a
	// departure that has not started its push (ruling 2). The arrival queue asked the same (TaxiInRefusal) before this.
	TArray<int32> Revoke;
	const FTaxiPlan Taxi = TaxiPlanning->PlanArrival(Network, Airframe, Rules, TaxiInRequestNow(Plan, Airframe), Revoke);
	if (!Taxi.IsPlanned())
	{
		return RefuseNoTaxiPlan(Airframe, TEXT(""));
	}

	FRoadAgent Agent;
	if (!Agent.StartArrival(Plan.End, Airframe, Plan.VacateAt, Taxi.Route))
	{
		// FLandingRun has already logged why. Nothing is admitted: an arrival that cannot be
		// flown must leave no aircraft in the world, rather than one frozen on final.
		return 0;
	}

	// ITS TAXI PLAN BOOKED BEFORE IT IS ADMITTED, under the id Admit is about to give it (review of #528 finding 6): a
	// booking that fails - the departures it would revoke kept - admits nothing, rather than an arrival with no plan.
	const int32 Expected = NextAgentId;
	if (!BookTaxiIn(Expected, Network, Taxi, Revoke))
	{
		return RefuseNoTaxiPlan(Airframe, TEXT(" (its taxi plan could not be booked)"));
	}

	// FRoadAgent is world-free and cannot read the actor's UPROPERTY or the rules table for
	// itself, so both are copied in here - the only time they ever need to meet. StampRules
	// also stamps ReverseSpeed, which an arrival never reads today - see its own comment for
	// why every admit path sets both figures regardless (issue #295).
	Agent.StampRules(Rules, ShutdownPauseSeconds);
	Agent.Class = ETraversalClass::Aircraft;
	Agent.SetGoalFrom(Plan.TaxiIn);

	// THE RUNWAY IS HELD FROM NOW. Claimed as occupied every tick by Advance while the phase
	// is Arriving; released at the Vacated handover. Held on the agent rather than looked
	// up again at release, because by then the graph may have been rebuilt.
	Agent.HoldRunway(Plan.RunwayChain, RunwayQuery::PointOnChain(Network, Plan.RunwayChain));

	// NO ZERO-SECOND POSE HERE, unlike DispatchAgent: FRoadAgent::StartArrival already ran
	// one and wrote LastMotion from the approach's own starting pose - see its comment - so
	// a second would be the second evaluator this codebase keeps out of motion.

	// THE THRESHOLD'S POSITION, because two parallel strips share a designator pair: "09/27"
	// alone could not say which runway a flight took (samples/2runways.png, 2026-09-29).
	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Arrival on runway %s (threshold at %.0f, %.0f): %.0f uu available, %.0f needed, vacating at exit %d of %d, ")
		TEXT("taxiing %.0f uu to a stand."),
		*RunwayDesignator::ToPairText(Plan.End.Direction), Plan.End.Threshold.X, Plan.End.Threshold.Y,
		Plan.End.Length, Plan.Needed, Plan.ExitOrdinal, Plan.ExitCount, Plan.TaxiIn.Length);

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
		Occupancy.Assert(FTrafficClaim::Make(Id, FTrafficResource::OfSurface(Segment),
			/*bOccupied*/ true, TraversalPriority(ETraversalClass::Aircraft)));
	}

	// AND THE STAND, for the same between-ticks reason. Agents.Last() is the agent Admit
	// just appended.
	ClaimGoalNodeAtDispatch(Agents.Last(), Id, Network);

	// ITS TAXI PLAN WAS BOOKED under this id above (Expected): Admit hands out NextAgentId, and nothing between admits.
	ensureMsgf(Id == Expected, TEXT("arrival admitted as %d, its taxi plan booked as %d"), Id, Expected);
	return Id;
}

void UGroundTraffic::ClaimGoalNodeAtDispatch(const FRoadAgent& Agent, int32 Id, const URoadNetwork& Network)
{
	// THE SAME RULE AS THE RUNWAY CHAIN ABOVE: raised here, not on the next claim pass,
	// because ArrivalPlanner reads the table between ticks and M3's sequencer will dispatch
	// two arrivals in one frame. Class and goal are read off the agent as admitted.
	if (Agent.Class != ETraversalClass::Aircraft || !Agent.GoalNode.IsSet()
		|| Network.FindEntityIndexByPoseNode(Agent.GoalNode) == INDEX_NONE)
	{
		return;
	}
	Occupancy.Assert(FTrafficClaim::Make(Id, FTrafficResource::OfNode(Agent.GoalNode),
		/*bOccupied*/ false, TraversalPriority(ETraversalClass::Aircraft)));
}

int32 UGroundTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FAirframe& Airframe, ETraversalClass Class, double ShutdownPauseSeconds)
{
	if (!Plan.IsDrivable())
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
	return AdmitDispatched(MoveTemp(Agent), Network, Plan, Class, ShutdownPauseSeconds);
}

int32 UGroundTraffic::DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan,
	const FVehicle& Vehicle, ETraversalClass Class, double ShutdownPauseSeconds)
{
	// The same refusal the FAirframe overload makes, before anything is started.
	if (!Plan.IsDrivable())
	{
		return 0;
	}

	FRoadAgent Agent;
	Agent.StartDrive(Plan, Vehicle);
	return AdmitDispatched(MoveTemp(Agent), Network, Plan, Class, ShutdownPauseSeconds);
}

int32 UGroundTraffic::AdmitDispatched(FRoadAgent&& Agent, const URoadNetwork* Network,
	const FRoutePlan& Plan, ETraversalClass Class, double ShutdownPauseSeconds)
{
	// FRoadAgent is world-free and cannot read the actor's UPROPERTY or the rules table for
	// itself, so both figures are copied in at dispatch - the only time they ever need to
	// meet. See FRoadAgent::StampRules for why one call sets both rather than one assignment
	// per admit path (issue #295). THE REVERSE SPEED matters here in particular: the agent
	// arms its own back-out mid-taxi when its route reaches a bay's reverse leg, so it must
	// already hold the figure by then - zero would refuse every reverse leg and strand the
	// vehicle at the service point.
	Agent.StampRules(Rules, ShutdownPauseSeconds);
	Agent.Class = Class;
	Agent.SetGoalFrom(Plan);

	ArmDepartureIfRunway(Agent, Network, Plan);

	// Posed before its first tick: a zero-second Advance asks where the taxi starts without
	// moving it, so LastMotion is a full pose - heading included - by the time the view is
	// spawned off the Admit broadcast below. StartTaxi's fallback covers a plan too short.
	//
	// GATED FIRST (#455): a route that OPENS with a reverse leg arms it in this very Advance, before any claim
	// pass has run for the agent, so without the gate a dispatch into held ground began the reverse and sat
	// mid-span. See GateReverseLeg for the three sites.
	// ENFORCED BY: Airside.Model.Traffic.ReverseFromDispatchWaitsForHeldGround
	GateReverseLeg(Agent);
	FAgentMotion Motion;
	EAgentEvent Event;
	Agent.Advance(0.0, Motion, Event);

	const int32 Id = Admit(MoveTemp(Agent));
	if (Network != nullptr)
	{
		ClaimGoalNodeAtDispatch(Agents.Last(), Id, *Network);
	}
	return Id;
}

void UGroundTraffic::ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const
{
	// CLEARED FIRST, and before every early return below. RedirectAgent and ExtendRoute send
	// an EXISTING agent along a new plan: one that had been armed for a runway and is now sent
	// to a stand would otherwise keep the old chain and hold that runway against everybody for
	// the rest of the session - and, with the ARMING kept too (bDepartureArmed and its order,
	// which this used to leave), take off on arriving at a stand. DisarmDeparture clears
	// DepartureRunway too now (issue #295) - the two were always reset together here.
	Agent.DisarmDeparture();

	// DOES THIS ROUTE END ON A RUNWAY? Asked here rather than by the tool, because the answer
	// is a fact about the network and the last polyline point is the only thing that knows
	// where the route actually finished. A route that ends anywhere else simply taxis, which
	// is what every route did before departures existed.
	// AsAircraft, since 2026-09-23: a vehicle has no climb to ask about, rather than a zeroed one.
	const FAirframe* Aircraft = Agent.AsAircraft();
	if (Network == nullptr || Plan.Polyline.Num() == 0 || Aircraft == nullptr || !Aircraft->Climb.IsSet())
	{
		return;
	}
	FRunwayEnd End;
	if (Network->RunwayExtentAt(Plan.Polyline.Last(), End))
	{
		// WHERE THE ROUTE JOINS THE STRIP is where the roll starts. A route planned by
		// DeparturePlanner ends on the entry arc's node; a hand-drawn one ends wherever it
		// ends; either way the offset is measured from the route, not assumed to be the
		// threshold - which it was, so every departure teleported to the threshold and
		// spun round (samples/runway1.png, 2026-09-07).
		double EntryOffset = FMath::Clamp(End.OffsetOf(Plan.Polyline.Last()), 0.0, End.Length);
		// WHICH WAY TO ROLL. RunwayExtentAt hands back the threshold NEAREST the route's end,
		// which is right for a backtrack (taxied to an end, turn round, roll away from it)
		// and wrong for an intersection entry past the midpoint: the nearer threshold is
		// then the one AHEAD, and rolling toward it is rolling off the end. A mid-strip
		// entry rolls the way the taxi arrived - the arc delivered it aligned - and an end
		// entry, within the strip's own width of a threshold, rolls away from that end.
		double HalfWidth = 0.0;
		Network->IsPointOnRunway(Plan.Polyline.Last(), End.Seed, &HalfWidth);
		const bool bAtAnEnd = EntryOffset <= HalfWidth * 2.0 || End.Length - EntryOffset <= HalfWidth * 2.0;
		if (!bAtAnEnd && Plan.Polyline.Num() >= 2)
		{
			const FVector2D Arrived = Plan.Polyline.Last() - Plan.Polyline[Plan.Polyline.Num() - 2];
			if (FVector2D::DotProduct(Arrived, End.Direction) < 0.0)
			{
				// THE SAME STRIP, THE OTHER WAY ROUND (#88): what used to be three loose
				// re-assignments is now what Reversed() means by construction.
				EntryOffset = End.Length - EntryOffset;
				End = End.Reversed();
			}
		}
		Agent.ArmDeparture(End, EntryOffset);

		// THE WHOLE CHAIN, not the seed segment: a runway is several segments by the time it
		// has exits, and a departure that held only the piece its taxi ended on would let a
		// second aircraft line up on the same strip further down. Recorded now rather than
		// looked up at the handover, because by then the graph may have been rebuilt - and WITH
		// a point on the strip, because a rebuild can kill every handle in it (OnGraphRebuilt's
		// re-point, RunwayQuery::RePointChain).
		TArray<FRoadSegmentId> Chain = Network->RunwayChain(End.Seed);
		const FVector2D OnStrip = RunwayQuery::PointOnChain(*Network, Chain);
		Agent.ArmDepartureRunway(MoveTemp(Chain), OnStrip);

		// The threshold's position, the arrival line's reason: parallels share a designator.
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Route ends on runway %s (threshold at %.0f, %.0f) %.0f uu past the threshold: %.0f uu available, departure armed"),
			*RunwayDesignator::ToPairText(End.Direction), End.Threshold.X, End.Threshold.Y, EntryOffset, End.Length - EntryOffset);

		// THE ROLL IS STILL DERIVED FROM THE ROUTE, not forced to the runway in use: a planner
		// route arrives aligned with it (intersection) or backtracks to its threshold, so this
		// gives the in-use direction - and a rebuild that re-arms a departure planned BEFORE the
		// player flipped keeps the original roll, which is ruling 2 (spec 2026-09-28-runway-in-
		// use): what is planned finishes as planned. Forcing InUseEnd here would turn that
		// aircraft round on the entry it already took. Loud when the two disagree, so a planner
		// that stops honouring the runway in use cannot do it quietly.
		const int32 Rolls = RunwayDesignator::Designate(End.Direction);
		const int32 InUse = RunwayDesignator::Designate(Network->InUseEnd(End).Direction);
		if (Rolls != InUse)
		{
			UE_LOG(LogAirsideTraffic, Warning,
				TEXT("Agent %d armed to roll %s against the runway in use %s (planned before a flip, or a route not from DeparturePlanner)"),
				Agent.Id, *RunwayDesignator::ToText(Rolls), *RunwayDesignator::ToText(InUse));
		}
	}
}

int32 UGroundTraffic::Admit(FRoadAgent&& Agent)
{
	Agent.AssignId(NextAgentId++);
	// BUILT BEFORE THE MOVE, from the agent it names: Gone -> the phase it was born in, Dispatched.
	const FAgentTransition Born = TransitionOf(Agent, EAgentPhase::Gone, EAgentEvent::Dispatched);
	const int32 Id = Agent.Id;
	Agents.Add(MoveTemp(Agent));
	// BEFORE THE BROADCAST BELOW, same reason as RetireAgent's and AdvanceOnce's removal path
	// (issue #295's O(1) lookup made this a real gap, not just a hypothetical one): a listener
	// firing synchronously off it - UAirsideTraffic::SpawnView, which calls FindAgent(Id) to
	// read LastMotion for the view's starting pose - used FindIndex's O(N) linear scan before
	// #295, which always saw the just-added agent whether or not any table had caught up. The
	// new AgentIndex map does not update itself with the array, so admitting an agent with the
	// old ORDER (add, broadcast, THEN rebuild) answered that same FindAgent(Id) with nullptr -
	// "Agent %d announced a phase change but is not in the model; no view spawned." on every
	// single admit, caught by the whole Present/ suite rather than a unit test of this file.
	RebuildAgentIndex();

	// Broadcast AFTER the add AND the rebuild, so a listener that spawns the view can find the
	// agent it is being told about - see UAirsideTraffic::SpawnView, which reads LastMotion
	// off it.
	Announce(Born);
	// #169: a new agent's dispatch claims its goal node in this same call, before this
	// function returns - see OccupancyRevision's own comment for why one bump per call
	// covers every claim made inside it.
	++OccupancyRevisionCount;
	return Id;
}

FAgentTransition UGroundTraffic::TransitionOf(const FRoadAgent& Agent, EAgentPhase From, EAgentEvent Cause)
{
	FAgentTransition Made;
	Made.AgentId = Agent.Id;
	Made.From = From;
	Made.To = Agent.Phase;
	Made.Cause = Cause;
	Made.GoalAtEvent = Agent.GoalNode;
	Made.At = Agent.LastMotion.Position;
	Made.Heading = Agent.LastMotion.Heading;
	return Made;
}

void UGroundTraffic::Announce(const FAgentTransition& Transition)
{
	// A PHASE CHANGE NOTHING NAMED is a defect upstream - FRoadAgent::Advance made a handover without an event, or an
	// operation announced without a cause - and every consumer that maps Cause will read it as nothing happening.
	// Said loudly, and announced anyway: a listener that never hears a phase change is the worse failure.
	// ENFORCED BY: Airside.Model.RoadAgent.EveryPhaseChangeNamesItsEvent (for Advance's handovers)
	if (Transition.Cause == EAgentEvent::None)
	{
		UE_LOG(LogAirsideTraffic, Error, TEXT("Agent %d: %s -> %s announced with no cause"), Transition.AgentId,
			*UEnum::GetValueAsString(Transition.From), *UEnum::GetValueAsString(Transition.To));
	}
	OnAgentPhaseChanged.Broadcast(Transition);
}

void UGroundTraffic::RebuildAgentIndex() const
{
	AgentIndex.Reset();
	AgentIndex.Reserve(Agents.Num());
	for (int32 Index = 0; Index < Agents.Num(); ++Index)
	{
		AgentIndex.Add(Agents[Index].Id, Index);
	}
}

int32 UGroundTraffic::FindIndex(int32 AgentId) const
{
	// STALE-CACHE GUARD (PR review on issue #295): AgentIndex is not a UPROPERTY (see its own
	// comment), so a duplicated UGroundTraffic - PIE's level duplication - can start with a
	// non-empty Agents and an empty AgentIndex, and every lookup afterwards would silently miss.
	// A count mismatch is the cheap, unambiguous tell: the two can only agree by coincidence
	// while the map is actually stale in exactly this way, so this costs one comparison on the
	// fast path and a full rebuild only on the rare frame it disagrees.
	if (AgentIndex.Num() != Agents.Num())
	{
		RebuildAgentIndex();
	}
	const int32* Found = AgentIndex.Find(AgentId);
	return Found != nullptr ? *Found : INDEX_NONE;
}

const FRoadAgent* UGroundTraffic::FindAgent(int32 AgentId) const
{
	const int32 Index = FindIndex(AgentId);
	return Index == INDEX_NONE ? nullptr : &Agents[Index];
}

int32 UGroundTraffic::HolderOfNode(FGuidelineNodeId Node) const
{
	int32 Holder = 0;
	Occupancy.IsHeld(FTrafficResource::OfNode(Node), 0, &Holder);
	return Holder;
}

TArray<FRouteRun> UGroundTraffic::RemainingRouteRuns(int32 AgentId) const
{
	TArray<FRouteRun> Runs;
	const FRoadAgent* Agent = FindAgent(AgentId);
	// EVERY PHASE ON A ROUTE (issue #444) - Taxiing and Reversing were named here, so a push drew nothing.
	if (Agent == nullptr || !Agent->IsOnRoute())
	{
		return Runs;
	}

	// A PUSH WALKS ITS OWN LINE, not the follower's: the lead-in and the far arm (FPushbackRun::Plan), the body
	// backing along it the whole way - one reverse run, of only what is LEFT of it and none once it is over (#502: a
	// stranded push drew its whole dead line) - and then the taxi out the handover starts, forward. A taxi out
	// waiting to be planned again has nothing yet to draw: the one it had is dead.
	// ENFORCED BY: Airside.Model.Traffic.PushRouteIsDrawn, Airside.Model.Traffic.PushRouteDrawsOnlyWhatIsLeft
	if (Agent->Phase == EAgentPhase::Manoeuvring)
	{
		Agent->Pushback.AppendRemainingRun(Agent->PhaseTraits().bBodyBacks, Runs);
		if (!Agent->IsWaitingFor(EAgentWait::ForTaxiOutRoute))
		{
			TArray<FRouteRun> TaxiOut;
			Agent->TaxiOutPlan.DescribeRuns(TaxiOut);
			Runs.Append(MoveTemp(TaxiOut));
		}
		return Runs;
	}

	Agent->Follower.Plan.DescribeRuns(Runs);

	// THE LIVE TOW REVERSE, drawn as solved: the first reverse run the steered axle has not
	// passed is the one being backed along (the follower waits at its start - FollowAndTow's
	// stop line), and its points become the leading axle's remaining path.
	if (Agent->Phase == EAgentPhase::Reversing && Agent->TowReverse.IsArmed())
	{
		double Along = 0.0;
		for (FRouteRun& Run : Runs)
		{
			if (Run.bReverse && Along + UE_KINDA_SMALL_NUMBER >= Agent->Follower.Travelled - 1.0)
			{
				Run.Points.Reset();
				for (const TowReverse::FSample& Sample : Agent->TowReverse.Samples)
				{
					if (Sample.Along >= Agent->TowReverse.Along && Sample.Axles.Num() > 0)
					{
						Run.Points.Add(Sample.Axles.Last());
					}
				}
				break;
			}
			for (int32 K = 1; K < Run.Points.Num(); ++K)
			{
				Along += FVector2D::Distance(Run.Points[K - 1], Run.Points[K]);
			}
		}
	}
	return Runs;
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

bool UGroundTraffic::SetVehicleForTest(int32 AgentId, const FVehicle& Vehicle)
{
	const int32 Index = FindIndex(AgentId);
	return Index != INDEX_NONE && Agents[Index].SetVehicleForTest(Vehicle);
}

bool UGroundTraffic::SetGoalForTest(int32 AgentId, FGuidelineNodeId Goal)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	Agents[Index].SetGoal(Goal);
	return true;
}

bool UGroundTraffic::ScriptWaitForTest(int32 AgentId, const FTrafficResource& Resource, int32 BlockerId, double StalledSeconds,
	int32 BlockedStep)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	// Through Refuse and the stall clock's own doors, as the claim pass and AdvanceOnce write them.
	// Step BlockedStep (INDEX_NONE unless a test names one) and an unlimited stop: only the blocker, the resource and
	// the clock are staged.
	FRoadAgent& Agent = Agents[Index];
	Agent.Refuse(BlockedStep, Resource, TNumericLimits<double>::Max(), BlockerId);
	Agent.ResetStall();
	Agent.AccrueStall(StalledSeconds);
	return true;
}

bool UGroundTraffic::BeginCrossingForTest(int32 AgentId, FRoadSegmentId RunwaySeed)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || Agents[Index].Phase != EAgentPhase::Taxiing)
	{
		return false;
	}

	// EXACTLY WHAT THE Vacated HANDOVER ARMS (see Advance): the seed and the phase, together
	// through BeginCrossing. No claim is raised here - the next Arbitrate raises it, as it
	// does in play.
	Agents[Index].BeginCrossing(RunwaySeed, ECrossingPhase::OnStrip);
	return true;
}

void UGroundTraffic::ReleaseGoal(FRoadAgent& Agent, int32 AgentId)
{
	// THE OLD STAND FREES NOW, between ticks: the next claim pass would drop it anyway
	// (ClaimGoalNode reads the new goal), but a planner asking in this frame must see it
	// free. And the model notes that a stand may have freed, for the re-offer pass.
	if (Agent.GoalNode.IsSet())
	{
		Occupancy.Release(AgentId, FTrafficResource::OfNode(Agent.GoalNode));
		bStandsMayHaveFreed = true;
	}

	// CLEARED HERE, WHERE THE GOAL ACTUALLY MOVES - not by the caller after this returns.
	// The stand re-offer (ReofferStands until #444) used to clear it on the agent AFTER RedirectAgent by re-running
	// FindIndex(Id), but TakeGoal is about to give the agent a real destination, and
	// RedirectAgent's broadcast (OnAgentPhaseChanged) may run a listener that retires an
	// agent synchronously - the re-entrancy contract AdvanceOnce states. (UJobBoard::OnAgentPhase
	// did, when it was bound straight to the broadcast; since the ops bus it hears the event a
	// drain later and nothing in production retires inside it - but the contract is this class's,
	// not its listeners'.) A caller re-indexing Agents by Id AFTER that broadcast can find
	// INDEX_NONE and index off the end. Clearing before the broadcast needs no such lookup: Agent
	// is still the entry the caller already found.
	//
	// A STAND WAIT ONLY (issue #444), as ClearAwaitingStand cleared only bAwaitingStand: a ForTaxiOutRoute hold is
	// ended by its own retry, which plans the route it waits for (AdoptTaxiOut / EndWait in ReplanHeldTaxiOut) - a goal
	// moving under a held departure does not give it a way to the runway.
	if (Agent.IsWaitingFor(EAgentWait::ForStand))
	{
		Agent.EndWait();
	}
}

void UGroundTraffic::TakeGoal(FRoadAgent& Agent, int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan)
{
	// THE NEW GOAL, ITS DEPARTURE AND ITS CLAIM, in that order: the arming reads the plan's
	// end, and the claim reads the goal SetGoalFrom has just written.
	Agent.SetGoalFrom(Plan);
	ArmDepartureIfRunway(Agent, Network, Plan);
	if (Network != nullptr)
	{
		ClaimGoalNodeAtDispatch(Agent, AgentId, *Network);
	}
	// #169: UNCONDITIONAL - the old goal was freed (ReleaseGoal) and the new one claimed whether
	// or not the agent's phase moves, and a Parked -> Taxiing redirect or an extension under a
	// moving agent is the common case where it does not. HERE, with the claim, so no caller of
	// the goal change can forget it; bumped before RedirectAgent's broadcast, so a listener
	// reading the revision sees the goal it is being told about.
	++OccupancyRevisionCount;
}

void UGroundTraffic::ChangeRoute(FRoadAgent& Agent, const FRouteChange& Change, const URoadNetwork* Network)
{
	// THE AGENT'S HALF MOVES NO GOAL - it cannot be asked to (ERouteGoal has no Move) - so it is handed Keep, and
	// TakeGoal, after it, sets the new goal from the plan with its claim. The bracket is in this order - release,
	// change, take - because ReleaseGoal reads the goal the agent had and TakeGoal the plan it now has.
	// ENFORCED BY: Airside.Model.Traffic.StandClaim (old stand released, new one held, at the redirect)
	ReleaseGoal(Agent, Agent.Id);
	Agent.ApplyRouteChange(Change, ERouteGoal::Keep, Occupancy);
	TakeGoal(Agent, Agent.Id, Network, Change.Plan);
}

bool UGroundTraffic::ExtendRoute(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Tail,
	double KeepBehind, double* OutDropped)
{
	if (OutDropped != nullptr)
	{
		*OutDropped = 0.0;
	}
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	FRoadAgent& Agent = Agents[Index];
	const FRoutePlan& Live = Agent.Follower.Plan;
	// A TAXI ONLY - FRoadAgent::IsReplannable, the table's column (issue #444): the splice is onto the follower.
	if (!Agent.IsReplannable() || !Live.IsValid())
	{
		return false;
	}
	// EVERY STEP KEPT: the join is the live plan's end, so the agent is on the unchanged prefix
	// and Replace's "Travelled survives" is exact.
	FRoutePlan Spliced = RouteSearch::Splice(Live, Live.Steps.Num(), Tail);
	if (!Spliced.IsValid())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("ExtendRoute %d refused: the tail does not start where the route ends"), AgentId);
		return false;
	}

	// THE DRIVEN HISTORY, TRIMMED to whole steps ending more than KeepBehind behind the agent.
	// Built here as the plan the steps after the cut would have made on their own: starting at
	// the node the last dropped step arrives at, its polyline from that step's end vertex, every
	// kept step's EndVertex and EndDistance re-based - Splice's own re-basing, run backwards.
	double Dropped = 0.0;
	if (KeepBehind >= 0.0 && Agent.GetBlockedStep() < 0)
	{
		const double Cut = Agent.Follower.Travelled - KeepBehind;
		int32 Keep = 0;
		while (Keep < Spliced.Steps.Num() && Spliced.Steps[Keep].EndDistance < Cut)
		{
			++Keep;
		}
		if (Keep > 0 && Keep < Spliced.Steps.Num())
		{
			const FRouteStep& LastDropped = Spliced.Steps[Keep - 1];
			FRoutePlan Trimmed;
			Trimmed.Result = Spliced.Result;
			Trimmed.Start = LastDropped.To;
			for (int32 At = LastDropped.EndVertex; At < Spliced.Polyline.Num(); ++At)
			{
				Trimmed.Polyline.Add(Spliced.Polyline[At]);
			}
			for (int32 Step = Keep; Step < Spliced.Steps.Num(); ++Step)
			{
				FRouteStep Rebased = Spliced.Steps[Step];
				Rebased.EndVertex -= LastDropped.EndVertex;
				Rebased.EndDistance -= LastDropped.EndDistance;
				Trimmed.Steps.Add(Rebased);
			}
			Trimmed.Length = GuidelineGeom::PolylineLength(Trimmed.Polyline);
			Dropped = LastDropped.EndDistance;
			Spliced = MoveTemp(Trimmed);
		}
	}

	// SAID, NOT SILENT, WHEN THE TAIL DOES NOT CONTINUE THE LINE: Splice welds at the NODE, so a
	// tail that leaves the join at an angle beyond the steer lock (or, with sampling, a point
	// off it) is accepted and then driven as a kink - RedirectAgent's kept-pose warning, for the
	// same reason: a caller should not inherit a crawl or a fold it cannot trace to here.
	{
		FVector2D EndAt = FVector2D::ZeroVector;
		double EndHeading = 0.0;
		FVector2D TailAt = FVector2D::ZeroVector;
		double TailHeading = 0.0;
		if (GuidelineGeom::PointAtDistance(Live.Polyline, Live.Length, EndAt, EndHeading)
			&& GuidelineGeom::PointAtDistance(Tail.Polyline, 0.0, TailAt, TailHeading))
		{
			const double OffDegrees = FMath::Abs(FMath::RadiansToDegrees(FMath::UnwindRadians(TailHeading - EndHeading)));
			const double LockDegrees = Agent.Chassis().Ground.MaxSteerDegrees;
			const double Gap = FVector2D::Distance(EndAt, TailAt);
			if (OffDegrees > LockDegrees || Gap > 1.0)
			{
				UE_LOG(LogAirsideTraffic, Warning, TEXT("ExtendRoute %d: the tail does not continue the route - it leaves %.0f deg off the route's end heading (lock %.0f), %.0f uu from its end; expect a crawl or a fold at the join."),
					AgentId, OffDegrees, LockDegrees, Gap);
			}
		}
	}

	// THE EXTENDED ROUTE JUDGED WHOLE, FROM THE LIVE CHAIN (review of 9441ccf1). The tail was
	// judged on its own, from a straight lay at its start; the trailer arrives at the join swung
	// by whatever the live route ended with, and two curves that each hold it can fold it
	// together. Refused before anything changes, like a tail that does not join: the caller's
	// route runs out and it plans again from rest.
	// ENFORCED BY: Airside.Model.Tow.ExtendRouteJudgesTheJoin
	//
	// THE LIVE SEED, LESS THE TRIMMED HISTORY: Travelled is measured along the plan being judged, which starts Dropped
	// later than the live one. Its heading is LastMotion's since issue #429 - this site alone read the follower's, the
	// fifth builder the "aligned 2026-09-27" note missed; the two agree for a tow driving forward, which is the only
	// thing ExtendRoute accepts (Taxiing).
	if (TOptional<FTowSeed> Seed = Agent.LiveTowSeed(); Seed.IsSet() && Network != nullptr)
	{
		const FVehicle& Vehicle = *Agent.AsVehicle();
		Seed->Travelled -= Dropped;
		const FFitVerdict Whole = VehicleFit::JudgePlan(Spliced, Vehicle, *Network, Seed.GetPtrOrNull());
		if (Whole.Refusal == EFitRefusal::TrailerFolds)
		{
			UE_LOG(LogAirsideTraffic, Warning, TEXT("ExtendRoute %d refused: the extended route folds the %s's tow (%s)"),
				AgentId, *Vehicle.TypeCode.ToString(), *Whole.Describe());
			return false;
		}
	}

	// THE GOAL MOVES, EXACTLY AS IN RedirectAgent AND THROUGH THE SAME TWO CALLS: the old goal's
	// claim and any stand wait let go (ReleaseGoal), then the new goal, its departure arming
	// (re-evaluated for the new end - an extended route whose OLD end was a runway must not take
	// off there) and its claim (TakeGoal). ChangeRoute's bracket, since issue #429 - with an
	// Extend's own aftermath inside it: Replace, Travelled rebased by Dropped, and NOTHING let go
	// and the wait left standing, because every step the agent had is still its route.
	// ENFORCED BY: Airside.Model.Traffic.ExtendRouteMovesTheGoal, Airside.Model.RouteChange.ExtendKeepsTheWait
	const double WasLength = Live.Length;
	ChangeRoute(Agent, FRouteChange::Extend(Spliced, Dropped), Network);
	if (OutDropped != nullptr)
	{
		*OutDropped = Dropped;
	}
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d route extended: %.0f uu on from %.0f, %.0f uu of driven route trimmed"),
		AgentId, Tail.Length, WasLength, Dropped);
	return true;
}

bool UGroundTraffic::RerouteAgent(int32 AgentId, const URoadNetwork* Network, int32 KeepSteps, const FRoutePlan& Tail)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	FRoadAgent& Agent = Agents[Index];
	const FRoutePlan& Live = Agent.Follower.Plan;
	// A TAXI ONLY - FRoadAgent::IsReplannable, ExtendRoute's reason (issue #444).
	if (!Agent.IsReplannable() || !Live.IsValid())
	{
		return false;
	}
	// GROUND VEHICLES ONLY - see the header. The one caller (UJobBoard::DriveVehicleTo) sends
	// vehicles; the guard is what keeps an aircraft's runway and departure arming off this path.
	if (Agent.AsVehicle() == nullptr)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RerouteAgent %d refused: not a ground vehicle"), AgentId);
		return false;
	}

	// NEVER BEHIND THE AGENT - see ReplanAt's identical guard, and the header.
	const int32 OnStep = CurrentStep(Live, Agent.Follower.Travelled);
	if (KeepSteps <= OnStep || KeepSteps > Live.Steps.Num())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RerouteAgent %d refused: keeping %d step(s) would cut the step the agent is on (%d)"),
			AgentId, KeepSteps, OnStep);
		return false;
	}

	const FRoutePlan Spliced = RouteSearch::Splice(Live, KeepSteps, Tail);
	if (!Spliced.IsValid())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RerouteAgent %d refused: the new route does not start where step %d ends"),
			AgentId, KeepSteps - 1);
		return false;
	}

	// THE SPLICE JUDGED WHOLE, FROM THE LIVE CHAIN - ExtendRoute's check, and the reverse too: a
	// route home from a service point opens its tail with the bay's reverse leg, which only the
	// chain the kept prefix leaves can solve. Refused, like a failed search, before anything moves -
	// and at LOG level, as SpliceReplan's is: a caller tries the next node on (UJobBoard::
	// SendTruckHome), so a refusal here is an answer to a question, not a fault.
	// LastMotion's heading, with its position - FPlanReResolver::QueryFor's reason, now FRoadAgent::LiveTowSeed's.
	if (const TOptional<FTowSeed> Seed = Agent.LiveTowSeed(); Seed.IsSet() && Network != nullptr)
	{
		const FVehicle& Vehicle = *Agent.AsVehicle();
		const FFitVerdict Whole = VehicleFit::JudgePlan(Spliced, Vehicle, *Network, Seed.GetPtrOrNull());
		if (Whole.Refusal == EFitRefusal::TrailerFolds || Whole.Refusal == EFitRefusal::ReverseUnsolvable)
		{
			UE_LOG(LogAirsideTraffic, Log, TEXT("RerouteAgent %d refused: the new route does not hold the %s's tow (%s)"),
				AgentId, *Vehicle.TypeCode.ToString(), *Whole.Describe());
			return false;
		}
	}

	// THE GOAL MOVES THROUGH ReleaseGoal/TakeGoal, as in RedirectAgent and ExtendRoute; the
	// follower keeps driving through Replace; and ReplanAt's bookkeeping for a route that changed
	// under a moving agent - see FPlanReResolver::ReplanAt for why each of the three is needed.
	// All of it one Splice through ChangeRoute (issue #429), the change ReplanAt makes with its goal kept.
	// ENFORCED BY: Airside.Model.RouteChange.SpliceEndsTheWait
	const double WasRemaining = Live.Length - Agent.Follower.Travelled;
	ChangeRoute(Agent, FRouteChange::Splice(Spliced), Network);
	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d re-routed after step %d: %.0f uu to go, was %.0f"),
		AgentId, KeepSteps - 1, Spliced.Length - Agent.Follower.Travelled, WasRemaining);
	return true;
}

bool UGroundTraffic::RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan, EAgentEvent Cause)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || !Plan.IsDrivable())
	{
		return false;
	}
	FRoadAgent& Agent = Agents[Index];
	const EAgentPhase Before = Agent.Phase;
	// STRANDED TOO (issue #396: a stranded taxi-in used to be Parked at that moment, and was redirected from here). A
	// restart puts it at the plan's FIRST point, which is a teleport unless it stands there - so SendAgentTo redirects a
	// stranded agent only when it is measured at the new route's start (its Stranded row), and the stand
	// re-offer rescues any other stranded waiter from where it stands (UGroundTraffic::ReofferStand, #429 review).
	// ENFORCED BY: Airside.Model.Traffic.ReofferWaiterStrandedAtItsExit, .ReofferStrandedWaiterIsRescued,
	// .ReofferStrandedWaiterDoesNotJump
	// THE THREE ARE FRoadAgent::IsRedirectable's column (issue #444) - the stand retry's waiter filter asks the same.
	if (!Agent.IsRedirectable())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("RedirectAgent %d refused: agent is %s"),
			AgentId, *UEnum::GetValueAsString(Before));
		return false;
	}

	// The bundle is the agent's own - a redirect changes where it goes, not what it is. It
	// used to be copied out here and handed back to StartTaxi, which assigned it from its
	// argument; RestartTaxi keeps whichever bundle (airframe or vehicle) the agent already
	// holds and never touches it, so there is nothing to copy and no self-assignment.

	// THE OLD GOAL LETS GO - see ReleaseGoal, shared with ExtendRoute so the two cannot drift.
	// Inside ChangeRoute's bracket since issue #429, below, after the pose is decided: nothing the
	// pose reads (LastMotion, the chassis, the plan) is a goal fact, so the order is free.

	// THE ENGINE'S "CAPTURED BEFORE StartTaxi" pair moved into FRoadAgent::ApplyRouteChange's
	// Restart with the rule it served (issue #429): a held taxi out planned again restarts too,
	// and its own copy of "keep the spool" was the second definition of what keeping it means.

	// A TOW KEEPS ITS CAB'S HEADING through a redirect, as it keeps its chain (StartDrive: "a rig
	// re-routed mid-drive keeps its trailer where it is, angled as it was"). The chain is stepped
	// against the cab's FIXED axle, a wheelbase behind the steered axle the follower re-seats on
	// the new line; seeding the heading from that line instead swings the fixed axle sideways in
	// one frame, and the chain reads the swing as a pull of up to a wheelbase. Measured
	// 2026-09-25: both tows on the rig course folded on the first frame of the leg that starts
	// on the width step's jog, and every handover moved the rig's trailer axle up to 350 uu.
	// Anything WITHOUT a chain keeps seeding from the line - an aircraft sent somewhere new
	// faces its new route at once, which Airside.Model.Traffic.RedirectPosesImmediatelyEvenPaused
	// pins; the follower's slew then closes the gap for a tow, at its own rate.
	// ENFORCED BY: Airside.Model.Tow.RedirectKeepsChainAndHeading
	// THE POSE SHOWN, NOT THE FOLLOWER'S (2026-09-26): after a tow reverse the vehicle parks where
	// FTowReverseRun left it, and the follower - which never ran during the reverse - still holds
	// the pose it had at the reverse leg's start. LastMotion is where the cab actually is; for a
	// tow that was taxiing the two are the same (DescribeMotion poses from the follower).
	const TOptional<double> KeptHeading = Agent.TowAxles.Num() > 0
		? TOptional<double>(Agent.LastMotion.Heading) : TOptional<double>();
	double InitialTravelled = 0.0;

	// SAID, NOT SILENT, when the kept pose cannot drive the new line: a plan that starts beyond
	// the steer lock of where the cab points, or somewhere other than where its steered axle
	// stands, has the follower slew or jump and the chain fold a frame later - with nothing
	// naming the redirect as the cause. The rig course always redirects from the lane end it
	// stopped on; AirportOps' return-to-depot for a towing vehicle need not be so careful, and
	// would otherwise inherit a silent fold.
	if (KeptHeading.IsSet())
	{
		const FVector2D Steered = Agent.LastMotion.Position
			+ FVector2D(FMath::Cos(KeptHeading.GetValue()), FMath::Sin(KeptHeading.GetValue())) * Agent.Chassis().SteerAxleX;
		double StartGap = FVector2D::Distance(Steered, Plan.Polyline[0]);

		// A CAB PART-WAY ALONG THE NEW LINE is seated where it is, not at the line's start. A tow
		// that backed into a bay stops with its TRAILER axle on the bay end - where a route out of
		// the bay starts - and its steered axle a chain's length up the exit; RestartTaxi at zero
		// jumped the cab back onto the trailer (665 uu on the rig, M_RigTest yard, 2026-09-26) and
		// folded it. Only when the steered axle is NOT at the start, so a redirect from the lane
		// end a tow stopped on - every loop-course redirect - is exactly what it was.
		// ENFORCED BY: AirportMgr.RigCourse.YardHeadless (every bay-to-bay redirect, the chain never jumping)
		double ProjectedOff = -1.0;
		if (StartGap > 1.0)
		{
			int32 Span = 0;
			double Fraction = 0.0;
			const double Off = GuidelineGeom::NearestOnPolyline(Plan.Polyline, Steered, Span, Fraction);
			ProjectedOff = Off;
			if (Off <= 1.0 + VehicleFit::TowReverseMaxExitOffset)
			{
				for (int32 K = 0; K < Span && K + 1 < Plan.Polyline.Num(); ++K)
				{
					InitialTravelled += FVector2D::Distance(Plan.Polyline[K], Plan.Polyline[K + 1]);
				}
				if (Plan.Polyline.IsValidIndex(Span + 1))
				{
					InitialTravelled += Fraction * FVector2D::Distance(Plan.Polyline[Span], Plan.Polyline[Span + 1]);
				}
				StartGap = Off;
			}
		}
		FVector2D NewStart = FVector2D::ZeroVector;
		double NewHeading = KeptHeading.GetValue();
		GuidelineGeom::PointAtDistance(Plan.Polyline, InitialTravelled, NewStart, NewHeading);
		// A ROUTE THAT OPENS WITH A REVERSE LEG is backed along, so the cab should face AWAY from
		// the line's direction: 180 degrees off is the fit there, not the misfit (2026-09-27
		// - every stand service cycle's route home warned "expect a slew or a fold" and did neither).
		if (Plan.Steps.Num() > 0 && Plan.Steps[0].bReverseLeg)
		{
			NewHeading += UE_DOUBLE_PI;
		}
		const double OffDegrees = FMath::Abs(FMath::RadiansToDegrees(FMath::UnwindRadians(NewHeading - KeptHeading.GetValue())));
		const double LockDegrees = Agent.Chassis().Ground.MaxSteerDegrees;
		if (OffDegrees > LockDegrees || StartGap > 1.0)
		{
			UE_LOG(LogAirsideTraffic, Warning, TEXT("RedirectAgent %d: the tow's kept pose does not fit the new line - start heading %.0f deg off the cab (lock %.0f), start %.0f uu from the steered axle (%.0f uu off the line where it stands); expect a slew or a fold."),
				AgentId, OffDegrees, LockDegrees, StartGap, ProjectedOff);
		}
	}
	// ONE RESTART THROUGH THE SEAM (issue #429): the goal released and taken around it, the
	// follower restarted from rest where the pose above says, the engine carried on (#107 - see
	// ApplyRouteChange's Restart, where the "already turning" and "not running" branches now live),
	// and the old route's RESERVATIONS let go and its wait ended, as every other change of route
	// already did. Those last two are new here: a Taxiing waiter redirected kept its stall clock
	// running into the new route, where the deadlock pass read it as a stalled cycle member on its
	// first refusal, and kept the old route's line ahead reserved until the next claim pass. What
	// the agent STANDS on is kept (Reservations, not the guidelines too) - a restart does not hop.
	// A Parked or Stranded agent holds no reservation and has no stall clock running (AdvanceOnce
	// resets it off a route), so for the two phases this used to be written for, nothing changes.
	// ENFORCED BY: Airside.Model.RouteChange.RestartEndsTheWait
	//
	// Class is NOT re-derived: a van redirected is still a van. StartTaxi rewrites the
	// follower and the airframe and nothing else, so the identity fields survive it; only
	// the goal moves, because that is the whole of what a redirect changes - TakeGoal.
	ChangeRoute(Agent, FRouteChange::Restart(Plan, ERouteRelease::Reservations, InitialTravelled, KeptHeading), Network);

	// POSED NOW, NOT LEFT FOR THE NEXT TICK (#107 item 3) - the same reason DispatchAgent runs
	// a zero-second Advance before Admit. UGroundTraffic::Advance early-returns on a paused
	// frame (DeltaSeconds <= 0), so nothing would otherwise call FRoadAgent::Advance for the
	// rest of one - and StartTaxi's own LastMotion reset above is a bare FAgentMotion() with
	// only Position filled in: heading 0 regardless of which way this route actually goes,
	// EngineRPM 0 regardless of what StartEngineAtSpeed just wrote into Agent.EngineRPM.
	// UAirsideTraffic::Advance poses the view off exactly this field every tick, paused ones
	// included, so a player redirecting while paused would see the aeroplane facing east with
	// a stopped propeller until play resumed, whichever way the new route actually points.
	//
	// GATED FIRST (#455), and this is THE case: every stand service cycle's route home opens with the reverse
	// leg, so this zero-second Advance is where a service truck arms its back-out - with no claim pass run
	// yet, and so no notion that another vehicle already holds the leg's ground. See GateReverseLeg.
	//
	// ARBITRATION CLEARED FIRST: the agent may be an existing one still carrying a refusal from the route it is being
	// taken off (a Taxiing agent redirected mid-wait), and TryArmReverseLeg reads the refusal the gate leaves. The
	// new route starts from rest and asks the table afresh, so the old answer means nothing to it. (AdmitDispatched
	// needs none: its agent has never been arbitrated.) Cleared by ChangeRoute above since issue #429 - a Restart
	// ends the wait - so it is already clear by here.
	// ENFORCED BY: Airside.Model.Traffic.ReverseFromRedirectWaitsForHeldGround,
	// Airside.Model.Traffic.ReverseIgnoresARefusalElsewhereOnTheRoute
	GateReverseLeg(Agent);
	FAgentMotion Motion;
	EAgentEvent Event;
	Agent.Advance(0.0, Motion, Event);

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d redirected: %.0f uu"), AgentId, Plan.Length);
	// ONE TRANSITION, THE CALLER'S CAUSE, even when the posing Advance above armed a reverse leg (Parked -> Reversing
	// for a truck whose route home opens backwards): the redirect is why it moved, and the arm is how. A redirect
	// that moves no phase (a taxiing agent given a new route) announces nothing, as before.
	if (Agent.Phase != Before)
	{
		Announce(TransitionOf(Agent, Before, Cause));
	}
	// #169: the occupancy revision was bumped in TakeGoal, with the claim - unconditional, unlike
	// the broadcast above.
	return true;
}

FDepartureAsk UGroundTraffic::AskDeparture(const FRoadAgent& Agent, const FAirframe& Aircraft,
	const URoadNetwork& Network) const
{
	FDepartureAsk Ask;

	// From where it PARKED - its goal node - not from its polyline position: the search is
	// over the graph and the pose node is the graph's name for this stand.
	// WITH THE OCCUPANCY, so a free runway beats a busy one (2026-09-29, samples/2runways.png).
	Ask.Plan = DeparturePlanner::PlanAny(Network, Agent.GoalNode, Aircraft, Agent.Class, &Occupancy);
	if (!Ask.Plan.IsValid())
	{
		Ask.Why = Ask.Plan.Why;
		return Ask;
	}

	// CAN IT SIMPLY DRIVE OUT? A MEASUREMENT OF THE GROUND AHEAD, not a flag on the stand -
	// which is why one question answers a taxi-through stand, a taxiway a player happened to
	// draw past a stand, and a graph rebuilt since this aeroplane parked.
	//
	// AGAINST LastMotion.Heading, which is where the aeroplane is actually pointing. The
	// stand's authored heading was the rejected alternative: it is the same number today, but
	// it describes the STAND, and an aeroplane that ended its taxi a few degrees off would
	// then be measured against something it is not aligned with.
	FVector2D StartAt = FVector2D::ZeroVector;
	double OutTangent = 0.0;
	const bool bHaveTangent =
		GuidelineGeom::PointAtDistance(Ask.Plan.Route.Polyline, 0.0, StartAt, OutTangent);

	// 180 WHEN THE POLYLINE CANNOT SAY - honouring the return rather than reading an
	// uninitialised double. A route with no direction is not a route that leads straight out.
	Ask.OffDegrees = bHaveTangent
		? FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(OutTangent - Agent.LastMotion.Heading)))
		: 180.0;
	if (bHaveTangent && Ask.OffDegrees <= Rules.StraightOutDegrees)
	{
		Ask.bStraightOut = true;
		PlanTaxiOut(Agent, Aircraft, Network, Ask);
		return Ask;
	}

	// THE PUSH GETS A ROUTE OF ITS OWN - see PushbackPlanner, and see FPushbackRun's header
	// for what walking a prefix of the DEPARTURE route did instead. It reverses onto the arm
	// of the junction the departure does not take, so that driving forward afterwards carries
	// the aeroplane through the junction and away; and because it finishes somewhere the
	// departure route never visits, the taxi out is planned from there in the same breath.
	//
	// CLEAR BY A FOOTPRINT AND A GAP, which is what this model already means by "clear of"
	// everywhere else - FClaimPass's window is built from the same pair. Not a new figure: how
	// far past a junction an aeroplane must finish is the same question as how much room it
	// takes up, and inventing a second answer is how the two drift.
	Ask.Push = PushbackPlanner::Plan(Network, Agent.GoalNode, Ask.Plan,
		Aircraft, Agent.Class,
		Rules.PushClearBy(Agent.Class));
	if (!Ask.Push.IsValid())
	{
		Ask.Why = EDepartureRefusal::NoPushbackRoute;
		return Ask;
	}

	// PUSHBACK CLEARANCE: granted whole, or withheld. A manoeuvring agent cannot replan -
	// there is no alternative way off a stand - so if the deadlock resolver ever picked one it
	// would have no move to make. Granting the whole push up front makes it atomic and removes
	// it as a deadlock source, and it is what ground control actually does: clearance is
	// granted or withheld, never half-granted.
	if (!IsPushGroundFree(Agent.Id, Ask.Push.PushRoute, Ask.Push.PushRoute.Length))
	{
		Ask.Why = EDepartureRefusal::PushbackBlocked;
		return Ask;
	}
	PlanTaxiOut(Agent, Aircraft, Network, Ask);
	return Ask;
}

EDepartureRefusal UGroundTraffic::DepartAgent(int32 AgentId, const URoadNetwork& Network)
{
	// OFF THE PUSH WATCH WHATEVER THIS ANSWERS - a PushbackBlocked below puts it back with the route it was refused on.
	// Every other answer is either a departure under way or a refusal the watch does not wait on.
	PushWatch.Remove(AgentId);

	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE || Agents[Index].Phase != EAgentPhase::Parked)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: %s"), AgentId,
			Index == INDEX_NONE ? TEXT("no such agent") : *UEnum::GetValueAsString(Agents[Index].Phase));
		return EDepartureRefusal::NotParked;
	}
	FRoadAgent& Agent = Agents[Index];

	// AN AEROPLANE'S ERRAND. A parked vehicle has no runway to plan for and no pushback need;
	// before 2026-09-23 it would have been planned with a zeroed airframe and refused by the
	// planner for want of a take-off roll. Refused here, by kind, and said so.
	const FAirframe* Aircraft = Agent.AsAircraft();
	if (Aircraft == nullptr)
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: not an aircraft."), AgentId);
		return EDepartureRefusal::NoRoute;
	}
	if (!Agent.GoalNode.IsSet())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("DepartAgent %d refused: parked at no node."), AgentId);
		return EDepartureRefusal::NoRoute;
	}

	// ALREADY CLEARED, TO PUSH LATER (taxi planning): its plan is booked and StartDuePushes starts it. Asked again
	// meanwhile it is still not pushing - and is not re-planned, which would give up its place in the order.
	if (const FTaxiClearance* Booked = TaxiPlanning->Find(AgentId); Booked != nullptr && Booked->Stage == ETaxiClearanceStage::Booked)
	{
		return EDepartureRefusal::PushbackBlocked;
	}

	// THE DECISION, planned once - see AskDeparture, which the push watch asks too. What follows logs and acts on it,
	// in the order the decision was made.
	const FDepartureAsk Ask = AskDeparture(Agent, *Aircraft, Network);
	const FDeparturePlan& Plan = Ask.Plan;
	// SAID ON A CHANGE, not per call - see LastDepartVerdict. A success is always said, and
	// clears the gate so a later refusal of the same aircraft is said again.
	const FString Verdict = DeparturePlanner::Describe(Plan);
	FString& LastSaid = LastDepartVerdict.FindOrAdd(AgentId);
	if (Plan.IsValid() || LastSaid != Verdict)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("DepartAgent %d: %s"), AgentId, *Verdict);
	}
	LastSaid = Plan.IsValid() ? FString() : Verdict;
	if (!Plan.IsValid())
	{
		return Plan.Why;
	}

	if (Ask.bStraightOut && Ask.bNoTaxiPlan)
	{
		return WatchForTaxiPlan(AgentId, Network, FRoutePlan());
	}
	if (Ask.bStraightOut)
	{
		// THE MEASURED ANGLE IS IN THE LINE. When a player asks why an aeroplane did not push
		// back, the angle that was measured IS the answer, and guessing it back out of the
		// geometry costs a PIE session - which is the one thing this project's notes say to
		// spend log lines on.
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d departs straight out of its stand (%.0f deg off the parked heading)"),
			AgentId, Ask.OffDegrees);
		// ON ITS TAXI PLAN'S ROUTE, now or when the plan says - see ClearDeparture; StartPlannedDeparture redirects it.
		return ClearDeparture(AgentId, Network, Ask);
	}

	const FPushbackPlan& Push = Ask.Push;
	UE_LOG(LogAirsideTraffic, Log, TEXT("DepartAgent %d: %s"), AgentId,
		*PushbackPlanner::Describe(Push));

	if (!Push.IsValid())
	{
		// PERMANENT, and said as a Warning because it is a LAYOUT the player can fix: a stand
		// on a dead-end taxiway has no second arm to reverse onto, so nothing will ever leave
		// it. Falling back on reversing down the departure's own arm was rejected - that is
		// precisely the behaviour reported as wrong, and doing it only on some layouts would
		// make it a defect that appears and disappears.
		UE_LOG(LogAirsideTraffic, Warning,
			TEXT("DepartAgent %d refused: stand has no arm to push back onto."), AgentId);
		return EDepartureRefusal::NoPushbackRoute;
	}

	if (Ask.bNoTaxiPlan)
	{
		return WatchForTaxiPlan(AgentId, Network, Push.PushRoute);
	}
	if (Ask.Why == EDepartureRefusal::PushbackBlocked)
	{
		// AT Log AND NOT Warning: a taxiway the player has left busy refuses this for as long
		// as they leave it, and it clears itself. It is asked again only when the push watch
		// below says the answer changed (or the caller's own trigger fires), so this is a line
		// per wake, not per tick; FTurnarounds::DepartTheReady throttles its own to a change of reason.
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d cannot push back yet: %.0f uu of ground is not free."),
			AgentId, Push.PushRoute.Length);
		// ON THE WATCH, with what the refusal was planned from - see FPushWatch. The strips read NOW, the instant PlanAny
		// ranked them, not DiffFreedom's baseline from the last tick.
		// ENFORCED BY: Airside.Model.Traffic.PushGroundFreed.RegisteredWithStripsHeldNow, Airside.Model.Traffic.PushGroundFreed.RunwayFlipReplans
		RefreshRunwaySeeds(Network, /*bForce*/ false);
		FPushWatch& Watch = PushWatch.Add(AgentId);
		Watch.PushRoute = Push.PushRoute;
		Watch.Network = &Network;
		Watch.EditRevision = Network.GetEditRevision();
		Watch.GuidelineRevision = Network.GetGuidelineRevision();
		Watch.TaxiRevision = TaxiPlanRevision();
		Watch.AskedAt = SimSeconds;
		RunwaysHeldNow(Network, Watch.RunwaysHeld);
		return EDepartureRefusal::PushbackBlocked;
	}

	// THE START MOVED TO StartPlannedDeparture (GroundTrafficPlanning.cpp, taxi planning PR 2), comments and log line
	// with it: a booked departure's push is started there too, when its plan says (StartDuePushes).
	return ClearDeparture(AgentId, Network, Ask);
}

bool UGroundTraffic::IsPushGroundFree(int32 AgentId, const FRoutePlan& Plan,
	double PushDistance) const
{
	// Every edge and end node the push will touch, from the stand to PushDistance. Built in
	// route order like the claim pass's own wanted list, though nothing here needs the order:
	// clearance is all-or-nothing, so the first refusal and the last are the same answer.
	TArray<FTrafficResource> Wanted;
	Wanted.Reserve(Plan.Steps.Num() * 2);
	for (const FRouteStep& Step : Plan.Steps)
	{
		Wanted.Add(FTrafficResource::OfEdge(Step.Edge));
		Wanted.Add(FTrafficResource::OfNode(Step.To));

		// THE STEP THAT CONTAINS PushDistance IS INCLUDED, not excluded: the test is on the
		// step's END, so a push that stops half way along a step has still claimed the whole
		// of it. Rounding the other way would leave the ground under the aeroplane's nose
		// unasked for.
		if (Step.EndDistance >= PushDistance)
		{
			break;
		}
	}

	// bCountOwnOccupied FALSE. The aeroplane is standing on its own stand node and the head of
	// its own lead-in, and refusing a push because the aeroplane is where it already is would
	// refuse every push there has ever been.
	return !Occupancy.IsAnyHeld(Wanted, AgentId, /*bCountOwnOccupied*/ false);
}

void UGroundTraffic::GateReverseLeg(FRoadAgent& Agent) const
{
	// A NO-OP FOR EVERYONE NOT AT A REVERSE SPAN'S START, and that is nearly every agent on nearly every frame:
	// PendingReverseSpan answers from the follower's precomputed list of reverse steps (an empty-array check when the
	// route has none) and only cuts the span out of the route when the agent is standing at it.
	int32 First = INDEX_NONE;
	int32 Last = INDEX_NONE;
	FRoutePlan Span;
	if (!Agent.PendingReverseSpan(First, Last, Span))
	{
		return;
	}

	// THE WHOLE SPAN, in route order, the same resources IsPushGroundFree asks for (every edge and its end node) and
	// for the same reason - a reverse is granted whole or withheld. NOT IsPushGroundFree ITSELF, because that answers
	// yes or no and a refusal has to say WHAT held it and WHO: the wait-for graph's edge, the inspector's "waiting
	// on" line and the deadlock resolver all read the pair, and a bare bool would leave the agent standing still
	// for no reason anything could report. The first held resource is the one named, as the claim pass names the
	// first refusal in route order.
	//
	// THE AGENT'S OWN CLAIMS ARE NOT HELD AGAINST IT (Id excluded): the truck is standing on the node the span
	// leaves, and what it reserved for this very leg is its own. At a dispatch its Id is 0, which no claim carries,
	// so nothing is excluded and nothing it holds yet is in the way.
	for (int32 Index = 0; Index < Span.Steps.Num(); ++Index)
	{
		const FRouteStep& Step = Span.Steps[Index];
		const FTrafficResource Wanted[2] = { FTrafficResource::OfEdge(Step.Edge), FTrafficResource::OfNode(Step.To) };
		for (const FTrafficResource& Resource : Wanted)
		{
			int32 Holder = 0;
			if (Occupancy.IsHeld(Resource, Agent.Id, &Holder))
			{
				// StopWithin 0: it is at the start of the span and goes no further until it is free - the arbiter
				// saying what the follower's own stop line at the span's start (FollowAndTow) already does.
				// ENFORCED BY: Airside.Model.Traffic.ReverseWaitsForHeldGround (it has not moved when it is let go)
				Agent.Refuse(First + Index, Resource, 0.0, Holder);
				return;
			}
		}
	}
}

bool UGroundTraffic::RetireAgent(int32 AgentId)
{
	const int32 Index = FindIndex(AgentId);
	if (Index == INDEX_NONE)
	{
		return false;
	}
	// BUILT BEFORE THE REMOVAL, from the agent it names - its goal and where it stood. To is Gone: the agent never
	// reaches that phase itself, it is taken out of the table.
	FAgentTransition Retired = TransitionOf(Agents[Index], Agents[Index].Phase, EAgentEvent::Retired);
	Retired.To = EAgentPhase::Gone;
	Agents.RemoveAt(Index);
	// BEFORE THE BROADCAST BELOW, which can run a listener that calls back into FindIndex
	// (a synchronous listener may call RetireAgent itself - see AdvanceOnce's own re-entrancy
	// comment) - a stale index table would answer that call with an entry shifted or gone.
	RebuildAgentIndex();

	// The table outlives the agent unless somebody says so: a retired vehicle's reservations
	// would block the junction it was standing in for the rest of the session.
	Occupancy.ReleaseAll(AgentId);
	TaxiPlanning->Drop(AgentId, nullptr);   // and its taxi plan, now: the queue asks between ticks (RetireFreesWithoutAdvance)
	bStandsMayHaveFreed = true;

	UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d retired"), AgentId);
	Announce(Retired);
	++OccupancyRevisionCount;   // #169: ReleaseAll, above, may have freed a stand or a runway.
	// AND SAYS WHICH, NOW - see DiffFreedom (review M1). After the broadcast, like the revision bump.
	// ENFORCED BY: Airside.Model.Traffic.RunwayFreed.DespawnOnRunway, AirportOps.Present.ArrivalQueue.RetireFreesWithoutAdvance
	DiffNow();
	return true;
}

void UGroundTraffic::ClearAgents()
{
	// Announced before Reset so a listener asking about the id still gets To == Gone for
	// every agent it was tracking, the same promise Advance's removal path makes.
	//
	// COPIED OUT FIRST, then announced: a synchronous listener may retire or dispatch (the re-entrancy contract
	// AdvanceOnce states), and a range-for over Agents while it does is a loop over an array being written.
	TArray<FAgentTransition> Cleared;
	Cleared.Reserve(Agents.Num());
	for (const FRoadAgent& Agent : Agents)
	{
		FAgentTransition& Each = Cleared.Add_GetRef(TransitionOf(Agent, Agent.Phase, EAgentEvent::Cleared));
		Each.To = EAgentPhase::Gone;
	}
	for (const FAgentTransition& Each : Cleared)
	{
		Announce(Each);
	}

	Agents.Reset();
	RebuildAgentIndex();
	Occupancy.Clear();
	TaxiPlanning->DropAll(TEXT("every agent was cleared"));
	// #169: AFTER Clear(), not folded into the loop above - the loop only announces; this is
	// the point every claim actually goes.
	++OccupancyRevisionCount;
	// THE PUSH WATCH NAMES AGENTS, and there are none. DiffNow below would drop every entry as gone - but only when a
	// network has been diffed; this does not depend on that. Agent ids are never reused within one UGroundTraffic
	// (NextAgentId only counts up), so a stale entry could not wake a stranger - it is hygiene, not a fix.
	PushWatch.Reset();
	// ENFORCED BY: Airside.Model.Traffic.RunwayFreed.ClearAgents
	DiffNow();
}

bool UGroundTraffic::HoldStand(int32 HolderId, FGuidelineNodeId PoseNode)
{
	if (!PoseNode.IsSet())
	{
		return false;
	}

	// A RESERVATION, never an occupation: nothing's body is at a stand hours before it lands.
	// ReleaseHold's use of ReleaseReservations depends on this staying false.
	const FTrafficClaim Claim = FTrafficClaim::Make(HolderId, FTrafficResource::OfNode(PoseNode), /*bOccupied*/ false);
	FTrafficClaim Blocker;
	const bool bGranted = Occupancy.TryClaim(Claim, Blocker) == EClaimResult::Granted;
	if (bGranted)
	{
		// #169: no agent and no phase change is involved in an AirportOps hold, so nothing
		// else would ever bump this for it.
		++OccupancyRevisionCount;
	}
	return bGranted;
}

void UGroundTraffic::ReleaseHold(int32 HolderId)
{
	Occupancy.ReleaseReservations(HolderId);
	// #169: unconditional - releasing a hold nobody made is harmless (see ReleaseReservations's
	// own comment) and a caller that just wants to be sure has no cheaper way to ask.
	++OccupancyRevisionCount;
	// A DECLINE, A CANCEL OR A DISPATCH gives a hold back between ticks - said now (review M1).
	// ENFORCED BY: Airside.Model.Traffic.RunwayFreed.StandsDiff ("the release itself fires")
	DiffNow();
}

bool UGroundTraffic::IsStandHeld(FGuidelineNodeId PoseNode, int32 ExcludingHolder) const
{
	return PoseNode.IsSet()
		&& Occupancy.IsHeld(FTrafficResource::OfNode(PoseNode), ExcludingHolder);
}

void UGroundTraffic::Advance(double DeltaSeconds, const URoadNetwork* Network)
{
	// SPLIT BEFORE STEPPING. The delta handed in is the frame time TIMES the player's speed
	// multiplier, so it grows with x2, x4, x8 - and a single step that large lets an agent
	// overshoot the waypoint it is turning onto and be corrected back next frame. On screen
	// that is an aeroplane jerking forwards and backwards; it was reported from play as
	// rubber-banding that appeared at x2 and was absent at x1, which is what named the cause.
	//
	// Everything inside a step stays exactly as it was - claims first, motion second, and
	// the whole handover chain - so this changes the SIZE of a step and nothing about what
	// one does.
	//
	// THE MOMENTS ARE THIS CALL'S, so they are emptied FIRST - before the paused frame's early return too: a
	// presenter reads them after every Advance, paused ones included, and last frame's touchdown read again on a
	// paused frame is a second puff.
	MomentsThisAdvance.Reset();
	if (DeltaSeconds <= 0.0)
	{
		return;
	}

	const double Longest = FMath::Max(Rules.MaxSubstepSeconds, KINDA_SMALL_NUMBER);
	const int32 Steps = FMath::Clamp(
		FMath::CeilToInt(DeltaSeconds / Longest), 1, FMath::Max(Rules.MaxSubsteps, 1));
	LastStepsForTest = Steps;

	// Divided rather than repeatedly subtracted: the steps then sum to exactly DeltaSeconds,
	// so SimSeconds and every integration inside stay in step with the caller's clock. Past
	// the ceiling this simply makes each step longer than Longest, which is the documented
	// trade - see FTrafficRules::MaxSubsteps.
	const double Step = DeltaSeconds / Steps;
	for (int32 Index = 0; Index < Steps; ++Index)
	{
		AdvanceOnce(Step, Network);
	}

	// AFTER EVERY SUBSTEP, once a frame - see DiffFreedom. No network, no arbitration, no table to diff.
	if (Network != nullptr)
	{
		DiffFreedom(*Network, /*bRebuilt*/ false);
		ExtendQueuedDepartures(*Network);
	}
}

void UGroundTraffic::DiffNow()
{
	if (const URoadNetwork* Network = DiffNetwork.Get())
	{
		DiffFreedom(*Network, /*bRebuilt*/ false);
	}
}

void UGroundTraffic::RefreshRunwaySeeds(const URoadNetwork& Network, bool bForce)
{
	// THE STRIPS, re-read only when the topology can have moved - see RunwaySeeds. A rebuild always re-reads:
	// it is where a runway vanishes, and the seed list must stop naming it before the diff below asks.
	if (bForce || RunwaySeedsNetwork != &Network || RunwaySeedsRevision != Network.GetEditRevision())
	{
		RunwaySeeds.Reset();
		for (const FRunwaySummary& Runway : AirsideCapability::SummariseRunways(Network))
		{
			RunwaySeeds.Add(Runway.End.Seed);
		}
		RunwaySeedsNetwork = &Network;
		RunwaySeedsRevision = Network.GetEditRevision();
	}
}

void UGroundTraffic::RunwaysHeldNow(const URoadNetwork& Network, TSet<FRoadSegmentId>& Out) const
{
	// HELD NOW, by the queue's own predicate - see OnRunwayFreed. PlanAny ranks by the same question
	// (IsAnyHeld over the strip's RunwaySurfaces), which is why the push watch keys on this set.
	//
	// THROUGH THE CHAIN CACHE (issue #446 item 8): the seed overload walks the strip's chain on every ask, and this
	// asked it of every strip every frame - closed #170's per-frame walk, back by another door. RunwayChains is the
	// claim pass's own cache (walked once per strip per topology); RunwayQuery::IsChainHeld's chain overload is the
	// same "held" over the segments it already holds.
	Out.Reset();
	for (const FRoadSegmentId Seed : RunwaySeeds)
	{
		if (RunwayQuery::IsChainHeld(RunwayChains.GetOrSeed(Network, Seed), &Occupancy))
		{
			Out.Add(Seed);
		}
	}
}

void UGroundTraffic::DiffFreedom(const URoadNetwork& Network, bool bRebuilt)
{
	RefreshRunwaySeeds(Network, bRebuilt);

	DiffNetwork = &Network;

	// INTO THE KEPT SCRATCH, then swapped with the baseline (issue #446 item 8): neither set is allocated afresh per frame.
	RunwaysHeldNow(Network, RunwaysNowScratch);
	// FREED IS "IN THE BASELINE, NOT HELD NOW" - which also covers a strip that is gone. A seed the rebuild
	// renamed for a strip still held reads as freed once: a spurious freed costs a listener one look, a missed
	// one strands an arrival queue, so the diff errs that way.
	TArray<FRoadSegmentId> FreedRunways;
	for (const FRoadSegmentId Seed : HeldRunways)
	{
		if (!RunwaysNowScratch.Contains(Seed))
		{
			FreedRunways.Add(Seed);
		}
	}
	Swap(HeldRunways, RunwaysNowScratch);
	RunwaysNowScratch.Reset();

	// THE STANDS, the same shape: IsStandCandidate is the one filter ChooseStand and UStandAllocator use, and
	// IsStandHeld is what they ask of it. Keyed by entity - see HeldStands.
	TMap<FEntityInstanceId, FGuidelineNodeId>& StandsNow = StandsNowScratch;
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Stand = Entities[Index];
		if (Stand.IsStandCandidate() && IsStandHeld(Stand.PoseNode, 0))
		{
			StandsNow.Add(Network.EntityIdAt(Index), Stand.PoseNode);
		}
	}
	TArray<FGuidelineNodeId> FreedStands;
	for (const TPair<FEntityInstanceId, FGuidelineNodeId>& Was : HeldStands)
	{
		if (!StandsNow.Contains(Was.Key))
		{
			FreedStands.Add(Was.Value);
		}
	}
	// ANY CHANGE TO THE HELD SET, either way - see StandHoldChangeCount. With nothing freed, the set now holds every
	// old member, so a different size can only be a stand gained.
	if (FreedStands.Num() > 0 || StandsNow.Num() != HeldStands.Num())
	{
		++StandHoldChanges;
	}
	Swap(HeldStands, StandsNowScratch);
	StandsNowScratch.Reset();

	// THE PUSH WATCH - see PushWatch. Each entry is asked what DepartAgent would ask: its stored route while every
	// input to the plan holds, AskDeparture whole when one moved - or on a rebuild, which re-derives rather than clears.
	TArray<int32> FreedPushes;
	for (TMap<int32, FPushWatch>::TIterator It = PushWatch.CreateIterator(); It; ++It)
	{
		const FRoadAgent* Agent = FindAgent(It.Key());
		const FAirframe* Aircraft = Agent != nullptr ? Agent->AsAircraft() : nullptr;
		if (Agent == nullptr || Aircraft == nullptr || Agent->Phase != EAgentPhase::Parked)
		{
			// GONE OR MOVED ON - retired, or departed by another door (the inspector's Depart): nothing to wake.
			It.RemoveCurrent();
			continue;
		}
		FPushWatch& Watch = It.Value();
		// !bRebuilt IS REDUNDANT TODAY: a rebuild follows a graph change, which has already moved GuidelineRevision below
		// (every guideline mutation bumps it). Kept so the rebuild's re-derive does not rest on that. Not pinned: with the
		// graph unchanged the revision holds, but then the stored route IS the route and both paths answer alike - there
		// is nothing a test could see (review M2, 2026-09-30).
		const bool bSamePlan = !bRebuilt
			&& Watch.Network.Get() == &Network
			&& Watch.EditRevision == Network.GetEditRevision()
			&& Watch.GuidelineRevision == Network.GetGuidelineRevision()
			&& Watch.RunwaysHeld.Num() == HeldRunways.Num() && Watch.RunwaysHeld.Includes(HeldRunways);
		// THE TAXI TABLE MOVED, but asked whole at most once a sim second - see FPushWatch::TaxiRevision.
		const bool bSameTaxi = Watch.TaxiRevision == TaxiPlanRevision() || SimSeconds - Watch.AskedAt < 1.0;
		bool bStillBlocked = false;
		if (bSamePlan && (bSameTaxi || !Watch.bTaxiBlocked))
		{
			bStillBlocked = Watch.bTaxiBlocked || !IsPushGroundFree(It.Key(), Watch.PushRoute, Watch.PushRoute.Length);
		}
		else
		{
			const FDepartureAsk Ask = AskDeparture(*Agent, *Aircraft, Network);
			bStillBlocked = Ask.Why == EDepartureRefusal::PushbackBlocked;
			if (bStillBlocked)
			{
				Watch.PushRoute = Ask.Push.PushRoute;
				Watch.Network = &Network;
				Watch.EditRevision = Network.GetEditRevision();
				Watch.GuidelineRevision = Network.GetGuidelineRevision();
				Watch.RunwaysHeld = HeldRunways;
				Watch.TaxiRevision = TaxiPlanRevision();
				Watch.AskedAt = SimSeconds;
				Watch.bTaxiBlocked = Ask.bNoTaxiPlan;
			}
		}
		if (!bStillBlocked)
		{
			FreedPushes.Add(It.Key());
			It.RemoveCurrent();
		}
	}

	// BASELINES FIRST, BROADCASTS AFTER: a listener that asks this model anything sees the state the diff saw.
	for (const FRoadSegmentId Seed : FreedRunways)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Runway (seed %d) freed%s"), Seed.Index, bRebuilt ? TEXT(" by a rebuild") : TEXT(""));
		OnRunwayFreed.Broadcast(Seed);
	}
	if (FreedStands.Num() > 0)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("%d stand(s) freed%s"), FreedStands.Num(), bRebuilt ? TEXT(" by a rebuild") : TEXT(""));
		OnStandsFreed.Broadcast(FreedStands);
	}
	for (const int32 AgentId : FreedPushes)
	{
		UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d is no longer blocked on its push ground%s"), AgentId,
			bRebuilt ? TEXT(" (after a rebuild)") : TEXT(""));
		OnPushGroundFreed.Broadcast(AgentId);
	}
	// AN ARRIVAL REFUSED A TAXI PLAN, AND THE TABLE HAS RELEASED A WINDOW SINCE: the queue's wake-up (OnTaxiPlansFreed).
	if (TaxiPlanning != nullptr && TaxiPlanning->TakeReleased() && bArrivalAwaitsTaxiPlan)
	{
		bArrivalAwaitsTaxiPlan = false;
		OnTaxiPlansFreed.Broadcast();
	}
}

void UGroundTraffic::AdvanceOnce(double DeltaSeconds, const URoadNetwork* Network)
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
		// TAXI PLANS FOLLOW THE CLAIMS JUST MADE: released behind each tail, entered at each nose (GroundTrafficPlanning.cpp).
		TrackTaxiPlans();
		// THE HELD TAXI-OUT REPLAN RAN HERE, before the agents moved, until #444: it is RetryWaiters' arm now, at the end
		// of this step with the stand re-offer - see RetryWaiters for why the handover lands on the same tick.
	}

	// ONE INSTANCE FOR THE HANDOVER CLAIM BELOW (issue #84): the only FClaimPass call in this
	// loop is ClaimGoalNode at the Taxiing -> Parked handover, so this is cheaper than
	// constructing one per agent and exactly as correct - see Arbitrate's own instance for why.
	//
	// TOptional, AND CONSTRUCTED ONLY WHEN THERE IS A NETWORK (issue #175): FTrafficContext
	// holds Network by reference, which cannot be bound to a null pointer, and Network null is
	// a real caller state - see Advance's own header, "the pre-M2 behaviour". The one call this
	// Pass exists for is itself gated on Network != nullptr a few lines down, so skipping
	// construction in that case changes nothing reachable; TOptional rather than a pointer to a
	// local because FClaimPass has no default constructor to placement over.
	TOptional<FClaimPass> Pass;
	if (Network != nullptr)
	{
		Pass.Emplace(FTrafficContext{*Network, Rules, Occupancy, NodeReach, RunwayChains, SimSeconds});
	}

	// Every handover (arrive -> taxi -> depart -> gone, or arrive -> taxi -> park) is owned
	// by FRoadAgent::Advance - see its own comment. This loop is left with: advance, watch
	// the phase, accrue the stall clock, drop an agent once it says Gone. The deadlock pass
	// that READS that clock runs after it, below, and does not touch this order.
	//
	// RE-ENTRANCY CONTRACT (issue #193): the broadcasts below - OnAgentPhaseChanged, through
	// Announce, here and in RedirectAgent/RetireAgent - can run a listener that calls back into
	// this class and retires or redirects ANY agent, including ones still to be visited this tick.
	// NO PRODUCTION LISTENER DOES, since the ops bus (#436 corrected this line): UAirsideTraffic
	// spawns and destroys views and relays, and the ops runtime only PUBLISHES - UJobBoard::OnAgentPhase,
	// which used to call RetireAgent synchronously from inside the broadcast, runs a drain later.
	// The contract stays because it is this class's, not its listeners': a synchronous listener
	// is legal, and Airside.Model.Traffic.ReofferStandsRetireReentrancy is one that retires.
	// ENFORCED BY: Check-Architecture rule 4 ('OnAgentPhaseChanged bound') - the two production binders named above
	// This loop tolerates that for two reasons together: it runs by DESCENDING index, so a RemoveAt at or below the
	// current Index only ever shifts already-visited slots (Index and above), never the ones
	// still to come; and it holds no reference across a broadcast - Agent and Index are used
	// only before each Broadcast call, never after. A caller that re-derives an index AFTER a
	// broadcast (FindIndex(Id) once RedirectAgent has already returned) does not have this
	// protection - see RetryWaiters' stand arm (ReofferStands until #444), which used to do exactly that.
	for (int32 Index = Agents.Num() - 1; Index >= 0; --Index)
	{
		FRoadAgent& Agent = Agents[Index];
		const EAgentPhase Before = Agent.Phase;
		const int32 Id = Agent.Id;

		// A REVERSE MAY ARM IN THIS ADVANCE, and only into free ground: asked AFTER Arbitrate, so the table it reads
		// is this tick's, and BEFORE the move. See GateReverseLeg.
		// ENFORCED BY: Airside.Model.Traffic.ReverseWaitsForHeldGround
		GateReverseLeg(Agent);
		FAgentMotion Motion;
		EAgentEvent Event;
		if (!Agent.Advance(DeltaSeconds, Motion, Event))
		{
			// Cleared or otherwise finished - the aircraft has gone, so everything it held
			// goes with it. An agent that stayed in the table would hold a runway nothing
			// could ever release.
			//
			// THE TRANSITION FIRST, from the agent it names (Event is Gone - Advance's own return), then the removal.
			FAgentTransition Gone = TransitionOf(Agent, Before, Event);
			Gone.To = EAgentPhase::Gone;
			Occupancy.ReleaseAll(Id);
			bStandsMayHaveFreed = true;
			Agents.RemoveAt(Index);
			// BEFORE THE BROADCAST, same reason as RetireAgent's own call: a listener firing
			// synchronously from it can call back into FindIndex (the contract above).
			RebuildAgentIndex();
			// Broadcast AFTER the removal so a listener that asks GetAgentCount sees the
			// agent already gone, which is what "To == Gone" promises.
			Announce(Gone);
			++OccupancyRevisionCount;   // #169: ReleaseAll, above, may have freed a stand or a runway.
			continue;
		}

		// SWITCHED ON THE EVENT FRoadAgent::Advance JUST REPORTED, not diffed from Before/
		// After phase here (issue #105 item 6) - Advance already OWNS every handover, so it
		// is the one place that should know which one just happened, Airborne included: that
		// one is not even an EAgentPhase change (Departing before and after), which is why it
		// used to need its own hand-rolled condition against a takeoff sub-phase instead of a
		// case in this switch.
		switch (Event)
		{
		case EAgentEvent::Vacated:
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
				// OnStrip AND NOT Committed: an aircraft that has just finished its landing
				// roll is ON the asphalt by definition, whatever the geometry of the exit it
				// is about to take says. Committed would make the hold wait for its centre to
				// be found on a strip it is already leaving.
				Agent.BeginCrossing(Agent.RunwayHeld[0], ECrossingPhase::OnStrip);
			}
			Agent.ReleaseRunway();
			break;

		case EAgentEvent::LinedUp:
			// The taxi that armed this departure has reached the threshold: what it held as
			// a reservation over the runway edges it drove becomes the occupancy it keeps
			// until Gone. THE RUNWAY CHANGES HANDS AT THE HANDOVER ITSELF, in the tick that
			// made it - waiting for the agent's next claim pass would leave the strip unheld
			// with an aeroplane lining up on it, and a landing could be cleared into that
			// frame. DepartureRunway comes off the AGENT rather than a fresh lookup, because
			// by now the graph may have been rebuilt under it; it was recorded at dispatch
			// for that reason.
			Agent.HoldRunway(Agent.GetDepartureRunway(), Agent.GetDepartureRunwayAt());

			// CLAIMED HERE AND NOT ON THE NEXT TICK'S non-Taxiing pass. A route that ends on
			// a junction turn path carries no DerivedFrom, so nothing claimed the strip while
			// it taxied the last few metres onto it; waiting a frame would leave the runway
			// unheld with an aeroplane lining up on it, and a landing could be cleared into
			// that frame. The result is not acted on for the same reason ClaimAhead's
			// non-Taxiing branch ignores it: a table cannot tell an aircraft at the threshold
			// to stop, and who goes next is URunwaySequencer's question in M3.
			for (const FRoadSegmentId Segment : Agent.RunwayHeld)
			{
				const FTrafficClaim Claim = FTrafficClaim::Make(Id, FTrafficResource::OfSurface(Segment),
					/*bOccupied*/ true, TraversalPriority(Agent.Class));
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
			// #169: the runway is now held, whether or not every segment's claim above was
			// Granted - see ArrivalPlanner::Plan's own RunwayOccupied check, which this is the
			// production source of. NOT covered by the phase-changed broadcast at the bottom
			// of this loop: this class's own comment on this switch says why Departing before
			// and after this event is not a phase change at all.
			++OccupancyRevisionCount;
			break;

		case EAgentEvent::Parked:
			// THE STAND IS OCCUPIED IN THE TICK THE AIRCRAFT PARKS, by the same rule as the
			// runway above: the claim pass ran before motion, while this agent was still
			// Taxiing, so its stand claim is the inbound reservation until the next pass -
			// and the panel would read "Reserved for" over an aircraft standing on the stand
			// for one frame.
			if (Network != nullptr)
			{
				Pass->ClaimGoalNode(Agent, *Network);
			}
			// A WAITING AIRCRAFT THAT HAS JUST STOPPED IS OFFERED A STAND ONCE MORE (#455). The stand retry consumes
			// bStandsMayHaveFreed whether or not it placed anybody, and a Taxiing waiter whose extension it could not
			// make (the tail did not join, or the plan died between the search and the splice) was told "keeps
			// waiting, asked again when something frees" - but the stand it was offered is still free, so nothing
			// frees, and the aircraft arrived at the end of its truncated route and sat there for good beside a free
			// stand. Its own stop is the retry the issue names: it is standing at GoalNode now, so the verb is
			// RedirectAgent (a Parked waiter's), which has no jump to make - and an aircraft that finds nothing costs
			// one pass, not one per frame.
			// ENFORCED BY: Airside.Model.Traffic.ReofferRefusedExtensionRetriesWhenItStops
			if (Agent.IsWaitingFor(EAgentWait::ForStand))
			{
				bStandsMayHaveFreed = true;
			}
			break;

		case EAgentEvent::Airborne:
			// THE RUNWAY IS FREE. A departure held the strip from the handover until Gone,
			// and Gone is the top of the climb - 300 m up, most of a minute after the wheels
			// left. In play (2026-09-06) every arrival dispatched in that minute was refused
			// "the runway is in use" while the strip sat empty. The strip is what the table
			// protects, and the strip is clear once the aircraft is established in the climb:
			// the next arrival joins its approach minutes out (FLandingRun, 1.9 km on a 100 m
			// glideslope) and cannot touch down under a climbing aircraft. Wake and separation
			// between successive movements are URunwaySequencer's (M3), not this table's.
			//
			// GUARDED, not unconditional: a departure from a junction turn path can reach
			// here holding nothing (LinedUp's own comment on DerivedFrom-less routes), and
			// releasing/logging a claim that was never made would be noise, not news.
			if (Agent.RunwayHeld.Num() > 0 || Agent.CrossingPhase != ECrossingPhase::None)
			{
				// Everything the departure held goes - RunwayHeld, the crossing it passed a
				// bar to reach - and the fields are reset so HoldRunwayOnly claims nothing back.
				Occupancy.ReleaseAll(Id);
				Agent.ReleaseRunway();
				Agent.EndCrossing();
				UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Id);
				// #169: the mirror of LinedUp's bump, above, and for the same reason - Departing
				// before and after Airborne, so the phase-changed broadcast below never fires.
				++OccupancyRevisionCount;
			}
			break;

		case EAgentEvent::None:
		case EAgentEvent::Gone:
		default:
			// Gone is unreachable here: Advance returns false the same frame it fires, and
			// that path already continued above. The rest - Stranded, the reverse leg's two, the
			// touchdown - change nothing this loop holds; they are announced or collected below.
			break;
		}

		// STOPPED AND WAITING, not merely stopped: an aircraft sitting out its shutdown pause
		// is not stalled, and neither is one crawling through a turn. All three conditions
		// together are what FDeadlockResolver::Resolve means by a waiter, and the clock
		// resets the moment any of them stops holding, so a junction wait that clears on its
		// own leaves nothing behind.
		//
		// FOR ANY AGENT ON A ROUTE, AT ITS SPEED ALONG THE PLAN (#455) - IsStoppedAndWaiting says why. This
		// used to be "Taxiing and Follower.Speed", so a refused reversing truck (whose follower is parked and
		// reads zero the whole leg) or pushed aeroplane never accrued and no jam through one could be seen.
		// ENFORCED BY: Airside.Model.Traffic.ReversingTruckStallsWhenItsSpanIsRefused
		if (Agent.IsStoppedAndWaiting())
		{
			Agent.AccrueStall(DeltaSeconds);
		}
		else
		{
			Agent.ResetStall();
		}

		// THE EVENT DECIDES, not a diff (#436): Advance named what it did, at most one handover a call, and the
		// transition carries that name as its Cause. One that moved no phase - a touchdown, the climb - is a MOMENT:
		// collected for the presenter (GetMomentsThisAdvance), not broadcast as a phase change nobody made.
		//
		// `|| Agent.Phase != Before` IS THE NET, not the rule: a phase change Advance did not name would otherwise go
		// unannounced, which is worse than announcing it with Cause None - which Announce logs as the defect it is.
		// ENFORCED BY: Airside.Model.RoadAgent.EveryPhaseChangeNamesItsEvent (no such change exists today)
		if (Event != EAgentEvent::None || Agent.Phase != Before)
		{
			const FAgentTransition Made = TransitionOf(Agent, Before, Event);
			if (Made.From == Made.To)
			{
				MomentsThisAdvance.Add(Made);
			}
			else
			{
				Announce(Made);
				// #169: covers the Parked handover above (Pass->ClaimGoalNode claimed the stand
				// earlier in this same iteration) and every other phase change that can move an
				// arrival's answer - Arriving ending, a departure starting its push. LinedUp and
				// Airborne bump for themselves, above, because they are the one case a phase change
				// does NOT accompany the occupancy change.
				++OccupancyRevisionCount;
			}
		}
	}

	// LAST, after every agent has claimed, moved and announced. The wait-for edges it reads
	// are then one frame's worth rather than a mixture of two, and an agent it replans starts
	// the next tick at the top of Arbitrate - which is the pass that re-claims for the new
	// plan. Running it before the motion loop would resolve on the previous frame's stalls
	// and hand the follower a plan the arbiter had not yet been asked about.
	if (Network != nullptr)
	{
		// CONTEXT IN PLACE OF FOUR POSITIONAL MEMBERS (issue #175) - Network, Rules, Occupancy
		// and NodeReach were four of Resolve's own seven parameters; see Model/TrafficContext.h.
		DeadlockResolver.Resolve(Agents, AgentIndex,
			FTrafficContext{*Network, Rules, Occupancy, NodeReach, RunwayChains, SimSeconds}, PlanReResolver);
	}

	// LAST OF ALL: a waiter is sent to a stand, or a held taxi out given a way to the runway, only once everyone has
	// claimed, moved and been replanned, so its new route starts the next tick at the top of Arbitrate like any other.
	// EVERY STEP, not only when a stand may have freed: the taxi-out arm gates itself per agent on the graph's revision.
	if (Network != nullptr)
	{
		RetryWaiters(*Network);
		StartDuePushes(*Network);
	}
}

int32 UGroundTraffic::CurrentStep(const FRoutePlan& Plan, double Travelled)
{
	// BINARY SEARCH, NOT A LEFT-TO-RIGHT WALK (issue #190): FClaimPass::WindowFor calls this
	// for every agent every substep, and Steps[N].EndDistance is CUMULATIVE polyline length
	// (RouteSearch.cpp builds it by appending each step's points in turn), so it is
	// monotonically non-decreasing along Plan.Steps - which is all a binary search for "the
	// first index where Travelled < EndDistance" needs.
	//
	// NOT A PERSISTED CURSOR, the shape the rest of this issue uses: T here is
	// FClaimPass::CentreOf, not Follower.Travelled, and CentreOf's own comment says why it
	// FLIPS SIGN in EAgentPhase::Manoeuvring - a push's centre moves the opposite way along
	// the plan from an ordinary taxi's. A cursor assumes the distance it walks only grows,
	// which is exactly what stops being true there; a binary search needs no such assumption
	// and gives the identical answer for whatever Travelled is asked, in either direction.
	int32 Lo = 0;
	int32 Hi = Plan.Steps.Num();
	while (Lo < Hi)
	{
		const int32 Mid = Lo + (Hi - Lo) / 2;
		if (Travelled < Plan.Steps[Mid].EndDistance)
		{
			Hi = Mid;
		}
		else
		{
			Lo = Mid + 1;
		}
	}

	if (Lo < Plan.Steps.Num())
	{
		return Lo;
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

// RankAt and ReachExcessAt MOVED TO FClaimPass (issue #84) - Model/TrafficClaims.h. Both are
// static there, taking Rules/Reach explicitly, so FDeadlockResolver::CanReplanAtBlockedStep
// can call FClaimPass::ReachExcessAt without standing up a whole claim pass just for it.

void UGroundTraffic::Arbitrate(const URoadNetwork& Network)
{
	// ONE PASS, SHARED ACROSS EVERY AGENT THIS CALL CLAIMS FOR (issue #84): FClaimPass carries
	// no state between agents - Rules, Occupancy, NodeReach and RunwayChains are references
	// to this class's own members - so one instance for the whole Arbitrate call is exactly
	// as correct as a fresh one per agent, and cheaper. RunwayChains (issue #170) makes that
	// sharing pay for a second thing too: two agents in the same Arbitrate call naming the
	// same runway seed now walk it once between them, not once each.
	// WITH THE TAXI PLANNING OWNER, whose order is the claim pass's one more refusal (taxi planning PR 2).
	FTrafficContext Context{Network, Rules, Occupancy, NodeReach, RunwayChains, SimSeconds};
	Context.Planning = TaxiPlanning;
	FClaimPass Pass{Context};

	// BY RANK, NOT BY LIST ORDER. Indices rather than a sorted copy of the agents: the claim
	// pass writes to the agents, so a copy would be arbitrating over stale ones.
	//
	// MEMBER, NOT A LOCAL (issue #190) - see ArbitrationOrder's own comment. Reset here
	// rather than left with whatever the last Arbitrate() call sorted.
	ArbitrationOrder.Reset();
	ArbitrationOrder.Reserve(Agents.Num());
	for (int32 Index = 0; Index < Agents.Num(); ++Index)
	{
		ArbitrationOrder.Add(Index);
	}
	ArbitrationOrder.Sort([this](int32 Left, int32 Right)
	{
		const int32 LeftRank = TraversalPriority(Agents[Left].Class);
		const int32 RightRank = TraversalPriority(Agents[Right].Class);

		// Ties to the lower id, which is first-to-be-dispatched. The per-node
		// PriorityOverride does NOT reorder this pass: it changes who wins a contested
		// resource (see FClaimPass::RankAt), not who is asked first, and a per-node rule
		// cannot decide a global order without asking every node about every agent.
		return LeftRank != RightRank ? LeftRank > RightRank : Agents[Left].Id < Agents[Right].Id;
	});

	for (const int32 Index : ArbitrationOrder)
	{
		Pass.Run(Agents[Index], Network);
	}

	// ONE RE-PASS over whoever lost a reservation to a higher rank during that pass. Without
	// it a preempted agent drives a whole frame on a reservation it no longer holds - into
	// the very node that was just taken from it.
	//
	// OUT-PARAM, NOT A RETURNED TSet (issue #190): TakePreempted used to return its find by
	// value, moving Occupancy's own set out and leaving it to rebuild from nothing the next
	// time a preemption actually happened. PreemptedScratch is this class's own member and
	// keeps its capacity across every Arbitrate() call instead.
	PreemptedScratch.Reset();
	Occupancy.TakePreempted(PreemptedScratch);
	for (const int32 AgentId : PreemptedScratch)
	{
		const int32 Index = FindIndex(AgentId);
		if (Index != INDEX_NONE)
		{
			Pass.Run(Agents[Index], Network);
		}
	}
}
