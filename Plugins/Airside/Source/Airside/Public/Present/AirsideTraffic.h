#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadAgent.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "AirsideTraffic.generated.h"

class URoadNetwork;
class ARoadAgentActor;

/**
 * One agent, plus the cube standing where it is.
 *
 * THE VIEW POINTER LIVES HERE, NOT ON FRoadAgent. FRoadAgent is Model/ - world-free, and
 * testable with no actor to spawn - and ARoadAgentActor is a level-resident view, so
 * pairing them is a Present-layer job. Putting View back on the model struct would be
 * exactly the dependency CLAUDE.md's layering rule forbids: Model reaching up to Present.
 *
 * Runtime only. Neither field is saved with the level - see UAirsideTraffic::Agents.
 */
USTRUCT()
struct AIRSIDE_API FAgentSlot
{
	GENERATED_BODY()

	UPROPERTY() FRoadAgent Agent;

	UPROPERTY() TObjectPtr<ARoadAgentActor> View = nullptr;

	/**
	 * Stable identity for the lifetime of the agent. Slot indices shift on RemoveAt, and a
	 * pointer to the view dies with it, so a listener that wants to say "the one I
	 * dispatched" needs a number nothing else reuses. Assigned from UAirsideTraffic::
	 * NextAgentId; 0 means unassigned and is never handed out.
	 */
	UPROPERTY() int32 Id = 0;
};

/**
 * Every aircraft under way: arrival dispatch, taxi dispatch, and the tick that advances them.
 *
 * Pattern: Mediator. An agent's journey is a handover between the runway, the taxi route and
 * the stand it ends at, and something has to own the list of who is mid-journey without
 * being the graph itself (an agent is a thing part way through a trip, not a fact about the
 * airport - see the old comment on ARoadNetworkActor::Agents, preserved on the field below)
 * or the mesh (agents share no geometry with the pavement they drive on). This class is that
 * something.
 *
 * A UCLASS(UObject) rather than a plain C++ class because FAgentSlot::View is a UObject
 * pointer the garbage collector must trace - a plain class holding TObjectPtr fields with no
 * UPROPERTY reflection is exactly how an actor's cube gets collected out from under it, the
 * bug FAgentSlot's own header comment warns about. Created with CreateDefaultSubobject on
 * the actor and held Transient: agents never reach disk - see Agents' own comment - so there
 * is nothing here a save would need.
 */
