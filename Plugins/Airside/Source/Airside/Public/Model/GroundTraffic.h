#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/RoadAgent.h"
#include "Model/RoadTraffic.h"
#include "Model/TrafficOccupancy.h"
#include "GroundTraffic.generated.h"

class URoadNetwork;

/**
 * The numbers the arbiter works with. Spec 2026-09-06 §2.3.
 *
 * Footprint and gap live HERE, per class, and not on FAirframe: the airframe has no
 * length figure today, and a second copy of a performance number is the drift this
 * codebase's "one struct per thing" rule exists to prevent. A per-type length is a later
 * refinement with one owner.
 */
USTRUCT()
struct AIRSIDE_API FTrafficRules
{
	GENERATED_BODY()

	/** How much of the line an agent's body covers, uu. Half ahead of Travelled, half behind. */
	UPROPERTY(EditAnywhere) double AircraftFootprint = 1000.0;
	UPROPERTY(EditAnywhere) double VehicleFootprint = 500.0;

	/** Clear line kept ahead of the nose, beyond the braking distance, uu. */
	UPROPERTY(EditAnywhere) double AircraftGap = 1500.0;
	UPROPERTY(EditAnywhere) double VehicleGap = 300.0;

	/** Weight on held length in the routing cost. See FRouteQuery::CongestionWeight. */
	UPROPERTY(EditAnywhere) double CongestionWeight = 2.0;

	/** Stopped-and-waiting this long before deadlock detection looks. A normal junction
	 *  wait must never trip it. */
	UPROPERTY(EditAnywhere) double StallSeconds = 3.0;

	/** An unresolvable waiter re-tries its replan this often, sim seconds. */
	UPROPERTY(EditAnywhere) double RetrySeconds = 5.0;

	/** After a graph rebuild, how near a live node must be to a step's end to be it. */
	UPROPERTY(EditAnywhere) double ResolveRadius = 25.0;

	double FootprintFor(ETraversalClass Class) const;
	double GapFor(ETraversalClass Class) const;
};

/**
 * Every agent under way, the reservation table, and the tick that arbitrates between
 * them. Spec 2026-09-06 §2.2, §3.
 *
 * Pattern: Mediator - the one UAirsideTraffic was, moved down a layer. It moved because the
 * milestone's three tests ("two agents converge on a node; one yields", and the rest) must
 * run with NewObject and no world, and every multi-agent test before this one had to
 * UWorld::CreateWorld because the agent list lived in Present/ beside its view pointers.
 * Free functions over a hand-built agent array were considered and rejected: the ORDER of
 * claims is what makes aircraft-over-vehicle true, and a test that built its own order
 * would not be testing the tick.
 *
 * An agent's journey is a handover between the runway, the taxi route and the stand it ends
 * at, and something has to own the list of who is mid-journey without being the graph itself
 * (an agent is a thing part way through a trip, not a fact about the airport - see Agents
 * below) or the mesh (agents share no geometry with the pavement they drive on). This class
 * is that something.
 *
 * WORLD-FREE. The network is passed to Advance per call and never held, so this object
 * cannot outlive the graph it arbitrates over and needs nothing from an actor.
 *
 * A UObject rather than a USTRUCT so the table and the agents are GC-visible UPROPERTYs
 * on something UAirsideTraffic can own by CreateDefaultSubobject, and so a test can
 * NewObject one. Transient throughout: agents never reach disk (see Agents).
 */
