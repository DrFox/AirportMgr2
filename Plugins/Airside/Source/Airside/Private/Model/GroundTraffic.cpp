// Dispatch, admission, redirect/retire, the registry, the tick, events, and the plan/step
// helpers everything else reads. FClaimPass (TrafficClaims.cpp), FDeadlockResolver
// (GroundTrafficDeadlock.cpp) and FPlanReResolver (GroundTrafficRebuild.cpp) hold the rest -
// see Model/GroundTraffic.h for the map (issue #84 split these off by responsibility, not
// just by file).

#include "Model/GroundTraffic.h"

#include "AirsideLog.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/PushbackPlanner.h"
#include "Model/PushbackRun.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficClaims.h"
#include "Solve/GuidelineGeom.h"
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

double FTrafficRules::PushSpeedFor(EPushbackNeed Need) const
{
	// A SWITCH AND NOT A TERNARY CHAIN, deliberately, and unlike the two functions above -
	// which have two cases and a documented "everything else" rule. This is the one place
	// that must agree with EPushbackNeed, so a value added to that enum has to produce a
	// compiler warning here rather than fall quietly into an else and push an A320 at a hand
	// tug's pace. The codebase's "lists that must agree are ONE list", applied to arithmetic.
	switch (Need)
	{
	case EPushbackNeed::SelfManoeuvre: return SelfManoeuvrePushSpeed;
	case EPushbackNeed::HandTug:       return HandTugPushSpeed;
	case EPushbackNeed::VehicleTug:    return VehicleTugPushSpeed;
	}

	// Unreachable while the switch is total. The CONSERVATIVE answer anyway, matching
	// FAirframe::PushbackNeed's own default: slowest is never unsafe.
	return HandTugPushSpeed;
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
			*RunwayDesignator::ToPairText(Plan.End.Direction), Plan.End.Length, Plan.Needed,
			Plan.ExitCount, Network.GetEntities().Num());
	}

	if (!Plan.IsValid())
	{
		UE_LOG(LogAirsideTraffic, Warning, TEXT("%s"), *ArrivalPlanner::DescribeRefusal(Plan));
		OnArrivalRefused.Broadcast(Plan.Why);
		return 0;
	}

	FRoadAgent Agent;
	if (!Agent.StartArrival(Plan.End, Airframe, Plan.VacateAt, Plan.TaxiIn))
	{
		// FLandingRun has already logged why. Nothing is admitted: an arrival that cannot be
		// flown must leave no aircraft in the world, rather than one frozen on final.
		return 0;
	}

	// FRoadAgent is world-free and cannot read the actor's UPROPERTY, so the pause is copied
	// in here - the only time the two ever need to meet.
	Agent.ShutdownPause = ShutdownPauseSeconds;
	Agent.Class = ETraversalClass::Aircraft;
	Agent.SetGoalFrom(Plan.TaxiIn);

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
		*RunwayDesignator::ToPairText(Plan.End.Direction), Plan.End.Length, Plan.Needed,
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
		Occupancy.Assert(FTrafficClaim::Make(Id, FTrafficResource::OfSurface(Segment),
			/*bOccupied*/ true, TraversalPriority(ETraversalClass::Aircraft)));
	}

	// AND THE STAND, for the same between-ticks reason. Agents.Last() is the agent Admit
	// just appended.
	ClaimGoalNodeAtDispatch(Agents.Last(), Id, Network);

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
	Agent.SetGoalFrom(Plan);

	ArmDepartureIfRunway(Agent, Network, Plan);

	// Posed before its first tick: a zero-second Advance asks where the taxi starts without
	// moving it, so LastMotion is a full pose - heading included - by the time the view is
	// spawned off the Admit broadcast below. StartTaxi's fallback covers a plan too short.
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
		// looked up at the handover, because by then the graph may have been rebuilt.
		Agent.DepartureRunway = Network->RunwayChain(End.Seed);

		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Route ends on runway %s %.0f uu past the threshold: %.0f uu available, departure armed"),
			*RunwayDesignator::ToPairText(End.Direction), EntryOffset, End.Length - EntryOffset);
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

int32 UGroundTraffic::HolderOfNode(FGuidelineNodeId Node) const
{
	int32 Holder = 0;
	Occupancy.IsHeld(FTrafficResource::OfNode(Node), 0, &Holder);
	return Holder;
}

