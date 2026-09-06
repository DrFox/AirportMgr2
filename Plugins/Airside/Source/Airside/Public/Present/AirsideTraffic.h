#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "AirsideTraffic.generated.h"

class URoadNetwork;
class ARoadAgentActor;

/**
 * Every aircraft under way, seen from Present/: the cube that shows each one, and the relay
 * that carries the model's events across the Present/Model boundary.
 *
 * THE MEDIATION ITSELF LIVES IN UGroundTraffic now. An agent's journey is a handover between
 * the runway, the taxi route and the stand it ends at, and something has to own the list of
 * who is mid-journey without being the graph itself (an agent is a thing part way through a
 * trip, not a fact about the airport) or the mesh (agents share no geometry with the pavement
 * they drive on) - but none of that needs a world, so M2 moved it down to Model/ where a test
 * can NewObject it. What is left here is the VIEW REGISTRY and the relay: the same shape the
 * ops plugin's own runtime object has above it - a Present-layer object that owns actors and
 * re-broadcasts a world-free model's events to the game module.
 *
 * A UCLASS(UObject) rather than a plain C++ class because Views holds UObject pointers the
 * garbage collector must trace - a plain class holding TObjectPtr fields with no UPROPERTY
 * reflection is exactly how an actor's cube gets collected out from under it. Created with
 * CreateDefaultSubobject on the actor and held Transient: agents never reach disk - see
 * UGroundTraffic::Agents - so there is nothing here a save would need.
 */
UCLASS()
class AIRSIDE_API UAirsideTraffic : public UObject
{
	GENERATED_BODY()

public:
	UAirsideTraffic();

	/**
	 * Re-points Model at THIS object's own subobject, and binds the relay to it.
	 *
	 * The same duplication rule ARoadNetworkActor::PostInitProperties documents: a duplicate
	 * (play-in-editor's copy of the level, or copy/paste) arrives holding the CDO's
	 * subobject, because InitProperties overwrites every Transient non-instanced pointer
	 * with the class default's value. The BINDING lives here rather than in the constructor
	 * for the same reason - bound in the constructor it would be a binding to whichever
	 * model the CDO made, and the duplicate's own model would broadcast to nobody.
	 */
	virtual void PostInitProperties() override;