UCLASS()
class AIRSIDE_API UGroundTraffic : public UObject
{
	GENERATED_BODY()

public:
	/** See UAirsideTraffic::OnAgentPhaseChanged, which relays this one layer up. */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32 /*AgentId*/, EAgentPhase /*From*/, EAgentPhase /*To*/);
	FOnAgentPhaseChanged OnAgentPhaseChanged;

	/** Fired when DispatchArrival refuses, with the planner's reason. The log line stays too. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	UPROPERTY(EditAnywhere, Category = "Airside|Traffic") FTrafficRules Rules;

	/**
	 * Lands an aircraft on the runway nearest Near and taxis it to a stand. Returns the new
	 * agent's id, or 0 - having broadcast OnArrivalRefused with the planner's reason. The
	 * runway chain is held from here until Vacated. Occupied runway: RunwayOccupied.
	 *
	 * WHICH RUNWAY, WHICH EXIT AND WHICH STAND ARE DECIDED BY ArrivalPlanner::Plan, before
	 * anything is admitted - see its header. An arrival that cannot be completed leaves no
	 * aircraft in the world, rather than one frozen on final or rolling to a runway it has
	 * no way off. This function's own job is what is left once that choice is made: arm the
	 * landing, hold the runway, and log the plan's own refusal or success - never re-derive
	 * either.
	 *
	 * ShutdownPauseSeconds is ARoadNetworkActor's own level-authored tunable, handed in
	 * rather than read back through Outer: FRoadAgent is world-free and cannot read it for
	 * itself, so somebody must copy it in at dispatch, and passing it explicitly says so at
	 * the call site instead of hiding it behind a back-pointer this class does not otherwise
	 * need.
	 */
	int32 DispatchArrival(const URoadNetwork& Network, const FVector2D& Near, const FAirframe& Airframe,
		double ShutdownPauseSeconds);

	/**
	 * Sends a new agent of Class along Plan. Returns its id, or 0 when the plan is unusable.
	 *
	 * Network may be null - a route with nowhere to check for a runway simply taxis, which
	 * is what every route did before departures existed. See ARoadNetworkActor::DispatchAgent
	 * for why the whole AIRFRAME is taken rather than its performance structs one at a time.
	 */
	int32 DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan, const FAirframe& Airframe,
		ETraversalClass Class, double ShutdownPauseSeconds);

	/**
	 * Sends an EXISTING agent along a new plan, keeping its id and its class.
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
	 * Re-routes a MOVING agent from SpliceStep onward, forbidding BannedEdge. Spec §4.
	 *
	 * HOW, NEVER WHEN. The deadlock resolver decides an agent is stuck and which edge to
	 * ban; a graph rebuild decides a plan no longer describes the airport. Both then call
	 * this, and it does the one thing they share: search from the node SpliceStep leaves
	 * from to the agent's own goal, under the ban AND the congestion cost, splice the answer
	 * onto the steps the agent has already driven, and hand the result to the follower.
	 * Putting the trigger in here would mean two callers with two different notions of
	 * "stuck" arguing over one function.
	 *
	 * NOT RedirectAgent. That one is a dispatch: StartTaxi resets the follower to rest at
	 * the polyline's first point, which teleports an agent mid-edge back to its origin. This
	 * keeps Travelled, Speed and Heading, and the line up to the splice is byte-identical,
	 * so the agent goes on driving the same metres it was already on.
	 *
	 * PRECONDITION: SpliceStep is AT OR AHEAD OF the step the agent is on. Travelled survives
	 * the splice, so splicing behind the agent re-maps the same route distance onto different
	 * geometry and the agent teleports sideways. Refused, not clamped - a caller asking for a
	 * node the agent has already passed has the wrong node.
	 *
	 * FALSE AND NOTHING CHANGED when the agent is unknown or not Taxiing, when SpliceStep is
	 * not a step of its plan or is behind the agent, when no route to the goal survives the
	 * ban, or when the splice fails. The agent keeps the plan it had: a caller that ignored
	 * the return would otherwise be driving something half-replanned.
	 *
	 * Vehicles and aircraft alike - nothing here reads the class beyond handing it to the
	 * query, because a van deadlocked in a service road is the same problem as an aircraft
	 * nose to nose on a taxiway.
	 */
	bool ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge);

	/**
	 * Removes an agent immediately, announcing <phase> -> Gone. For a service vehicle that
	 * has returned to its depot: it does not fly away, so nothing else would ever remove it.
	 * False for an unknown id.
	 */
	bool RetireAgent(int32 AgentId);

	/** Removes every agent. */
	void ClearAgents();

	/**
	 * One tick, in this order: Arbitrate (see it) writes every agent's StopWithin, then
	 * each agent advances under that cap, accrues StalledSeconds while it is stopped and
	 * waiting, and announces any phase change - dropping the agent once it says Gone. Then
	 * ResolveDeadlocks (see it) reads those stall clocks, and may replan one agent per
	 * wait-for cycle.
	 *
	 * ARBITRATION FIRST AND MOTION SECOND, never interleaved: a claim must be visible to
	 * every agent before any of them moves on it, or the last agent in the list drives
	 * through a node the first one took in the same frame.
	 *
	 * Network null means no arbitration - every agent drives as if alone, which is the
	 * pre-M2 behaviour and what a caller with no graph yet gets.
	 */
	void Advance(double DeltaSeconds, const URoadNetwork* Network);

	/** How many agents are currently under way or parked at their destination. */
	int32 GetAgentCount() const { return Agents.Num(); }

	/** Id of the most recently dispatched agent, or 0 when nothing is under way. */
	int32 GetNewestAgentId() const { return Agents.Num() > 0 ? Agents.Last().Id : 0; }

	const TArray<FRoadAgent>& GetAgents() const { return Agents; }
	const FRoadAgent* FindAgent(int32 AgentId) const;
	const FTrafficOccupancy& GetOccupancy() const { return Occupancy; }

	/**
	 * The table, writable, for a test that has to plant a claim no agent owns - the phantom
	 * occupant Airside.Model.Traffic.BoxEntryFirstOnly parks on a node, or a runway held by
	 * something the model has no agent for. Production code reaches the table only through
	 * the claim pass; a second writer would be the second evaluator this class exists to
	 * prevent, which is why this says ForTest in its name.
	 */
	FTrafficOccupancy& OccupancyForTest() { return Occupancy; }
	double GetSimSeconds() const { return SimSeconds; }

	/**
	 * How many DISTINCT wait-for cycles this session has logged, keyed by lowest member id.
	 *
	 * The one thing a test can ask that the logs would otherwise be the only record of.
	 *
	 * THE KEY IS WHAT MAKES THIS ONE, not the retry stamp: a cycle re-detected on the next
	 * tick, and the same ring re-formed an hour later, both key to the same lowest member id
	 * and so count once. How many LINES that cycle produced is a different question and
	 * GetDeadlockLogLinesForTest answers it - conflating the two was a comment defect on this
	 * very pair, so the two accessors now say which is which.
	 */
	int32 GetCyclesDetectedForTest() const { return CyclesSeen.Num(); }

	/**
	 * How many deadlock lines - resolved or unresolvable - this session has emitted.
	 *
	 * THE RETRY STAMP IS WHAT MAKES THIS PERIODIC. An unresolvable cycle is re-detected on
	 * every single tick; LastResolveAttempt is why it is reported once per Rules.RetrySeconds
	 * instead, and this is the count that measures it.
	 */
	int32 GetDeadlockLogLinesForTest() const { return DeadlockLogLines; }

	/** Id of the last agent whose deadlock replan SUCCEEDED, or 0 if none ever has. */
	int32 GetLastResolvedAgentForTest() const { return LastResolvedAgent; }