const TArray<FVector2D>& UGroundTraffic::RemainingRoute(int32 AgentId) const
{
	const FRoadAgent* Agent = FindAgent(AgentId);
	if (Agent == nullptr || Agent->Phase != EAgentPhase::Taxiing)
	{
		static const TArray<FVector2D> Empty;
		return Empty;
	}
	return Agent->Follower.Plan.Polyline;
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

	// EXACTLY WHAT THE Vacated HANDOVER ARMS (see Advance): the seed and the phase, together
	// through BeginCrossing. No claim is raised here - the next Arbitrate raises it, as it
	// does in play.
	Agents[Index].BeginCrossing(RunwaySeed, ECrossingPhase::OnStrip);
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

	// THE OLD STAND FREES NOW, between ticks: the next claim pass would drop it anyway
	// (ClaimGoalNode reads the new goal), but a planner asking in this frame must see it
	// free. And the model notes that a stand may have freed, for the re-offer pass.
	if (Agent.GoalNode.IsSet())
	{
		Occupancy.Release(AgentId, FTrafficResource::OfNode(Agent.GoalNode));
		bStandsMayHaveFreed = true;
	}

	// CAPTURED BEFORE StartTaxi, which always primes a cold start of its own
	// (bEngineRunning=true, EngineRPM=0.0) - so these are whether the engine was running and
	// what RPM it actually had a moment ago, not the post-StartTaxi state that call is about
	// to overwrite both with.
	const bool bWasRunning = Agent.bEngineRunning;
	const double PriorRPM = Agent.EngineRPM;

	Agent.StartTaxi(Plan, Own);

	if (bWasRunning)
	{
		// AND THE ENGINE IS ALREADY TURNING. StartTaxi starts from cold, which is right for a
		// plain dispatch and wrong for an aeroplane that was already taxiing, or has spent a
		// turnaround on a stand where its engines were started long before it rolled and
		// never stopped. Left cold, the propeller was still winding up while the aircraft
		// taxied out at full speed - reported from play as the prop never having time to spin
		// up on departure.
		Agent.StartEngineAtSpeed();
	}
	else
	{
		// THE ENGINE WAS NOT RUNNING (#107 item 2) - DepartAgent on a parked aircraft that ran
		// out its post-arrival shutdown pause, or ReofferStands on one that is stranded and
		// parked with its own shutdown countdown running. StartEngineAtSpeed's own header says
		// what it is FOR - "as it is for an aeroplane that has spent a turnaround ... before it
		// taxied out" - which presumes the engine was already running; calling it
		// unconditionally snapped a stopped propeller straight to full power in one frame,
		// with no spool-up at all.
		//
		// PriorRPM RESTORED, NOT LEFT AT ZERO: bEngineRunning false does not mean the
		// propeller has actually stopped turning - AdvanceEngine spools it DOWN over
		// SpoolDownSeconds, so a redirect that lands mid-decay (the ReofferStands case above)
		// still has real RPM on it. StartTaxi's own cold start just wrote EngineRPM=0.0 over
		// that, which would have snapped a spooling-down propeller to a dead stop and then
		// spooled it back UP from zero - the same one-frame snap this fix exists to remove,
		// only downward first. Restoring it here means AdvanceEngine picks up the ramp exactly
		// where it actually was, whichever direction it was headed.
		Agent.EngineRPM = PriorRPM;
	}

	// Class is NOT re-derived: a van redirected is still a van. StartTaxi rewrites the
	// follower and the airframe and nothing else, so the identity fields survive it; only
	// the goal moves, because that is the whole of what a redirect changes.
	Agent.SetGoalFrom(Plan);
	ArmDepartureIfRunway(Agent, Network, Plan);
	if (Network != nullptr)
	{
		ClaimGoalNodeAtDispatch(Agent, AgentId, *Network);
	}

	// POSED NOW, NOT LEFT FOR THE NEXT TICK (#107 item 3) - the same reason DispatchAgent runs
	// a zero-second Advance before Admit. UGroundTraffic::Advance early-returns on a paused
	// frame (DeltaSeconds <= 0), so nothing would otherwise call FRoadAgent::Advance for the
	// rest of one - and StartTaxi's own LastMotion reset above is a bare FAgentMotion() with
	// only Position filled in: heading 0 regardless of which way this route actually goes,
	// EngineRPM 0 regardless of what StartEngineAtSpeed just wrote into Agent.EngineRPM.
	// UAirsideTraffic::Advance poses the view off exactly this field every tick, paused ones
	// included, so a player redirecting while paused would see the aeroplane facing east with
	// a stopped propeller until play resumed, whichever way the new route actually points.
	FAgentMotion Motion;
	EAgentEvent Event;
	Agent.Advance(0.0, Motion, Event);

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
	FRoadAgent& Agent = Agents[Index];
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
		GuidelineGeom::PointAtDistance(Plan.Route.Polyline, 0.0, StartAt, OutTangent);

	// 180 WHEN THE POLYLINE CANNOT SAY - honouring the return rather than reading an
	// uninitialised double. A route with no direction is not a route that leads straight out.
	const double OffDegrees = bHaveTangent
		? FMath::Abs(FMath::RadiansToDegrees(
			FMath::UnwindRadians(OutTangent - Agent.LastMotion.Heading)))
		: 180.0;

	if (bHaveTangent && OffDegrees <= Rules.StraightOutDegrees)
	{
		// THE MEASURED ANGLE IS IN THE LINE. When a player asks why an aeroplane did not push
		// back, the angle that was measured IS the answer, and guessing it back out of the
		// geometry costs a PIE session - which is the one thing this project's notes say to
		// spend log lines on.
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d departs straight out of its stand (%.0f deg off the parked heading)"),
			AgentId, OffDegrees);
		return RedirectAgent(AgentId, &Network, Plan.Route)
			? EDepartureRefusal::None : EDepartureRefusal::NoRoute;
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
	const FPushbackPlan Push = PushbackPlanner::Plan(Network, Agent.GoalNode, Plan,
		Agent.Airframe, Agent.Class,
		Rules.FootprintFor(Agent.Class) + Rules.GapFor(Agent.Class));

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

	// PUSHBACK CLEARANCE: granted whole, or withheld. A manoeuvring agent cannot replan -
	// there is no alternative way off a stand - so if the deadlock resolver ever picked one it
	// would have no move to make. Granting the whole push up front makes it atomic and removes
	// it as a deadlock source, and it is what ground control actually does: clearance is
	// granted or withheld, never half-granted.
	if (!IsPushGroundFree(AgentId, Push.PushRoute, Push.PushRoute.Length))
	{
		// AT Log AND NOT Warning: a taxiway the player has left busy refuses this for as long
		// as they leave it, and it clears itself. UFuelService::DepartTheReady already
		// throttles its own line to a CHANGE of reason, which keeps this from filling the file.
		UE_LOG(LogAirsideTraffic, Log,
			TEXT("Agent %d cannot push back yet: %.0f uu of ground is not free."),
			AgentId, Push.PushRoute.Length);
		return EDepartureRefusal::PushbackBlocked;
	}

	const EAgentPhase Before = Agent.Phase;
	if (!Agent.StartPushback(Push.PushRoute, Push.TaxiOutRoute, Agent.Airframe,
		Rules.PushSpeedFor(Agent.Airframe.PushbackNeed), Rules.PushAccel,
		Agent.Airframe.Engine.MaxRPM * Rules.PowerbackRPMFraction))
	{
		return EDepartureRefusal::NoRoute;
	}

	// THE GOAL IS THE TAXI OUT'S, not the push's. The claim pass reserves the node an agent is
	// heading FOR, and a push that claimed its own end would have the aeroplane reserving a
	// patch of taxiway as though it were a stand. What it is going to is the runway.
	Agent.SetGoalFrom(Push.TaxiOutRoute);
	ArmDepartureIfRunway(Agent, &Network, Push.TaxiOutRoute);
	ClaimGoalNodeAtDispatch(Agent, AgentId, Network);

	// THE NEED IS NAMED even though nothing branches on it yet. Slice 1 pushes all three the
	// same way and nobody is doing the pushing, so this line is the only place the gap between
	// "needs a tug" and "has one" is visible at all.
	UE_LOG(LogAirsideTraffic, Log,
		TEXT("Agent %d pushing back %.0f uu, then %.0f uu to taxi out, %s"),
		AgentId, Push.PushRoute.Length, Push.TaxiOutRoute.Length,
		*UEnum::GetValueAsString(Agent.Airframe.PushbackNeed));

	OnAgentPhaseChanged.Broadcast(AgentId, Before, Agent.Phase);
	return EDepartureRefusal::None;
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
	bStandsMayHaveFreed = true;

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
	return Occupancy.TryClaim(Claim, Blocker) == EClaimResult::Granted;
}