	/**
	 * Fired on every phase change, including spawn (From == Gone) and removal (To == Gone).
	 * Gone-as-"did not exist" is the reading LastAgentPhaseForTest already gives it, so one
	 * convention covers both ends of an agent's life without a separate spawned/removed pair.
	 * A plain (non-dynamic) delegate: the only subscriber is C++ in AirportOps, which relays
	 * onto its own Blueprint-facing bus. Making this one dynamic too would be two Blueprint
	 * surfaces for one fact.
	 *
	 * RELAYED from UGroundTraffic::OnAgentPhaseChanged. AirportOps binds HERE, not to the
	 * model: the game module reaches the airport through the actor, and a subscriber that
	 * had to walk down into Model/ would be reaching past the composition root.
	 */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32 /*AgentId*/, EAgentPhase /*From*/, EAgentPhase /*To*/);
	FOnAgentPhaseChanged OnAgentPhaseChanged;

	/** Fired when DispatchArrival refuses, with the planner's reason. Relayed from the model,
	 *  which logs it too. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	/** The agents themselves, for a caller that wants the model rather than the view. */
	UGroundTraffic* GetModel() const { return Model; }

	/** Id of the most recently dispatched agent, or 0 when nothing is under way. */
	int32 GetNewestAgentId() const;

	/**
	 * Lands an aircraft on the runway nearest a point and taxis it to a stand.
	 *
	 * WHICH RUNWAY, WHICH EXIT AND WHICH STAND ARE DECIDED BY ArrivalPlanner::Plan, through
	 * UGroundTraffic::DispatchArrival - see its header. This is left with the view.
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
	 *
	 * Class defaults to Aircraft, which is what every caller before M2 meant.
	 */
	bool DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan, const FAirframe& Airframe,
		double SurfaceZ, double ShutdownPauseSeconds, ETraversalClass Class = ETraversalClass::Aircraft);

	/**
	 * Sends an EXISTING agent along a new plan, keeping its id and its view.
	 *
	 * The seam AirportOps composes "go to the stand, dwell, return to the depot" from
	 * (spec 2.0, amended by the M1 plan): DispatchAgent, then on the Parked event wait the
	 * dwell on the sim clock, then this, then RetireAgent on the second Parked. Dwell lives
	 * with the job, not here, because how long a fuel truck stays is a fact about the fuel
	 * job, and movement should not have to be told about jobs.
	 *
	 * See UGroundTraffic::RedirectAgent for which phases accept it and why.
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
	int32 GetAgentCount() const;

	/**
	 * The most recently dispatched agent's actor, or null when nothing is under way.
	 *
	 * The NEWEST rather than the nearest or the first: the one you just sent is the one you
	 * want to watch, and any other rule makes "follow it" mean something different depending
	 * on what else happens to be taxiing.
	 */
	ARoadAgentActor* GetNewestAgent() const;

	/**
	 * Advance every agent by DeltaSeconds and hand the model's answer to its view.
	 *
	 * Called from ARoadNetworkActor::Tick, which owns the actual tick registration - this is
	 * a plain function, not an override, because a UObject Tick needs FTickableGameObject or
	 * similar machinery this class has no other use for.
	 *
	 * Network is what the model arbitrates over; null means every agent drives as if alone.
	 * Passed per call and never held, so this object cannot outlive the graph.
	 */
	void Advance(float DeltaSeconds, double SurfaceZ, const URoadNetwork* Network);

	/**
	 * The newest agent's Phase, for Airside.Present.ArrivalDispatch - which drives a real
	 * Tick loop and needs to see it move Arriving -> Taxiing -> Parked, not just that the
	 * agent still exists. EAgentPhase::Gone when there is no newest agent: the same phase an
	 * agent itself ends in, which reads correctly as "nothing here to ask" either way.
	 */
	EAgentPhase LastAgentPhaseForTest() const;

	/**
	 * The newest agent's OWN taxi speed cap, for the same test - reading FRoadAgent::
	 * Follower::Ground rather than the FAirframe the caller dispatched with, so this proves
	 * the handover in FRoadAgent::Advance (Airframe.Ground copied into the follower at the
	 * VACATED handover) actually reached the struct that drives the taxi, not merely that
	 * DispatchArrival was handed the right number.
	 */
	double LastAgentTaxiSpeedCapForTest() const;

	/** The newest agent's last shown position, road plane, for the redirect test. Zero when none. */
	FVector2D LastAgentPositionForTest() const;

private:
	/** The Mediator. CreateDefaultSubobject in the constructor, re-pointed by name in
	 *  PostInitProperties - the duplication rule ARoadNetworkActor already follows. */
	UPROPERTY(Transient) TObjectPtr<UGroundTraffic> Model;

	/**
	 * Agent id -> the cube standing where it is. THE VIEW POINTER LIVES HERE, NOT ON
	 * FRoadAgent: the agent is Model/, world-free and testable with no actor, and the view
	 * is a level-resident actor, so pairing them is a Present-layer job. Putting View on the
	 * model struct would be exactly the dependency CLAUDE.md's layering rule forbids: Model
	 * reaching up to Present. A map keyed by id rather than a parallel array, because the
	 * model's array reorders on removal.
	 *
	 * Runtime only. Nothing here is saved with the level - see UGroundTraffic::Agents.
	 */
	UPROPERTY(Transient) TMap<int32, TObjectPtr<ARoadAgentActor>> Views;

	/** The last SurfaceZ a caller gave, so a view spawned off a phase event can be posed. */
	UPROPERTY(Transient) double SurfaceZ = 0.0;

	void OnModelPhaseChanged(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void OnModelArrivalRefused(EArrivalRefusal Why);

	void SpawnView(int32 AgentId);
	void DestroyView(int32 AgentId);
};
