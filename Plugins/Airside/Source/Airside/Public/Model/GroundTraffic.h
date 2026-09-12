#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/NodeReach.h"
#include "Model/RoadAgent.h"
#include "Model/RoadTraffic.h"
#include "Model/TrafficOccupancy.h"
#include "GroundTraffic.generated.h"

class URoadNetwork;
enum class EDepartureRefusal : uint8;

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
 * What one UGroundTraffic::OnGraphRebuilt did. Read by the test that pins spec §6.1's
 * "re-pointed, not replaced": every case of Airside.Model.Traffic.GraphRebuild would also
 * pass on an implementation that simply re-searched Start to Goal on every rebuild, and the
 * difference between the two is a REPLAN COUNT of zero - which is a number, not a shape, and
 * so needs an accessor rather than an assertion about geometry.
 *
 * NOT reflected and not a UPROPERTY: it is bookkeeping about the last call, nothing in the
 * simulation branches on it, and an agent list that never reaches disk cannot leave a
 * meaningful summary behind for a later session.
 */
struct FGraphRebuildSummary
{
	/** Agents whose plan was re-resolved, stranded ones EXCLUDED - see the log line. */
	int32 ReResolved = 0;
	int32 Replanned = 0;
	int32 Truncated = 0;
	int32 Stranded = 0;
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
 *
 * ONE CLASS, FOUR TRANSLATION UNITS - the shape URoadEditFacade already uses
 * (RoadEditFacade.cpp beside RoadEditFacadeSurfaces.cpp). Split by CONCERN rather than by
 * size, so a reader chasing a traffic report opens the file named after the rule:
 *
 *   - GroundTraffic.cpp          dispatch, admit, redirect/retire, Advance, Arbitrate, and
 *                                the plan/step helpers the other three read;
 *   - GroundTrafficClaims.cpp    ClaimAhead and the claim geometry it is built from (§3);
 *   - GroundTrafficDeadlock.cpp  ResolveDeadlocks, CanReplanAtBlockedStep, ReplanAt (§4, §5);
 *   - GroundTrafficRebuild.cpp   OnGraphRebuilt, ReResolvePlan, SpliceReplan (§6).
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