void UGroundTraffic::ReleaseHold(int32 HolderId)
{
	Occupancy.ReleaseReservations(HolderId);
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
	}

	// ONE INSTANCE FOR THE HANDOVER CLAIM BELOW (issue #84): the only FClaimPass call in this
	// loop is ClaimGoalNode at the Taxiing -> Parked handover, so this is cheaper than
	// constructing one per agent and exactly as correct - see Arbitrate's own instance for why.
	FClaimPass Pass{Rules, Occupancy, NodeReach};

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
		EAgentEvent Event;
		if (!Agent.Advance(DeltaSeconds, Motion, Event))
		{
			// Cleared or otherwise finished - the aircraft has gone, so everything it held
			// goes with it. An agent that stayed in the table would hold a runway nothing
			// could ever release.
			Occupancy.ReleaseAll(Id);
			bStandsMayHaveFreed = true;
			Agents.RemoveAt(Index);
			// Broadcast AFTER the removal so a listener that asks GetAgentCount sees the
			// agent already gone, which is what "To == Gone" promises.
			OnAgentPhaseChanged.Broadcast(Id, Before, EAgentPhase::Gone);
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
			Agent.RunwayHeld.Reset();
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
			break;

		case EAgentEvent::Parked:
			// THE STAND IS OCCUPIED IN THE TICK THE AIRCRAFT PARKS, by the same rule as the
			// runway above: the claim pass ran before motion, while this agent was still
			// Taxiing, so its stand claim is the inbound reservation until the next pass -
			// and the panel would read "Reserved for" over an aircraft standing on the stand
			// for one frame.
			if (Network != nullptr)
			{
				Pass.ClaimGoalNode(Agent, *Network);
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
				Agent.RunwayHeld.Reset();
				Agent.EndCrossing();
				UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d released the runway"), Id);
			}
			break;

		case EAgentEvent::None:
		case EAgentEvent::Gone:
		default:
			// Gone is unreachable here: Advance returns false the same frame it fires, and
			// that path already continued above.
			break;
		}

		// STOPPED AND WAITING, not merely stopped: an aircraft sitting out its shutdown pause
		// is not stalled, and neither is one crawling through a turn. All three conditions
		// together are what FDeadlockResolver::Resolve means by a waiter, and the clock
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
		DeadlockResolver.Resolve(Agents, *Network, Rules, Occupancy, NodeReach, PlanReResolver, SimSeconds);
	}

	// LAST OF ALL: a waiter is sent to a stand only once everyone has claimed, moved and been
	// replanned, so its redirect starts the next tick at the top of Arbitrate like any other.
	if (bStandsMayHaveFreed && Network != nullptr)
	{
		ReofferStands(*Network);
	}
}