UCLASS()
class AIRSIDE_API UAirsideTraffic : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Fired on every phase change, including spawn (From == Gone) and removal (To == Gone).
	 * Gone-as-"did not exist" is the reading LastAgentPhaseForTest already gives it, so one
	 * convention covers both ends of an agent's life without a separate spawned/removed pair.
	 * A plain (non-dynamic) delegate: the only subscriber is C++ in AirportOps, which relays
	 * onto its own Blueprint-facing bus. Making this one dynamic too would be two Blueprint
	 * surfaces for one fact.
	 */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32 /*AgentId*/, EAgentPhase /*From*/, EAgentPhase /*To*/);
	FOnAgentPhaseChanged OnAgentPhaseChanged;

	/** Fired when DispatchArrival refuses, with the planner's reason. The log line stays too. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	/** Id of the most recently dispatched agent, or 0 when nothing is under way. */
	int32 GetNewestAgentId() const { return Agents.Num() > 0 ? Agents.Last().Id : 0; }

	/**
	 * Lands an aircraft on the runway nearest a point and taxis it to a stand.
	 *
	 * WHICH RUNWAY, WHICH EXIT AND WHICH STAND ARE DECIDED BY ArrivalPlanner::Plan, before
	 * anything is spawned - see its header. An arrival that cannot be completed leaves no
	 * aircraft in the world, rather than one frozen on final or rolling to a runway it has
	 * no way off. This function's own job is what is left once that choice is made: arm the
	 * landing, spawn the view, and log the plan's own refusal or success - never re-derive
	 * either.
	 *
	 * SurfaceZ and ShutdownPauseSeconds are ARoadNetworkActor's own level-authored tunables,
	 * handed in rather than read back through Outer: FRoadAgent is world-free and cannot read
	 * them for itself, so somebody must copy them in at dispatch, and passing them explicitly
	 * says so at the call site instead of hiding it behind a back-pointer this class does not
	 * otherwise need.
	 */
	bool DispatchArrival(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe,
		double SurfaceZ, double ShutdownPauseSeconds);

	/**
	 * Sends one agent along a plan, spawning the cube that shows it. False if it cannot.
	 *
	 * Network may be null - a route with nowhere to check for a runway simply taxis, which
	 * is what every route did before departures existed. See ARoadNetworkActor::DispatchAgent
	 * for why the whole AIRFRAME is taken rather than its performance structs one at a time.
	 */
	bool DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan, const FAirframe& Airframe,
		double SurfaceZ, double ShutdownPauseSeconds);

	/**
	 * Sends an EXISTING agent along a new plan, keeping its id and its view.
	 *
	 * The seam AirportOps composes "go to the stand, dwell, return to the depot" from
	 * (spec §2.0, amended by the M1 plan): DispatchAgent, then on the Parked event wait the
	 * dwell on the sim clock, then this, then RetireAgent on the second Parked. Dwell lives
	 * with the job, not here, because how long a fuel truck stays is a fact about the fuel
	 * job, and movement should not have to be told about jobs.
	 *
	 * Accepted in Parked or Taxiing (a mid-route redirect is a replan). Refused in Arriving
	 * or Departing - an aircraft on the runway is not something a job can redirect - and for
	 * an unknown id. Arms a departure if the new route ends on a runway, exactly as
	 * DispatchAgent does, through the same helper.
	 */
	bool RedirectAgent(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan);

	/**
	 * Removes an agent and its view immediately, announcing <phase> -> Gone. For a service
	 * vehicle that has returned to its depot: it does not fly away, so nothing else would
	 * ever remove it. False for an unknown id.
	 */
	bool RetireAgent(int32 AgentId);

	/** Removes every agent and its cube. */
	void ClearAgents();

	/** How many agents are currently under way or parked at their destination. */
	int32 GetAgentCount() const { return Agents.Num(); }

	/**
	 * The most recently dispatched agent's actor, or null when nothing is under way.
	 *
	 * The NEWEST rather than the nearest or the first: the one you just sent is the one you
	 * want to watch, and any other rule makes "follow it" mean something different depending
	 * on what else happens to be taxiing.
	 */
	ARoadAgentActor* GetNewestAgent() const
	{
		return Agents.Num() > 0 ? Agents.Last().View.Get() : nullptr;
	}

	/**
	 * Advance every agent by DeltaSeconds and hand the model's answer to its view, dropping
	 * an agent once its own FRoadAgent::Advance says Gone.
	 *
	 * Called from ARoadNetworkActor::Tick, which owns the actual tick registration - this is
	 * a plain function, not an override, because a UObject Tick needs FTickableGameObject or
	 * similar machinery this class has no other use for.
	 */
	void Advance(float DeltaSeconds, double SurfaceZ);

	/**
	 * The newest agent's Phase, for Airside.Present.ArrivalDispatch - which drives a real
	 * Tick loop and needs to see it move Arriving -> Taxiing -> Parked, not just that the
	 * agent still exists. EAgentPhase::Gone when there is no newest agent: the same phase an
	 * agent itself ends in, which reads correctly as "nothing here to ask" either way.
	 */
	EAgentPhase LastAgentPhaseForTest() const
	{
		return Agents.Num() > 0 ? Agents.Last().Agent.Phase : EAgentPhase::Gone;
	}

	/**
	 * The newest agent's OWN taxi speed cap, for the same test - reading FRoadAgent::
	 * Follower::Ground rather than the FAirframe the caller dispatched with, so this proves
	 * the handover in FRoadAgent::Advance (Airframe.Ground copied into the follower at the
	 * VACATED handover) actually reached the struct that drives the taxi, not merely that
	 * DispatchArrival was handed the right number.
	 */
	double LastAgentTaxiSpeedCapForTest() const
	{
		return Agents.Num() > 0 ? Agents.Last().Agent.Follower.Ground.Taxi.SpeedCap : 0.0;
	}

	/** The newest agent's last shown position, road plane, for the redirect test. Zero when none. */
	FVector2D LastAgentPositionForTest() const
	{
		return Agents.Num() > 0 ? Agents.Last().Agent.LastMotion.Position : FVector2D::ZeroVector;
	}

private:
	/**
	 * Runtime only, and deliberately not part of URoadNetwork. An agent is a thing part way
	 * through a journey, not a fact about the airport: putting them in the network would
	 * snapshot them into every undo Memento and serialise them into the saved level, so
	 * re-opening a map would restore half-driven cubes that no longer have a route.
	 */
	UPROPERTY(Transient) TArray<FAgentSlot> Agents;

	/** Next FAgentSlot::Id to hand out. Transient: ids are per-session, agents never reach disk. */
	UPROPERTY(Transient) int32 NextAgentId = 1;

	/** Assigns the slot's id, stores it, and announces the spawn. The one place both happen. */
	void Admit(FAgentSlot&& Slot);

	int32 FindSlot(int32 AgentId) const;

	/**
	 * Arms a departure when Plan ends on a runway. Pulled out of DispatchAgent so
	 * RedirectAgent gets the identical rule; two copies would be the drift this codebase's
	 * "one struct per thing" rule exists to prevent.
	 */
	void ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const;
};