	/**
	 * The figures this tick arbitrates in. NOT the knob a designer turns.
	 *
	 * TRANSIENT AND NO LONGER EditAnywhere. This object is re-created every session and
	 * re-pointed on a PIE duplication, so a value typed into its Details panel was never
	 * saved and never survived the copy - it looked like a knob and was not one. The knob is
	 * ARoadNetworkActor::TrafficRules, which the .umap saves; UAirsideTraffic::Advance
	 * copies it in here every tick, so this field is always the level's answer and a
	 * world-free test can still set it directly.
	 */
	UPROPERTY(Transient) FTrafficRules Rules;

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
	 * Sends a PARKED agent to whichever runway gives the shortest admitted taxi, with the
	 * take-off armed - the inspector's Depart button. Anything not Parked is refused as
	 * NotParked: a taxiing aircraft has a plan, an arriving one is not on the ground, a
	 * departing one is already going. Composed from PlanAny and RedirectAgent so there is
	 * one arming path (ArmDepartureIfRunway) and one engine restart (StartTaxi).
	 */
	EDepartureRefusal DepartAgent(int32 AgentId, const URoadNetwork& Network);

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
	 *
	 * BannedNode, when set, closes every arm into that node; BannedEdge closes one edge. The
	 * resolver passes the node when a node refused the agent and the edge otherwise. Every
	 * replan also avoids runway-derived edges (FRouteQuery::AvoidRunways, Held) - a replan that
	 * taxied along the strip re-reserved it and starved the bar-holder it was trying to get
	 * round. ALSO FALSE when the route found is the route the agent already has: a "replan"
	 * that changes nothing must not count as a resolution, or the resolver logs a cycle as
	 * resolved every window while nobody moves.
	 */
	bool ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge,
		FGuidelineNodeId BannedNode = FGuidelineNodeId());

	/**
	 * Re-points every agent's route at the graph that has just been rebuilt. Spec §6.
	 *
	 * BY POSITION, NEVER BY HANDLE, and that is forced rather than chosen:
	 * FRoadGuidelineBuilder::Build frees every derived node and edge slot and allocates
	 * fresh ones, so an agent's steps name handles that are dead the instant the builder
	 * returns. The guideline graph shares its ends by HANDLE and deliberately has no
	 * bitwise weld contract (see FGuidelineEdge), so the only identity that can cross a
	 * rebuild is "the live node that now holds this step's end position". The polyline is
	 * what survives it: it is the line the agent is physically driving, so re-resolving
	 * against it cannot move the agent by so much as a unit.
	 *
	 * THREE OUTCOMES PER AGENT, in order of how much the player would notice: every step
	 * resolves and nothing changes but the handles; a step AHEAD of the agent has lost its
	 * edge but a route to the same goal survives, and that is spliced on from the last step
	 * that resolved; or nothing replaces it and the route is TRUNCATED there, so the agent
	 * drives to a stop on live pavement instead of off the end of the airport.
	 *
	 * AND THE FOURTH, WHICH IS NOT A DEGREE OF THE OTHER THREE: the step the agent is
	 * DRIVING ON is the one that went. Spec §6.2 - "only an agent whose current step itself
	 * is gone is stranded in place" - and it is stranded rather than replanned because a
	 * splice at the current step re-reads the agent's own Travelled on new geometry and
	 * teleports it (3310 uu, measured; unbounded in principle), while a truncation there
	 * would end the route behind it. So is an agent whose own ground no longer has a node
	 * within Rules.ResolveRadius. A stranded agent keeps the runway surface it is standing
	 * on - see ReResolvePlan's Strand - because a rebuild moves no aeroplanes.
	 *
	 * THEN THE GUIDELINE CLAIMS GO AND THE SURFACE CLAIMS STAY - FTrafficOccupancy::
	 * ReleaseGuidelineClaims, not Clear(). The reason is NOT that a stale handle could name
	 * new pavement: handles are generation-checked, so a claim on a freed slot simply matches
	 * nothing. It is that the RESOURCES those claims name have stopped existing - every
	 * derived node and edge was freed - and no agent will ever re-claim them, because the
	 * routes that named them have just been re-pointed at other handles. They would sit in
	 * the table for the rest of the session.
	 *
	 * A SURFACE IS NOT ONE OF THEM. FRoadSegmentId is the road model, which a guideline
	 * rebuild does not regenerate, so the strip under an aeroplane is as real afterwards as
	 * before - and ArrivalPlanner::Plan reads this table directly at DispatchArrival, between
	 * ticks. Dropping those claims would show a crossing, a roll-out or a line-up as a free
	 * runway for as long as it took the player to click: the window Task 7 closed.
	 *
	 * Everyone re-claims their guidelines on the next tick, which is safe because Advance
	 * arbitrates BEFORE it moves anything - there is no frame in which an agent drives on a
	 * table it has not claimed in.
	 *
	 * AIRCRAFT TOO, unlike dispatch-time routing (spec §4): a route fixed at clearance is
	 * still a route over pavement, and pavement the player has just deleted is not something
	 * an aircraft can be held to.
	 */
	void OnGraphRebuilt(const URoadNetwork& Network);

	/**
	 * Removes an agent immediately, announcing <phase> -> Gone. For a service vehicle that
	 * has returned to its depot: it does not fly away, so nothing else would ever remove it.
	 * False for an unknown id.
	 */
	bool RetireAgent(int32 AgentId);

	/** Removes every agent. */
	void ClearAgents();

	/**
	 * Hold a stand for something that is not an agent yet - an accepted flight, hours before
	 * it is dispatched.
	 *
	 * THE SAME TABLE AND THE SAME CLAIM the planner already honours: ArrivalPlanner::
	 * ChooseStand skips a stand whose PoseNode is held, and FTrafficOccupancy::IsHeld counts
	 * a reservation (bOccupied false) as held. A separate reservation table would be a second
	 * source of truth, and the two would drift the first time a stand was freed in one of
	 * them - the same argument FTrafficResource's header makes for one table over three.
	 *
	 * HOLDERID IS NOT AN AGENT ID. Agent ids are allocated NextAgentId++ from 1, so callers
	 * pass a NEGATIVE id (AirportOps passes the negative of the flight id) and the two id
	 * spaces cannot collide without a registry that would have to be kept in step.
	 *
	 * False if someone else already holds it, in which case nothing was changed.
	 */
	bool HoldStand(int32 HolderId, FGuidelineNodeId PoseNode);

	/**
	 * Give back every hold made by HolderId.
	 *
	 * ReleaseReservations and not ReleaseAll: bOccupied is the whole test there, and a hold
	 * is never occupied, so for a holder the two agree - but ReleaseAll would also take a
	 * BODY if a caller ever passed a real agent id by mistake, and an aeroplane silently
	 * losing the stand it is standing on is not a bug that announces itself.
	 */
	void ReleaseHold(int32 HolderId);

	/** Whether any holder but ExcludingHolder holds this stand - a body or a reservation. */
	bool IsStandHeld(FGuidelineNodeId PoseNode, int32 ExcludingHolder) const;

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

	/** True after a stand claim was released or a rebuild ran, until Advance's re-offer pass
	 *  consumes it. For Airside.Model.Traffic.StandClaim. */
	bool StandsMayHaveFreedForTest() const { return bStandsMayHaveFreed; }

	/**
	 * The table, writable, for a test that has to plant a claim no agent owns - the phantom
	 * occupant Airside.Model.Traffic.BoxEntryFirstOnly parks on a node, or a runway held by
	 * something the model has no agent for. Production code reaches the table only through
	 * the claim pass; a second writer would be the second evaluator this class exists to
	 * prevent, which is why this says ForTest in its name.
	 */
	FTrafficOccupancy& OccupancyForTest() { return Occupancy; }

	/**
	 * Makes AgentId's plan unusable where it stands, keeping the agent and its position.
	 * True if the id was known.
	 *
	 * IT STANDS IN FOR THE PAVEMENT GOING AWAY UNDER A MOVING AGENT - a rebuild that leaves
	 * a step with no live edge and no route to replace it, or a redirect that lands a bad
	 * plan on a live follower. Both end in ClaimAhead's dead-plan branch, which must release
	 * every claim the agent holds; without a hook there is no world-free way to reach that
	 * branch, and a seam no test reaches is one a later edit can quietly unwire (see
	 * Airside.Model.Traffic.DeadPlanReleases).
	 *
	 * ForTest in its name for the same reason OccupancyForTest is: nothing in production
	 * invalidates a plan by hand - OnGraphRebuilt truncates or strands through
	 * ReResolvePlan, which is a decision, not an assignment.
	 */
	bool StrandForTest(int32 AgentId);

	/**
	 * Puts a Taxiing agent ON a runway the way the Vacated handover does - CrossingRunway
	 * set, phase OnStrip - so a world-free test can stage an aircraft that has just landed
	 * without a stand definition, which DispatchArrival needs and a bare automation run
	 * has not got. Stands in for FRoadAgent::Advance's Arriving -> Taxiing frame and
	 * nothing else; the geometric release then runs as in play. False for an unknown or
	 * non-Taxiing agent.
	 */
	bool BeginCrossingForTest(int32 AgentId, FRoadSegmentId RunwaySeed);

	/**
	 * ReplanAt with no banned node, from outside the resolver. What the deadlock resolver
	 * and OnGraphRebuilt call; exposed so a test can pin what a replan's SEARCH is allowed
	 * to use - a free runway end, a held one - without staging the two-aircraft cycle that
	 * would otherwise be the only way to make the resolver replan on demand. ForTest for the
	 * same reason as the others above: in production a replan is a decision, never a call.
	 */
	bool ReplanAtForTest(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge)
	{
		return ReplanAt(AgentId, Network, SpliceStep, BannedEdge, FGuidelineNodeId());
	}
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

	/** How many reservation cycles were settled by a yield rather than a replan. See §5. */
	int32 GetYieldsForTest() const { return Yields; }

	/** Id of the last agent that yielded its reservations, or 0 if none has. */
	int32 GetLastYieldedAgentForTest() const { return LastYieldedAgent; }

	/** What the last OnGraphRebuilt did. See FGraphRebuildSummary for why a test needs it. */
	FGraphRebuildSummary GetLastRebuildSummaryForTest() const { return LastRebuild; }

	/**
	 * The two maps between ROUTE distance - what the follower walks and what a stop point is
	 * expressed in - and EDGE distance, which is what a claim's interval means. Pure
	 * arithmetic on doubles: no member of this class, no network, no agent.
	 *
	 * PUBLIC AND NESTED, rather than private statics behind a ForTest forwarder. Both mirror
	 * a reversed step, both are one line of arithmetic that is wrong in a way no fixture
	 * reads back directly (a From > To interval conflicts with nothing at all, silently), so
	 * they are worth pinning on their own and a test has to reach them. A ForTest wrapper on
	 * UGroundTraffic would instead be a second door into the claim pass - the thing
	 * OccupancyForTest's comment warns against - and two free functions in Model/ would stop
	 * saying which pass they belong to. Nesting costs neither.
	 *
	 * Airside.Model.Traffic.ClaimGeometry is the test.
	 */
	struct FClaimGeometry
	{
		/** A claim's extent along one edge, in EDGE distance from that edge's A end. */
		struct FEdgeInterval
		{
			double From = 0.0;
			double To = 0.0;
		};

		/**
		 * The route interval [Lo, Hi] on a step that begins at StepBegin and whose edge is
		 * Length long, mapped into edge distance.
		 *
		 * Route distance to EDGE distance, measured from the edge's A end. A reversed
		 * step is walked from B, so its interval mirrors through the edge length - and
		 * the mirror swaps the ends, which is why From takes what Hi produced. Getting
		 * this backwards would give From > To, and a half-open interval that way round
		 * conflicts with nothing at all.
		 */
		static FEdgeInterval EdgeInterval(double Lo, double Hi, double StepBegin, double Length, bool bReversed)
		{
			FEdgeInterval Interval;
			Interval.From = Lo - StepBegin;
			Interval.To = Hi - StepBegin;
			if (bReversed)
			{
				Interval.From = Length - (Hi - StepBegin);
				Interval.To = Length - (Lo - StepBegin);
			}
			return Interval;
		}

		/**
		 * Where a blocker's edge interval first bars the way, back in ROUTE distance, on a
		 * step that begins at StepBegin and whose edge is Length long.
		 *
		 * The blocker's NEAREST boundary ahead, back in route distance. On a reversed
		 * step the agent is walking the edge from B, so the near end of the blocker's
		 * interval is its To, mirrored.
		 */
		static double BoundaryAhead(double StepBegin, double Length, bool bReversed,
			double BlockerFrom, double BlockerTo)
		{
			return bReversed ? StepBegin + (Length - BlockerTo) : StepBegin + BlockerFrom;
		}
	};

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

	/**
	 * How far each node's claim reaches along each of its edges - see NodeReach.h for the
	 * bug this exists for. MUTABLE because the claim pass is const over the agent it is
	 * building for and this is memoisation of the graph, not state of the simulation:
	 * dropping it changes nothing an agent does, only how much geometry the next tick
	 * re-samples.
	 */
	mutable FNodeReachCache NodeReach;

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

	/**
	 * When each cycle key last settled by a YIELD (SimSeconds). Read by the tick, unlike the
	 * sets above: a cycle that re-forms within Rules.RetrySeconds of yielding is one a yield
	 * did not fix - the other member wanted something a third party holds - and goes to the
	 * replan path instead of yielding for ever. Plain member for the same reason as CyclesSeen:
	 * agents never reach disk, so a key could not mean anything to a later session.
	 */
	TMap<int32, double> YieldedAt;

	/** Reservation cycles settled by a yield; and who yielded last. Test-facing. */
	int32 Yields = 0;
	int32 LastYieldedAgent = 0;

	/** Deadlock lines emitted, resolved and unresolvable alike. Test-facing, as above. */
	int32 DeadlockLogLines = 0;

	/** What the last OnGraphRebuilt did. Test-facing, as above. */
	FGraphRebuildSummary LastRebuild;

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
	 *   - a step whose END NODE carries HoldingPositionFor claims the chain that names, always
	 *     RESERVED (nobody is occupied THROUGH a bar) and only once the window has reached
	 *     the node. A refusal stops the agent with its NOSE on the bar, which is the one
	 *     refusal that does not subtract the gap.
	 *
	 * A non-Taxiing agent claims only SURFACES, occupied, and releases the rest: an arrival
	 * on the roll owns the strip and nothing on the taxiway. Which surfaces is RunwayHeld -
	 * the handovers that fill it live in Advance - PLUS the chain of any crossing still in
	 * progress, because spec §3.4's "their surface" has to mean the one the body is on: an
	 * aircraft whose plan dies mid-crossing is Parked by the end of that tick with RunwayHeld
	 * empty, and holding nothing would show the strip free with an aeroplane on it. The third
	 * route is that one.
	 */
	void ClaimAhead(FRoadAgent& Agent, const URoadNetwork& Network);

	/**
	 * The numbers ONE claim pass works in: the agent's centre, its body and gap, and the
	 * stretch of route the window covers. Read by four of the five steps below.
	 *
	 * ONE STRUCT rather than six parameters threaded through four helpers - the codebase's
	 * "one struct per thing", and for its stated reason: a figure copied by hand into a
	 * sibling call is a figure somebody will one day forget to copy, and the copy that
	 * nobody set is how an arrival taxied on default figures.
	 *
	 * The names are the one-letter ones the rules are written in, because the rules are
	 * arithmetic and T + F/2 reads as the nose while Travelled + Footprint/2 does not.
	 */
	struct FClaimWindow
	{
		/** Follower.Travelled: the agent's CENTRE, never its nose. */
		double T = 0.0;
		/** Footprint and gap for this agent's class. See FTrafficRules. */
		double F = 0.0;
		double G = 0.0;
		/** T + F/2 + braking distance + G, and T - F/2. WindowFor says why the halves. */
		double Head = 0.0;
		double Tail = 0.0;
		/** Index into Plan.Steps that T falls on. */
		int32 Current = INDEX_NONE;
	};

	/**
	 * Where the agent's nose, centre and tail are this tick, sampled ONCE per pass out of
	 * the one array the follower walks. See SampleBody, and UpdateCrossing, which is the
	 * only reader: all three rules of a crossing ask about the same three points, and
	 * sampling per rule is the second evaluator this codebase's guideline invariant forbids.
	 */
	struct FClaimBody
	{
		/** All three or none - PointAtDistance fails on the polyline, not on the distance. */
		bool bValid = false;
		FVector2D Nose = FVector2D::ZeroVector;
		FVector2D Centre = FVector2D::ZeroVector;
		FVector2D Tail = FVector2D::ZeroVector;
	};

	/** One thing an agent wants this tick. Defined in GroundTrafficClaims.cpp, where the
	 *  only three functions that build or read one live. */
	struct FWantedClaim;

	/** A non-Taxiing agent's whole claim pass: hold RunwayHeld AND the chain its body is
	 *  crossing, release everything else. Spec §3.4's "their surface and nothing else",
	 *  where the surface includes the one it is standing on. ClaimAhead's first branch. */
	void HoldRunwayOnly(FRoadAgent& Agent, const URoadNetwork& Network);

	/**
	 * The one claim that is not about the ground under or ahead of the agent: its DESTINATION.
	 * An Arriving or Taxiing aircraft RESERVES the stand pose node it is heading for, and a
	 * Parked agent OCCUPIES the node it parked at - stand or not; the M2 rule "a parked agent
	 * holds only its surface" left a parked aircraft on a taxiway junction holding nothing
	 * (spec 2026-09-07-stand-occupancy §3, amended).
	 *
	 * DERIVED FROM GoalNode EVERY TICK, after the phase's own claim pass has run its
	 * ReleaseExcept. Threading the stand through BuildPending/ApplyClaims was rejected: those
	 * are route-ordered claims whose first refusal sets StopWithin, and a stand must never
	 * stop an aircraft short - it is a reservation for a place, not a queue for a line. The
	 * drop-and-reclaim inside one agent's pass is invisible: Arbitrate is synchronous and no
	 * other agent has the same goal (the planner and the rebuild see to that).
	 */
	void ClaimGoalNode(FRoadAgent& Agent, const URoadNetwork& Network);

	/** Claims Agent's GoalNode as a stand reservation right now, for the between-ticks
	 *  window DispatchArrival reads the table in. Used by both dispatches and the redirect. */
	void ClaimGoalNodeAtDispatch(const FRoadAgent& Agent, int32 Id, const URoadNetwork& Network);

	/**
	 * Offers every waiting aircraft (bAwaitingStand) the best free stand reachable from where
	 * it stopped, through RedirectAgent. Runs at the end of Advance when bStandsMayHaveFreed;
	 * one pass, then the flag clears whether or not anyone was placed.
	 */
	void ReofferStands(const URoadNetwork& Network);

	/**
	 * Set when a stand claim is released (redirect, retire, Gone) or the graph is rebuilt;
	 * consumed by Advance's re-offer pass. A FLAG rather than an event: the table is rebuilt
	 * per tick and the model is world-free, so one bool checked per frame is the cheapest
	 * correct thing.
	 */
	bool bStandsMayHaveFreed = false;

	/** A Taxiing agent whose plan went bad under it: give back every GUIDELINE, keep any
	 *  runway surface and the crossing that describes it (a plan says nothing about where a
	 *  body is), clear the arbitration fields. ClaimAhead's second branch. */
	void ReleaseForDeadPlan(FRoadAgent& Agent);

	/** T, F, G, Head, Tail and the current step for one pass. See FClaimWindow. */
	FClaimWindow WindowFor(const FRoadAgent& Agent) const;

	/** Nose, centre and tail out of Plan.Polyline, once. See FClaimBody. */
	static FClaimBody SampleBody(const FRoutePlan& Plan, const FClaimWindow& Window);

	/** Step 0's phase machine: arm at a bar that leads onto the strip, note the centre
	 *  reaching it, release when the whole body is off it. Spec §3.1's fourth route. */
	void UpdateCrossing(FRoadAgent& Agent, const URoadNetwork& Network,
		const FClaimWindow& Window, const FClaimBody& Body) const;

	/** Steps 0-2: everything the agent wants this tick, IN ROUTE ORDER, which is what the
	 *  first-refusal rule reads. Nothing is asked of the table here. */
	void BuildPending(const FRoadAgent& Agent, const URoadNetwork& Network,
		const FClaimWindow& Window, TArray<FWantedClaim>& Pending) const;

	/** Step 3: ask the table for each in turn, keep what was granted or occupied, and let
	 *  the FIRST refusal write StopWithin, WaitingOn and BlockedStep. */
	void ApplyClaims(FRoadAgent& Agent, const FClaimWindow& Window, const TArray<FWantedClaim>& Pending);

	/** How far a refused claim lets the agent go, in route distance from T. A pure function
	 *  of the refusal's kind, the step it was raised on, and the window. */
	static double StopWithinFor(const FWantedClaim& Want, const FTrafficClaim& Blocker,
		const FClaimWindow& Window);

	/**
	 * Who goes first at Node. The node's PriorityOverride if it has one, else the class
	 * order. Spec §3.3, §5.4.
	 */
	int32 RankAt(const URoadNetwork& Network, FGuidelineNodeId Node, ETraversalClass Class) const;

	/**
	 * How much FURTHER than half a footprint Node's claim reaches along Edge for this class,
	 * in uu; 0 at an ordinary junction. The claim pass adds it wherever it used to compare a
	 * distance to the node against F/2, and the stop point for a refused node moves back by
	 * it, so a body told to wait for a node waits where the lines have actually parted.
	 */
	double ReachExcessAt(const URoadNetwork& Network, FGuidelineNodeId Node, FGuidelineEdgeId Edge,
		ETraversalClass Class) const;

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
	bool CanReplanAtBlockedStep(const FRoadAgent& Agent, const URoadNetwork* Network) const;

	/** What re-resolution did to one plan. Counted by OnGraphRebuilt for its one log line. */
	enum class EReResolve : uint8
	{
		/** Every remaining step found its node and its edge again; only handles changed. */
		Intact,
		/** A step's edge was gone; a fresh route from there to the goal was spliced on. */
		Replanned,
		/** Gone with nothing to replace it: the route now ends at the last live node. */
		Truncated,
		/** Not even the ground under the agent resolved, or truncation left no route at all. */
		Stranded,
	};

	/**
	 * Re-points Plan's steps from FromStep onward at the rebuilt graph. See OnGraphRebuilt.
	 *
	 * FromStep IS THE FIRST STEP THE AGENT HAS NOT FINISHED, and the steps behind it keep
	 * their dead handles - with ONE exception that is not optional. The node the current step
	 * LEAVES FROM is Steps[FromStep-1].To (or Plan.Start at step 0), it is what StepFromNode
	 * answers, and FOUR live readers ask for it every tick:
	 *
	 *   - ClaimAhead's crossing arm, which asks whether that node carries a HoldingPositionFor bar.
	 *     A dead handle reads as no bar, so no crossing would ever arm again after a rebuild;
	 *   - ClaimAhead's tail-node claim, OfNode(From), held while the body is still within
	 *     Footprint/2 of it. On a dead handle that claim protects nothing and the junction
	 *     BEHIND the agent is open for somebody to drive into;
	 *   - RankAt, which falls back to the class order when the node cannot be found - so a
	 *     node's PriorityOverride would silently stop applying;
	 *   - ReplanAt's Query.Start when the failed step IS the current one, where a dead handle
	 *     gives ERouteResult::NoStart - so that replan could never succeed, and the truncation
	 *     that followed would write the same dead handle into GoalNode.
	 *
	 * So this function re-points that one node as soon as it has resolved it. Steps further
	 * back are genuinely never read again - the follower walks the polyline - and are left
	 * alone, because re-resolving them could only change numbers the agent has already passed.
	 *
	 * Applied to Follower.Plan from CurrentStep for a Taxiing agent, and to TaxiInPlan from
	 * 0 for an Arriving one - the route it will fly when it vacates, which no follower is on
	 * yet and which is just as dead after a rebuild as one being driven.
	 *
	 * WHICH OF THE TWO DECIDES WHAT A FAILURE AT FromStep MEANS. Under a moving follower it
	 * is the ground the agent is on, so it strands in place (spec §6.2, and see the branch
	 * itself for why a replan there teleports). A taxi-in plan is a route nobody has entered
	 * - the aircraft is on the runway - so its first step failing is an ordinary replan, and
	 * only a route that cannot be rebuilt at all strands it.
	 *
	 * MUTATES Agent.GoalNode when the goal position still resolves, because every replan
	 * from here on searches to it: a goal handle left naming a freed slot would fail every
	 * subsequent deadlock replan for the rest of the session, silently.
	 */
	EReResolve ReResolvePlan(FRoadAgent& Agent, FRoutePlan& Plan, int32 FromStep, const URoadNetwork& Network);

	/**
	 * Runs Query and splices its answer onto Plan's first KeepSteps steps, IN PLACE.
	 *
	 * True and Plan is the new journey; FALSE AND PLAN IS UNTOUCHED - the search or the
	 * splice failed, and a caller that ignored the return would otherwise be driving
	 * something half-replanned. That all-or-nothing shape is the whole reason this is one
	 * function and not two calls at each site.
	 *
	 * NO AGENT AND NO FOLLOWER. ReplanAt needs the follower handled (Travelled survives, the
	 * reservations drop, the stall clock resets); a TaxiInPlan has no follower on it at all
	 * and must not touch one. What the two share is exactly this - search, splice, keep or
	 * discard - so this is where it lives, and each caller adds its own aftermath.
	 */
	static bool SpliceReplan(const URoadNetwork& Network, const FRouteQuery& Query, int32 KeepSteps, FRoutePlan& Plan);

	/** Route distance at which Step begins - the previous step's end, or 0. */
	static double StepStart(const FRoutePlan& Plan, int32 Step);

	/** The node Step leaves from: the previous step's To, or the plan's Start. */
	static FGuidelineNodeId StepFromNode(const FRoutePlan& Plan, int32 Step);

	/** Which step Travelled is on. The map from a distance to an edge, read off
	 *  FRouteStep::EndDistance so it cannot disagree with the polyline the follower walks. */
	static int32 CurrentStep(const FRoutePlan& Plan, double Travelled);
};