void UGroundTraffic::ReofferStands(const URoadNetwork& Network)
{
	// ONE PASS, then the flag clears whether or not anyone was placed: a waiter that still
	// found nothing will be asked again the next time something frees, not every frame.
	bStandsMayHaveFreed = false;
	TArray<int32> Waiting;
	for (const FRoadAgent& Agent : Agents)
	{
		if (Agent.bAwaitingStand && Agent.GoalNode.IsSet()
			&& (Agent.Phase == EAgentPhase::Parked || Agent.Phase == EAgentPhase::Taxiing))
		{
			Waiting.Add(Agent.Id);
		}
	}
	// By id, not by reference: RedirectAgent writes into Agents and broadcasts.
	for (const int32 Id : Waiting)
	{
		const FRoadAgent* Agent = FindAgent(Id);
		if (Agent == nullptr)
		{
			continue;
		}
		FRoutePlan Route;
		const FGuidelineNodeId Stand = ArrivalPlanner::ChooseStand(Network, Agent->GoalNode, Agent->Airframe, &Occupancy, Id, &Route);
		if (!Stand.IsSet() || !Route.IsValid())
		{
			continue;
		}
		if (RedirectAgent(Id, &Network, Route))
		{
			Agents[FindIndex(Id)].bAwaitingStand = false;
			UE_LOG(LogAirsideTraffic, Log, TEXT("Agent %d: a stand freed; sent to the stand at node %d"), Id, Stand.Index);
		}
	}
	// RedirectAgent re-raised the flag on releasing the old goal; nothing else has changed
	// since this pass started, so it is cleared again rather than costing an empty pass.
	bStandsMayHaveFreed = false;
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

// RankAt and ReachExcessAt MOVED TO FClaimPass (issue #84) - Model/TrafficClaims.h. Both are
// static there, taking Rules/Reach explicitly, so FDeadlockResolver::CanReplanAtBlockedStep
// can call FClaimPass::ReachExcessAt without standing up a whole claim pass just for it.

void UGroundTraffic::Arbitrate(const URoadNetwork& Network)
{
	// ONE PASS, SHARED ACROSS EVERY AGENT THIS CALL CLAIMS FOR (issue #84): FClaimPass carries
	// no state between agents - Rules, Occupancy and NodeReach are references to this class's
	// own members - so one instance for the whole Arbitrate call is exactly as correct as a
	// fresh one per agent, and cheaper.
	FClaimPass Pass{Rules, Occupancy, NodeReach};

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
		// resource (see FClaimPass::RankAt), not who is asked first, and a per-node rule
		// cannot decide a global order without asking every node about every agent.
		return LeftRank != RightRank ? LeftRank > RightRank : Agents[Left].Id < Agents[Right].Id;
	});

	for (const int32 Index : Order)
	{
		Pass.Run(Agents[Index], Network);
	}

	// ONE RE-PASS over whoever lost a reservation to a higher rank during that pass. Without
	// it a preempted agent drives a whole frame on a reservation it no longer holds - into
	// the very node that was just taken from it.
	for (const int32 AgentId : Occupancy.TakePreempted())
	{
		const int32 Index = FindIndex(AgentId);
		if (Index != INDEX_NONE)
		{
			Pass.Run(Agents[Index], Network);
		}
	}
}