private:
	/**
	 * Runtime only, and deliberately not part of URoadNetwork. An agent is a thing part way
	 * through a journey, not a fact about the airport: putting them in the network would
	 * snapshot them into every undo Memento and serialise them into the saved level, so
	 * re-opening a map would restore half-driven cubes that no longer have a route.
	 */
	UPROPERTY(Transient) TArray<FRoadAgent> Agents;

	/** Next id to hand out. Ids are per-session; 0 is never issued. */
	UPROPERTY(Transient) int32 NextAgentId = 1;

	UPROPERTY(Transient) FTrafficOccupancy Occupancy;

	/** Sim seconds elapsed through Advance. The deadlock resolver's retry clock. */
	UPROPERTY(Transient) double SimSeconds = 0.0;

	/**
	 * Every cycle key (the lowest member id) this session has LOGGED. Its only reader is
	 * GetCyclesDetectedForTest.
	 *
	 * NOT a UPROPERTY, and transient by being a plain member: it is test-facing bookkeeping
	 * about log lines, not state the simulation reads - nothing in the tick branches on it,
	 * and an agent list that never reaches disk cannot leave a meaningful key behind for a
	 * later session. Reflecting it would say it mattered to the model, which it does not.
	 */
	TSet<int32> CyclesSeen;

	/** Last agent whose deadlock replan succeeded; 0 until one does. Test-facing, as above. */
	int32 LastResolvedAgent = 0;

	/** Deadlock lines emitted, resolved and unresolvable alike. Test-facing, as above. */
	int32 DeadlockLogLines = 0;

	/** Assigns the id, stores the agent, announces Gone -> its phase. The one place all three happen. */
	int32 Admit(FRoadAgent&& Agent);

	int32 FindIndex(int32 AgentId) const;

	/**
	 * Arms a departure when Plan ends on a runway. Pulled out of DispatchAgent so
	 * RedirectAgent gets the identical rule; two copies would be the drift this codebase's
	 * "one struct per thing" rule exists to prevent.
	 */
	void ArmDepartureIfRunway(FRoadAgent& Agent, const URoadNetwork* Network, const FRoutePlan& Plan) const;

	/**
	 * One claim pass over every agent, highest rank first. Spec 2026-09-06 §3.4.
	 *
	 * Agents are ordered by TraversalPriority descending, then id ascending, and each
	 * claims in turn, so a claim made by one is visible to every LATER agent in the same
	 * frame. That order - not list order - is the whole of "the aircraft goes first when
	 * both reach the node in one tick": list order would hand the node to whoever happened
	 * to be dispatched first.
	 *
	 * Then ONE re-pass over Occupancy.TakePreempted(): an agent whose reservation was taken
	 * from it - by a higher rank, or by somebody standing on it - must find that out THIS
	 * tick, or it drives a frame on a reservation it no longer holds.
	 *
	 * ONE pass and not a loop to a fixed point, and the bound is WEAKER than "nothing can
	 * change" - stated exactly here because an over-strong version of it was written first.
	 * It is NOT that rank inversions cannot happen: a node's PriorityOverride inverts rank
	 * against the visit order deliberately, which is the whole of
	 * Airside.Model.Traffic.PriorityOverride.
	 *
	 * What holds is that a re-claiming agent asks for the IDENTICAL list - same Travelled,
	 * same speed, so the same window and the same resources - so every claim it had GRANTED
	 * is an update that preempts nothing new, and every claim it had REFUSED is refused
	 * again by the same holder. The one thing that CAN differ: claims the first pass SKIPPED
	 * past its own first refusal were neither granted nor refused, so if the blocker released
	 * them later in the same pass the re-pass now reaches them, and may preempt a reservation
	 * made after it. That loser re-claims on its next tick, so it is one frame stale at
	 * worst - accepted rather than iterated to a fixed point, which would cost a pass per
	 * agent per frame to close a one-frame window.
	 */
	void Arbitrate(const URoadNetwork& Network);

	/**
	 * What one agent holds and reserves this tick, and how far it may go. Spec §3.1-§3.3.
	 *
	 * A Taxiing agent takes T = Travelled, F = footprint, G = gap, and a window
	 * W = Speed^2 / (2 Decel) + G - braking distance plus the gap, so a fast agent reserves
	 * far ahead and a stopped one still holds F/2 + G and keeps its place in a queue. It
	 * then walks its remaining steps from Head = T + W back to Tail = T - F/2 (HALF the
	 * footprint behind, because Travelled is the agent's CENTRE: a van 300 uu past a node
	 * with a 500 uu footprint has cleared it) and asks for, in route order:
	 *
	 *   - the node the current step LEFT, while the centre is still within F/2 of it;
	 *   - on each step the window touches, the edge interval [max(Tail, start),
	 *     min(Head, end)] mapped into edge distance - mirrored through the edge length on a
	 *     reversed step - occupied on the step it is standing on, reserved beyond;
	 *   - that step's END node, once the window passes it; and, by the BOX-JUNCTION ENTRY
	 *     RULE, at the moment the window reaches the START of the FIRST step shorter than
	 *     F + G that the agent has not yet entered. A box is an edge the agent cannot stand
	 *     on without still blocking the node behind it, which is every junction turn path,
	 *     so it must be granted the far end before it commits to the near one. The FIRST
	 *     such step only: a window routinely spans several boxes, and demanding the far end
	 *     of each is the rejected alternative below.
	 *
	 * First refusal in ROUTE ORDER decides everything: WaitingOn is the blocker, BlockedStep
	 * the step, and StopWithin the distance to G short of the refused thing - G short of the
	 * BOX's START when the box was refused at entry, so the agent stops outside the junction
	 * where it can still turn, rather than inside it where nobody can. All granted:
	 * StopWithin unbounded, WaitingOn 0, BlockedStep -1. Past the first refusal the agent
	 * goes on claiming the ground it OCCUPIES and reserves nothing further: dropping its own
	 * occupancies there let the agent that had merely reserved the node it stands on drive
	 * into it.
	 *
	 * TWO ALTERNATIVES REJECTED, both traced by hand on the three-vehicle triangle:
	 *
	 *  - Release everything and re-claim. ReleaseExcept keeps what is still wanted, so
	 *    first-to-reserve survives across ticks; an agent that dropped its holds and asked
	 *    again would be a stranger to its own queue every frame, and any higher-ranked
	 *    agent could step into the gap it had just opened in front of itself.
	 *  - Extending the box requirement through every CONSECUTIVE short step. It deadlocks
	 *    harder: an agent then refuses to move until a node two junctions ahead is free,
	 *    and the agent holding that node is waiting on it. Spec §3.1 records the trace;
	 *    Airside.Model.Traffic.BoxEntryFirstOnly measures it, at 400 uu of line the chained
	 *    rule kept a van out of while the box in front of it was empty.
	 *
	 * A RUNWAY SURFACE IS CLAIMED BY THE TWO TAXIING ROUTES OF SPEC §3.1, both raised in
	 * route order beside the thing that implied them, so the first-refusal rule still decides:
	 *
	 *   - a step whose EDGE derives from a runway segment claims that segment's whole CHAIN,
	 *     occupied on the step being stood on and reserved beyond, ranked as that edge is. A
	 *     refusal stops the agent a gap short of the step's START - outside the strip;
	 *   - a step whose END NODE carries HoldShortFor claims the chain that names, always
	 *     RESERVED (nobody is occupied THROUGH a bar) and only once the window has reached
	 *     the node. A refusal stops the agent with its NOSE on the bar, which is the one
	 *     refusal that does not subtract the gap.
	 *
	 * A non-Taxiing agent claims only the runway segments in RunwayHeld, occupied, and
	 * releases the rest: an arrival on the roll owns the strip and nothing on the taxiway.
	 * The third route is that one, and the handovers that fill RunwayHeld live in Advance.
	 */
	void ClaimAhead(FRoadAgent& Agent, const URoadNetwork& Network);

	/**
	 * Who goes first at Node. The node's PriorityOverride if it has one, else the class
	 * order. Spec §3.3, §5.4.
	 */
	int32 RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class) const;

	/**
	 * The wait-for graph, its cycles, and one replan per cycle per retry window. Spec §5.
	 *
	 * AT THE END OF THE TICK, after every agent has claimed and moved, so the WaitingOn edges
	 * it reads are this frame's and not a mixture of two. Every agent stalled longer than
	 * Rules.StallSeconds contributes ONE edge - id -> WaitingOn - which makes the graph a
	 * functional one (out-degree at most 1), and the walk from any member therefore either
	 * runs out of stalled waiters or closes into exactly one cycle. That is what bounds it:
	 * each step adds an agent not already on the path, and there are finitely many agents.
	 *
	 * A CYCLE IS KEYED BY ITS LOWEST MEMBER ID, so it is handled once however many members
	 * would have found it - that key, and nothing else, is why one jam is one entry in
	 * CyclesSeen. WHAT THE RETRY STAMP DOES IS SEPARATE: re-detecting a cycle inside
	 * Rules.RetrySeconds does nothing at all, so an unresolvable jam is REPORTED on a cadence
	 * rather than on every tick, and the player's later fix (a new edge out of it) is picked
	 * up on the next window.
	 *
	 * NO REVERSING - spec §1. The one move available is a member turning at the node it is
	 * stopped at, so a cycle whose members are all mid-edge is logged and left, which is what
	 * Airside.Model.Traffic.HeadOnStops measures.
	 */
	void ResolveDeadlocks(const URoadNetwork& Network);

	/**
	 * Can this member of a cycle turn where it stands? Spec §5's refined resolver rule.
	 *
	 * True only for a Taxiing agent that is STOPPED, was refused something (BlockedStep), and
	 * is AT the node that step leaves from - within Gap + Footprint/2 short of it and not
	 * past it. The alternative to a banned edge is another edge OUT of that node, so an agent
	 * that has already entered the edge cannot take it without reversing, and one still a
	 * whole edge short of the node would be replanned from a node it is nowhere near.
	 */
	bool CanReplanAtBlockedStep(const FRoadAgent& Agent) const;

	/** Route distance at which Step begins - the previous step's end, or 0. */
	static double StepStart(const FRoutePlan& Plan, int32 Step);

	/** The node Step leaves from: the previous step's To, or the plan's Start. */
	static FGuidelineNodeId StepFromNode(const FRoutePlan& Plan, int32 Step);

	/** Which step Travelled is on. The map from a distance to an edge, read off
	 *  FRouteStep::EndDistance so it cannot disagree with the polyline the follower walks. */
	static int32 CurrentStep(const FRoutePlan& Plan, double Travelled);
};
