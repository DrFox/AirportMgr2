#pragma once

#include "CoreMinimal.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeadlockResolver.h"
#include "Model/NodeReach.h"
#include "Model/PlanReResolver.h"
#include "Model/RoadAgent.h"
#include "Model/RoadTraffic.h"
#include "Model/RunwayQuery.h"
#include "Model/TrafficOccupancy.h"
#include "Model/TrafficRules.h"
#include "GroundTraffic.generated.h"

class URoadNetwork;
enum class EDepartureRefusal : uint8;
struct FDepartureAsk;

// FTrafficRules, FPlanReResolver AND FDeadlockResolver MOVED OFF THIS HEADER (issue #175) to
// Model/TrafficRules.h, Model/PlanReResolver.h and Model/DeadlockResolver.h - each included
// above, so every one of the 47 existing includers of THIS header keeps compiling unchanged.
// TrafficClaims.h and RoadNetworkActor.h are the two switched to including only what they
// actually name (TrafficRules.h, or TrafficRules.h plus what FClaimPass needs) instead of
// this whole header - see those headers' own comments, and Model/TrafficContext.h for the
// parameter object issue #175 also introduced.

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
 * SPLIT BY RESPONSIBILITY, NOT JUST BY FILE (issue #84). Four translation units, the shape
 * URoadEditFacade already uses (RoadEditFacade.cpp beside RoadEditFacadeSurfaces.cpp) - but
 * three of the four now define a STRUCT UGroundTraffic merely owns one of, rather than
 * methods and fields living directly on this class:
 *
 *   - GroundTraffic.cpp    dispatch, admit, redirect/retire, registry, Advance, Arbitrate,
 *                          events, and the plan/step helpers the other three read;
 *   - TrafficClaims.cpp    FClaimPass - one agent's claim pass (§3). Model/TrafficClaims.h;
 *   - GroundTrafficDeadlock.cpp  FDeadlockResolver - the wait-for graph (§4, §5);
 *   - GroundTrafficRebuild.cpp   OnGraphRebuilt (kept here: it is tick-order orchestration,
 *                                not mechanism) plus FPlanReResolver - ReplanAt,
 *                                ReResolvePlan, SpliceReplan (§4, §6), which both
 *                                OnGraphRebuilt and FDeadlockResolver call into.
 *
 * UGroundTraffic keeps the registry, dispatch, tick order and events; it owns one FClaimPass
 * (constructed fresh per Arbitrate call - it carries no state of its own), one
 * FDeadlockResolver and one FPlanReResolver. A test can construct any of the three directly
 * and drive it with no UGroundTraffic at all.
 */
UCLASS()
class AIRSIDE_API UGroundTraffic : public UObject
{
	GENERATED_BODY()

	/** Strand, BeginCrossing and ReplanAt (#104) - see FGroundTrafficTestAccess, declared
	 *  after this class, for why they are not public members of it. */
	friend struct FGroundTrafficTestAccess;

public:
	/** See UAirsideTraffic::OnAgentPhaseChanged, which relays this one layer up. */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnAgentPhaseChanged, int32 /*AgentId*/, EAgentPhase /*From*/, EAgentPhase /*To*/);
	FOnAgentPhaseChanged OnAgentPhaseChanged;

	/** Fired when DispatchArrival refuses, with the planner's reason. The log line stays too. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnArrivalRefused, EArrivalRefusal);
	FOnArrivalRefused OnArrivalRefused;

	/**
	 * A runway's strip was held and is not now - fired once per strip, with the seed its summary names, at the
	 * end of the Advance (or OnGraphRebuilt) that saw it go. See DiffFreedom. Relayed by UAirsideTraffic.
	 *
	 * DERIVED, NOT RAISED AT A SITE (ops batch 3 §5): "held" is ArrivalPlanner::IsChainHeld, exactly what the
	 * arrival queue's IsRunwayBusy asks, so "freed" means "what the queue asks just turned false" - whatever
	 * released it. A taxiing crossing clears through the per-tick claim pass with no event and no
	 * OccupancyRevision bump of its own, and a per-site event would have missed it; a diff cannot.
	 * ENFORCED BY: Airside.Model.Traffic.RunwayFreed.CrossingClears
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnRunwayFreed, FRoadSegmentId /*Seed*/);
	FOnRunwayFreed OnRunwayFreed;

	/**
	 * Stand pose nodes that were held - a reservation (HoldStand) or an agent's goal or body - and are not now,
	 * one broadcast per Advance (or rebuild) that freed any. Same diff as OnRunwayFreed. The bStandsMayHaveFreed
	 * sites are NOT this signal: they gate ReofferStands inside this class, and stay as they are.
	 * ENFORCED BY: Airside.Model.Traffic.RunwayFreed.StandsDiff
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnStandsFreed, const TArray<FGuidelineNodeId>& /*PoseNodes*/);
	FOnStandsFreed OnStandsFreed;

	/**
	 * A parked aircraft DepartAgent refused PushbackBlocked would not be refused that now - fired once, and the
	 * aircraft dropped from the watch, at the end of the DiffFreedom that saw it. Relayed by UAirsideTraffic.
	 *
	 * DERIVED WITH DepartAgent's OWN DECISION (AskDeparture), not raised where ground is released: the push
	 * ground frees through the per-tick claim pass (a taxiing aircraft moving on), which has no event and moves
	 * no OccupancyRevision - PR D's finding about the one refusal the job board used to poll every frame for.
	 * "Would not be refused that" includes a replan that now goes straight out, or is refused for another
	 * reason: the caller asks again and hears the new answer, which costs one ask; missing it strands the
	 * aircraft. An aircraft retired or no longer Parked leaves the watch silently.
	 * ENFORCED BY: Airside.Model.Traffic.PushGroundFreed.TaxiingBlockerClears
	 */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPushGroundFreed, int32 /*AgentId*/);
	FOnPushGroundFreed OnPushGroundFreed;

	/** How many parked aircraft are on the push watch (see OnPushGroundFreed). */
	int32 PushWatchCountForTest() const { return PushWatch.Num(); }

	// NO RunwayFreedCount (removed in ops batch 3 PR E's review, 2026-09-30): PR D added it for PR E's two pollers,
	// and neither reads a held runway - a held taxi out's replan is only RANKED by one (see ReplanHeldTaxiOuts'
	// gate), and LandChoices::Build is given no traffic model at all. A counter nothing consumes is a list declared
	// and never read (CLAUDE.md); OnRunwayFreed itself is what AirportOps and the tests count.

	/**
	 * How many DiffFreedom calls have found the set of held stands CHANGED - one gained or lost, either way. The
	 * stand card's key (ops batch 3 PR E): OccupancyRevision covers goal claims, holds and phase changes, but a
	 * body the per-tick claim pass puts on a pose node, or takes off it, moves no revision; OnStandsFreed sees
	 * only the losing half. A plain session counter, like OccupancyRevision.
	 * ENFORCED BY: Airside.Model.Traffic.StandHolds.ChurnIsCounted
	 */
	uint32 StandHoldChangeCount() const { return StandHoldChanges; }

	/** How many times ReplanHeldTaxiOuts has actually tried to plan a held taxi out - its gate's counter. */
	int32 TaxiOutReplanAttemptsForTest() const { return TaxiOutReplanAttempts; }

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
	 * The same, for a service vehicle: the agent is started with StartDrive and carries an
	 * FVehicle. See FRoadAgent::Chassis for why the two bundles are held side by side.
	 */
	int32 DispatchAgent(const URoadNetwork* Network, const FRoutePlan& Plan, const FVehicle& Vehicle,
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
	 * Appends Tail to a MOVING agent's route, IN PLACE: RouteSearch::Splice of the live plan's
	 * every step with Tail, handed to the follower with Replace. Travelled, Speed, Heading and a
	 * tow's chain all carry on - the agent never learns its old goal was a goal.
	 *
	 * NOT RedirectAgent, which is a dispatch: it restarts the follower from REST at the new
	 * plan's first point, so a vehicle sent on from where it stopped stops first. Nor ReplanAt,
	 * which searches from a step to the agent's OWN goal; this keeps the whole route and moves
	 * the goal. For a caller that already knows where the agent goes next before it gets
	 * there - the rig test course joining one loop onto the next, as a real job would chain a
	 * return leg - and wants no stop at the join.
	 *
	 * FALSE AND NOTHING CHANGED when the agent is unknown or not Taxiing, or when Tail does not
	 * start at the node the live plan ends on (Splice's own precondition: two lines that do not
	 * meet would put a jump in the polyline). The goal moves exactly as RedirectAgent moves it,
	 * through the same two private calls (ReleaseGoal, TakeGoal): the old claim and any stand wait
	 * let go, the departure re-armed or disarmed for the NEW end, the new goal claimed.
	 * ENFORCED BY: Airside.Model.Traffic.ExtendRouteKeepsMoving, .ExtendRouteMovesTheGoal
	 *
	 * KeepBehind >= 0 ALSO TRIMS the driven history: whole steps ending more than KeepBehind uu
	 * behind the agent are dropped, and Travelled is rebased by the distance dropped (written to
	 * OutDropped), so a caller that extends for ever does not grow the route - or the per-tick
	 * walks over it - for ever. A caller keeping distances along the route (markers) subtracts
	 * OutDropped. Skipped, not forced, while the agent is held at a step (GetBlockedStep() >= 0):
	 * the arbitration names that step by index, and re-indexing it under a held agent is not
	 * worth the saving.
	 * ENFORCED BY: Airside.Model.Traffic.ExtendRouteTrimsHistory
	 */
	bool ExtendRoute(int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Tail,
		double KeepBehind = -1.0, double* OutDropped = nullptr);

	/**
	 * GROUND VEHICLES ONLY: an aircraft is refused (false, logged) - its route changes go through
	 * RedirectAgent and ReplanAt, which own its runway and stand claims.
	 *
	 * Sends a MOVING agent somewhere NEW without stopping it: the live plan's first KeepSteps
	 * steps, spliced with Tail (RouteSearch::Splice), handed to the follower with Replace - so
	 * Travelled, Speed, Heading and a tow's chain all carry on - and the goal moved to Tail's end
	 * through the same two calls RedirectAgent and ExtendRoute use (ReleaseGoal, TakeGoal).
	 *
	 * FOR A JOB CALLED OFF MID-DRIVE (final review, 2026-09-27): a fuel vehicle whose aircraft
	 * leaves before it arrives. RedirectAgent is a dispatch - it restarts the vehicle at the new
	 * plan's FIRST point, so a route home searched from anywhere but where the vehicle is put it
	 * there in one frame. ReplanAt keeps the goal; ExtendRoute keeps every step. This keeps the
	 * steps up to a node ahead of the agent and changes the goal - ReplanAt's splice with a new
	 * destination, and ReplanAt's bookkeeping: reservations for the route that no longer exists
	 * released, the arbitration fields and the stall clock cleared.
	 *
	 * KeepSteps MUST KEEP THE STEP THE AGENT IS ON (KeepSteps > CurrentStep), for ReplanAt's
	 * reason: Travelled survives, so a splice behind the agent re-maps it onto other geometry and
	 * it teleports. Tail must start where step KeepSteps - 1 ends.
	 *
	 * FALSE AND NOTHING CHANGED for an unknown or not-Taxiing agent, a KeepSteps that is not
	 * ahead of it, a Tail that does not join, or - for a tow - a spliced route that does not
	 * hold its trailer, judged WHOLE from the live chain (VehicleFit::JudgePlan): a tail searched
	 * from a straight lay can still fold a trailer the kept prefix has swung, and a reverse in it
	 * is solved from the chain the prefix leaves, which only the whole route knows.
	 * ENFORCED BY: AirportOps.Fuel.TowRecalledMidRouteGetsHome,
	 * AirportOps.Fuel.TruckRecalledMidRouteGetsHome
	 */
	bool RerouteAgent(int32 AgentId, const URoadNetwork* Network, int32 KeepSteps, const FRoutePlan& Tail);

	/**
	 * Sends a PARKED agent to whichever runway gives the shortest admitted taxi, with the
	 * take-off armed - the inspector's Depart button. Anything not Parked is refused as
	 * NotParked: a taxiing aircraft has a plan, an arriving one is not on the ground, a
	 * departing one is already going. Composed from PlanAny and RedirectAgent so there is
	 * one arming path (ArmDepartureIfRunway) and one engine restart (StartTaxi).
	 *
	 * A PushbackBlocked refusal also puts the aircraft on the push watch - see OnPushGroundFreed. Any other
	 * answer takes it off.
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
	 * EVERY CLAIM IS RE-MADE AT ONCE, not on the next tick: OnGraphRebuilt ends by running the
	 * tick's own claim pass (Arbitrate) with no motion, before its freed diff. "The next tick"
	 * was not soon enough - DepartAgent and the ops drain read this table between ticks, and
	 * a paused game has no next tick (push-ground-freed review I1): a taxiing body missing from
	 * the table read as free ground, and a push was granted into it.
	 * ENFORCED BY: Airside.Model.Traffic.PushGroundFreed.RebuildKeepsTaxiingBlocker
	 *
	 * AIRCRAFT TOO, unlike dispatch-time routing (spec §4): a route fixed at clearance is
	 * still a route over pavement, and pavement the player has just deleted is not something
	 * an aircraft can be held to.
	 */
	void OnGraphRebuilt(const URoadNetwork& Network);

	/**
	 * Puts a STRANDED agent back on the pavement - the player's Unstick (spec
	 * 2026-09-29-unstick-agent), never the simulation's: a stranding stays final for everything
	 * automatic (see ReResolvePlan's Strand).
	 *
	 * Hops the agent onto the nearest point, within RescueRejoinRadius (15 m), of a live edge
	 * running the way it faces with a route to Goal - its OWN goal when Goal is unset - and taxis
	 * on from there with its speed and heading kept (FRoadAgent::RejoinTaxi). The goal moves
	 * through ReleaseGoal/TakeGoal like every other goal change, so the old goal's claim lets go.
	 * Broadcasts Stranded -> Taxiing.
	 *
	 * FALSE AND NOTHING CHANGED for an unknown id, an agent that is not Stranded (a moving one is
	 * ReplanAt's), a Goal that is no live node, or no such pavement within the radius.
	 * ENFORCED BY: Airside.Model.Traffic.RescueStranded.Rejoins, .Refuses, .NewGoal
	 */
	bool RescueStranded(int32 AgentId, const URoadNetwork& Network, FGuidelineNodeId Goal);

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
	 * DeadlockResolver.Resolve (see FDeadlockResolver) reads those stall clocks, and may
	 * replan one agent per wait-for cycle.
	 *
	 * ARBITRATION FIRST AND MOTION SECOND, never interleaved: a claim must be visible to
	 * every agent before any of them moves on it, or the last agent in the list drives
	 * through a node the first one took in the same frame.
	 *
	 * Network null means no arbitration - every agent drives as if alone, which is the
	 * pre-M2 behaviour and what a caller with no graph yet gets.
	 */
	void Advance(double DeltaSeconds, const URoadNetwork* Network);

	// MaxSubstepSeconds AND MaxSubsteps MOVED TO FTrafficRules (#107 item 6): both were
	// UPROPERTY(EditAnywhere) here, but this class is a Transient, non-instanced UObject one
	// layer below ARoadNetworkActor and never exposed EditAnywhere itself, so neither figure
	// could ever reach the Details panel - see FTrafficRules's own comment on the two. Read
	// as Rules.MaxSubstepSeconds / Rules.MaxSubsteps now.

	/** How many agents are currently under way or parked at their destination. */
	int32 GetAgentCount() const { return Agents.Num(); }

	/** Id of the most recently dispatched agent, or 0 when nothing is under way. */
	int32 GetNewestAgentId() const { return Agents.Num() > 0 ? Agents.Last().Id : 0; }

	const TArray<FRoadAgent>& GetAgents() const { return Agents; }
	const FRoadAgent* FindAgent(int32 AgentId) const;
	const FTrafficOccupancy& GetOccupancy() const { return Occupancy; }

	/**
	 * Bumped whenever occupancy changes in a way ArrivalPlanner::Plan could answer
	 * differently for - a stand held or freed, a runway taken or cleared (issue #169).
	 *
	 * NOT every Occupancy mutation: a taxiing agent's edge and node claims churn every tick
	 * (Arbitrate re-claims the whole table each frame) and Plan never reads them - only
	 * IsHeld on a stand's PoseNode and IsAnyHeld on RunwaySurfaces do. The sites that bump
	 * this are exactly the ones that can move one of those two answers: Admit/RetireAgent/
	 * ClearAgents (an agent's claims appear or vanish wholesale), RedirectAgent (frees the
	 * old goal, claims the new one), HoldStand/ReleaseHold (AirportOps's own reservation,
	 * with no agent involved at all), and AdvanceOnce's LinedUp/Airborne/removal/generic
	 * phase-changed branches (a departure claims or releases the runway chain it holds -
	 * see AdvanceOnce's own comment on why Airborne is not even a phase change and so is not
	 * covered by watching phase alone).
	 *
	 * SAME IDIOM AS ULedger::Revision AND URoadNetwork::GetGuidelineRevision: a plain
	 * session counter, not a UPROPERTY - a poller's cheapest question is "has anything
	 * changed since the number I remember", and this is the number for occupancy.
	 *
	 * WHAT THIS DOES NOT COVER: a taxiing (not landing or departing) agent crossing a live
	 * runway holds its surface through the same per-tick claim pass as an edge, with no
	 * discrete claim/release event of its own - see FClaimPass::Run (TrafficClaims.cpp).
	 * A cache keyed on this revision can therefore under-report "runway in use" for the
	 * width of one such crossing. Accepted rather than chased: it self-corrects on the very
	 * next bump (arrivals, departures and stand holds are the common case in a live
	 * airport), and ArrivalPlanner::Plan itself is unaffected - only a viewmodel's CACHE of
	 * its answer would read stale, for at most the length of one crossing.
	 */
	uint32 OccupancyRevision() const { return OccupancyRevisionCount; }

	/**
	 * Every wait cycle nobody in can be turned out of, right now, as member agent ids - FDeadlockResolver::AlertCycles
	 * over this model's agents: every member an aircraft or an agent that cannot be replanned (a reversing truck, a
	 * pushed aeroplane - #455). For the ops Deadlock alert: the resolver logs these, and a log line is not
	 * something a player sees. Out is reset first.
	 */
	void CurrentDeadlocks(TArray<TArray<int32>>& Out) const;

	/**
	 * The agent currently holding Node, or 0 if nobody is (0 is never a real agent id - see
	 * FRoadAgent::Id). Wraps FTrafficResource::OfNode and IsHeld's ExcludingAgent=0 idiom
	 * ("exclude no real agent") so a caller outside Model/ can ask "who is here" without
	 * knowing either exists - Tool/StandPlaceTool used to build the resource and read the
	 * sentinel itself (#104).
	 */
	int32 HolderOfNode(FGuidelineNodeId Node) const;

	/**
	 * AgentId's planned polyline, road-plane, or an empty array if there is no such agent or
	 * it is not Taxiing. NOT trimmed to what is actually left to drive - Travelled advances a
	 * point along this same array (see FRouteFollower::Advance) but nothing shortens the
	 * array itself, so this is the whole plan every frame. Good enough for the one picture
	 * that reads it (FSelectTool's route preview); a true "from here to the end" trim is
	 * future work if something needs it. Exists so Tool/ names the agent it wants rather than
	 * reaching Agent->Follower.Plan.Polyline itself (#104).
	 */
	const TArray<FVector2D>& RemainingRoute(int32 AgentId) const;

	/**
	 * RemainingRoute cut into forward and reverse runs (FRoutePlan::DescribeRuns), for the route
	 * view's two styles (spec 2026-09-26 §5) - and answered while Reversing too, which
	 * RemainingRoute is not. While a TOW is backing, the run it is backing along is the path its
	 * solved trailer axle actually takes (FTowReverseRun's samples), not the raw leg: what is
	 * drawn is what is driven. Empty for no such agent or one neither taxiing nor reversing.
	 */
	TArray<FRouteRun> RemainingRouteRuns(int32 AgentId) const;

	/** True after a stand claim was released or a rebuild ran, until Advance's re-offer pass
	 *  consumes it. For Airside.Model.Traffic.StandClaim. */
	bool StandsMayHaveFreedForTest() const { return bStandsMayHaveFreed; }

	/**
	 * Empties AgentIndex WITHOUT touching Agents - the exact shape a duplicated UGroundTraffic
	 * is in before its first lookup (see AgentIndex's own comment: DuplicateObject's property
	 * walk never reaches a plain C++ member, so a copy starts with this default-constructed
	 * empty regardless of how many agents came along with it). Airside.Model.Traffic.
	 * FindAgentSurvivesAStaleIndex uses this to pin FindIndex's count-mismatch guard without
	 * staging an actual PIE duplication, which nothing in this test module can drive headlessly.
	 */
	void ClearAgentIndexForTest() { AgentIndex.Reset(); }

	/**
	 * The table, writable, for a test that has to plant a claim no agent owns - the phantom
	 * occupant Airside.Model.Traffic.BoxEntryFirstOnly parks on a node, or a runway held by
	 * something the model has no agent for. Production code reaches the table only through
	 * the claim pass; a second writer would be the second evaluator this class exists to
	 * prevent, which is why this says ForTest in its name.
	 */
	FTrafficOccupancy& OccupancyForTest() { return Occupancy; }

	// Strand, BeginCrossing and ReplanAt (staging plan-death, a mid-crossing landing, and a
	// pinned-search replan no test could otherwise reach without staging the whole scenario
	// that would trigger it in production) moved behind FGroundTrafficTestAccess, declared
	// after this class (#104) - three public mutators nothing in production ever calls, each
	// documented ForTest in its own comment, are now one friend struct that says so once.

	double GetSimSeconds() const { return SimSeconds; }

	/**
	 * How many substeps the most recent Advance call split its DeltaSeconds into.
	 *
	 * THE ONLY WAY TO SEE THE SPLIT FROM OUTSIDE (#107 item 5): Advance's step count is not
	 * otherwise observable, and the defect this exists to pin - DeltaSeconds crossing the
	 * Present/Model seam as a float (UAirsideTraffic::Advance used to take one) - shows up
	 * ONLY in this count: float(1.0/30.0) is very slightly LARGER than the double it should
	 * equal, so dividing by MaxSubstepSeconds and taking CeilToInt rounds up at every exact
	 * multiple of a substep - one spurious extra step at 30 Hz x1, one at 60 Hz x4. The
	 * follower's own physics do not show this reliably (FTrafficSubstepTest's near-exact
	 * SplitGap), so the count is what a test can actually assert.
	 */
	int32 GetLastStepsForTest() const { return LastStepsForTest; }

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
	int32 GetCyclesDetectedForTest() const { return DeadlockResolver.CyclesSeen.Num(); }

	/**
	 * How many deadlock lines - resolved or unresolvable - this session has emitted.
	 *
	 * THE RETRY STAMP IS WHAT MAKES THIS PERIODIC. An unresolvable cycle is re-detected on
	 * every single tick; LastResolveAttempt is why it is reported once per Rules.RetrySeconds
	 * instead, and this is the count that measures it.
	 */
	int32 GetDeadlockLogLinesForTest() const { return DeadlockResolver.DeadlockLogLines; }

	/** Id of the last agent whose deadlock replan SUCCEEDED, or 0 if none ever has. */
	int32 GetLastResolvedAgentForTest() const { return DeadlockResolver.LastResolvedAgent; }

	/** How many reservation cycles were settled by a yield rather than a replan. See §5. */
	int32 GetYieldsForTest() const { return DeadlockResolver.Yields; }

	/** Id of the last agent that yielded its reservations, or 0 if none has. */
	int32 GetLastYieldedAgentForTest() const { return DeadlockResolver.LastYieldedAgent; }

	// NO GetDeadlockResolverForTest() (issue #84 review): a test that wants to drive an
	// FDeadlockResolver directly constructs its own - Airside.Model.Traffic.
	// DeadlockResolverStandalone does exactly that - rather than reading this instance's,
	// which would have had zero callers. The five accessors above stay for every existing
	// caller of UGroundTraffic's own resolver.

	/** What the last OnGraphRebuilt did. See FGraphRebuildSummary for why a test needs it. */
	FGraphRebuildSummary GetLastRebuildSummaryForTest() const { return LastRebuild; }

	/** Actual RunwayQuery::RunwayChain walks the claim pass has forced through RunwayChains
	 *  this session - see FRunwayChainCache::GetWalksForTest (issue #170). */
	int32 GetRunwayChainWalksForTest() const { return RunwayChains.GetWalksForTest(); }

	// FClaimGeometry MOVED TO FClaimPass (Model/TrafficClaims.h) IN ISSUE #84, with the claim
	// pass it belongs to. Airside.Model.Traffic.ClaimGeometry now includes that header instead
	// of this one.

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

	/**
	 * Id -> index into Agents, MAKING FindIndex O(1) (issue #295). FindIndex/FindAgent used to
	 * be Agents.IndexOfByPredicate - an O(N) scan called from every dispatch, redirect and
	 * extend, and FDeadlockResolver::Resolve ran the same scan again, unindexed, inside its
	 * OWN Sort comparators (O(N log N) calls, each an O(N) scan: O(N^2 log N) for one cycle).
	 *
	 * REBUILT WHOLESALE ON EVERY Admit/RetireAgent/AdvanceOnce-removal, not maintained
	 * incrementally: Agents.RemoveAt SHIFTS every later index, so an incremental update would
	 * touch as many entries as a rebuild does, and a rebuild is simpler to get right under the
	 * re-entrancy AdvanceOnce's own comment describes (a broadcast can retire a DIFFERENT agent
	 * from inside this same call). Admission and retirement are rare next to the many lookups
	 * a single tick makes, so the O(N) cost lands where it is cheap to pay it.
	 *
	 * Not a UPROPERTY: an index into a Transient array is not state a save would ever need,
	 * the same reason ArbitrationOrder above is not one.
	 *
	 * MUTABLE, WITH FindIndex REBUILDING IT ON A COUNT MISMATCH (PR review on issue #295): a
	 * duplicated UGroundTraffic (PIE's level duplication) copies Agents - a reflected UPROPERTY,
	 * even though Transient - but this plain C++ member is invisible to DuplicateObject's
	 * property walk, so a duplicate starts with it default-constructed empty regardless of how
	 * many agents came along. Every FindAgent on the duplicate would silently miss until the
	 * next Admit/RetireAgent/AdvanceOnce-removal happened to rebuild it - which, for a level
	 * that admits nothing new, could be never. See FindIndex.
	 */
	mutable TMap<int32, int32> AgentIndex;

	/** Rebuilds AgentIndex from Agents. See AgentIndex's own comment for why wholesale. Const
	 *  (and AgentIndex mutable) because FindIndex, a const lookup, is what actually calls this
	 *  on a stale-cache guard - the cache the agent lives in does not change, only the index. */
	void RebuildAgentIndex() const;

	UPROPERTY(Transient) FTrafficOccupancy Occupancy;

	/**
	 * Arbitrate's rank order, PROMOTED FROM A LOCAL (issue #190): a fresh TArray<int32> used
	 * to be built and sorted every Arbitrate() call - once a substep, not once an agent, but
	 * still a malloc/free for an array whose size barely changes tick to tick. Reset and
	 * refilled at Arbitrate's own top; kept as a member purely so its capacity survives
	 * between calls. Not a UPROPERTY - rebuilt every call, nothing a save would ever need.
	 */
	TArray<int32> ArbitrationOrder;

	/**
	 * Occupancy::TakePreempted's destination, PROMOTED FROM ITS RETURN VALUE (issue #190):
	 * TakePreempted used to return a TSet<int32> BY VALUE, moving its own internal storage
	 * out to the caller and leaving itself to rebuild one from nothing the next time an
	 * agent is actually preempted - rare, but the reallocation this caused was not. Reset
	 * and refilled at the top of every Arbitrate() call; see FTrafficOccupancy::TakePreempted
	 * for the other half. Not a UPROPERTY, for the same reason as ArbitrationOrder above.
	 */
	TSet<int32> PreemptedScratch;

	/**
	 * How far each node's claim reaches along each of its edges - see NodeReach.h for the
	 * bug this exists for. MUTABLE because the claim pass is const over the agent it is
	 * building for and this is memoisation of the graph, not state of the simulation:
	 * dropping it changes nothing an agent does, only how much geometry the next tick
	 * re-samples.
	 */
	mutable FNodeReachCache NodeReach;

	/**
	 * Which segments make up the strip a runway seed belongs to, memoised - see
	 * FRunwayChainCache for the bug this exists for (issue #170) and why EditRevision is the
	 * right key. MUTABLE for the same reason as NodeReach above: memoisation of the graph,
	 * not simulation state, so a const claim pass may still fill it in.
	 */
	mutable FRunwayChainCache RunwayChains;

	/** Sim seconds elapsed through Advance. The deadlock resolver's retry clock. */
	UPROPERTY(Transient) double SimSeconds = 0.0;

	/** How many substeps the last Advance call took. See GetLastStepsForTest. Not a
	 *  UPROPERTY: bookkeeping about the last call, not state a save would ever need. */
	int32 LastStepsForTest = 0;

	/**
	 * The wait-for graph and its cycle bookkeeping (issue #84) - CyclesSeen, YieldedAt,
	 * Yields, LastYieldedAgent, DeadlockLogLines and LastResolvedAgent used to be members
	 * here; see FDeadlockResolver's own comment for why they are its public fields now
	 * instead. Constructed once and kept, unlike FClaimPass: its bookkeeping must persist
	 * from tick to tick.
	 */
	FDeadlockResolver DeadlockResolver;

	/** The replan mechanism FDeadlockResolver::Resolve and OnGraphRebuilt both call into.
	 *  See FPlanReResolver - stateless, so this exists mainly for a consistent calling
	 *  convention (PlanReResolver.ReplanAt(...)) rather than because it must persist. */
	FPlanReResolver PlanReResolver;

	/** What the last OnGraphRebuilt did. Test-facing, as above. */
	FGraphRebuildSummary LastRebuild;

	/**
	 * The departure verdict last LOGGED per aircraft, empty once it departed. DepartAgent is
	 * retried every tick by UJobBoard until it succeeds - so lengthening a runway releases
	 * the aircraft - and a refusal logged per call wrote one line 14,944 times in three
	 * minutes (2026-09-27, Airside.Model.Traffic.DepartRefusalSaidOnce). A session log gate,
	 * not state - not a UPROPERTY, not saved.
	 */
	TMap<int32, FString> LastDepartVerdict;

	/**
	 * The drive side the last OnGraphRebuilt saw, so the next can tell a flip from any other
	 * edit (FTrafficContext::bLanesMirrored). Unset until the first rebuild. Not a UPROPERTY:
	 * it describes what this session's agents were planned against, not saved state.
	 */
	TOptional<EDriveSide> LastDriveSide;

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
	 * THE GOAL CHANGE, in two halves because RedirectAgent restarts the follower between them:
	 * ReleaseGoal lets the old goal's claim and any stand wait go; TakeGoal sets the new goal
	 * from Plan, arms or disarms its departure and claims it. RedirectAgent and ExtendRoute
	 * both call exactly these, so the two ways of moving a goal cannot drift apart.
	 */
	void ReleaseGoal(FRoadAgent& Agent, int32 AgentId);
	void TakeGoal(FRoadAgent& Agent, int32 AgentId, const URoadNetwork* Network, const FRoutePlan& Plan);

	/**
	 * Everything both DispatchAgent overloads do once the agent is started with its bundle:
	 * the pause, the reverse speed, class, goal, departure arming, the posing Advance, Admit
	 * and the goal claim. ONE BODY, so the two overloads differ in the Start* call and in
	 * nothing else - a second copy is where an aircraft and a truck would start to disagree.
	 */
	int32 AdmitDispatched(FRoadAgent&& Agent, const URoadNetwork* Network, const FRoutePlan& Plan,
		ETraversalClass Class, double ShutdownPauseSeconds);

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
	 * Every aeroplane holding at the end of a push for a taxi out it can drive (see
	 * FRoadAgent::bTaxiOutStale) is given one: DeparturePlanner::PlanAny from the live node
	 * nearest where it stands, joined to it by a short leg so it DRIVES there rather than
	 * appearing there, and re-armed from the new route's end. None found: it keeps holding,
	 * said once, and is asked again when the guideline graph moves - the player's fix is an
	 * edit, and every edit moves it - so the fix releases it, the user's ruling for this case
	 * (2026-09-27). It was asked every tick until ops batch 3 PR E; see the .cpp for why the
	 * graph's revision is every input of a refusal. Not while the graph is mid-edit: the route
	 * must be on the lines the player is about to see.
	 */
	void ReplanHeldTaxiOuts(const URoadNetwork& Network);

	/** One bounded step. Advance splits a long frame into these - see FTrafficRules::MaxSubstepSeconds. */
	void AdvanceOnce(double DeltaSeconds, const URoadNetwork* Network);

	// ClaimAhead's full header (the numbered sequence, the box-junction entry rule, the two
	// rejected alternatives, the runway surface routes) MOVED WITH IT to FClaimPass::Run
	// (issue #84) - Model/TrafficClaims.h. Along with it: FClaimWindow, FClaimBody,
	// FWantedClaim, HoldRunwayOnly, ReleaseForDeadPlan, WindowFor, SampleBody, UpdateCrossing,
	// BuildPending, ApplyClaims, StopWithinFor, RankAt, ReachExcessAt. Arbitrate constructs
	// one FClaimPass per call and calls its Run where this used to call ClaimAhead.

	/**
	 * Claims Agent's GoalNode as a stand reservation right now, for the between-ticks
	 * window DispatchArrival reads the table in. Used by both dispatches and the redirect.
	 *
	 * NOTE: the per-tick version of this claim is FClaimPass::ClaimGoalNode now (issue #84).
	 * This one remains on UGroundTraffic because it fires at DISPATCH, between ticks,
	 * before any FClaimPass exists for this agent.
	 */
	void ClaimGoalNodeAtDispatch(const FRoadAgent& Agent, int32 Id, const URoadNetwork& Network);

	/**
	 * Is every edge and node a push off the stand will touch free of everyone else?
	 *
	 * PUSHBACK CLEARANCE, asked ONCE before the manoeuvre starts rather than tick by tick. A
	 * manoeuvring agent cannot replan - a stand's lead-in is the only way off it - so it has
	 * no move for the deadlock resolver to find, and a push that could be stopped half way
	 * would be a phase able to block a taxiway indefinitely with nothing able to act on it.
	 * Granting the whole thing up front makes it atomic, which is also what ground control
	 * does: clearance is granted or withheld, never half-granted.
	 */
	bool IsPushGroundFree(int32 AgentId, const FRoutePlan& Plan, double PushDistance) const;

	/**
	 * A REVERSE LEG'S CLEARANCE, the twin of IsPushGroundFree and asked the same way (#455): before an agent that is
	 * standing at the start of a reverse span may arm it, is every edge and node of that span free of everyone else?
	 * When something holds any of it the agent is REFUSED at the span (Refuse, naming the holder), and
	 * FRoadAgent::TryArmReverseLeg reads that refusal and waits at the service point instead of arming: the reverse
	 * WAITS, it does not start into held ground and sit mid-span.
	 *
	 * WHY THE SAME RULE AS THE PUSH: a reverse is the other manoeuvre with no second way out (the bay's leg is the
	 * only line), and #453 made the claim pass hold its whole span once it is Reversing. Held from the first frame
	 * but not gated, a reverse into ground somebody else held began, was refused a few metres on, and stopped in
	 * the span - across the very lane it was meant to clear.
	 *
	 * CALLED BEFORE EVERY Advance THAT CAN ARM A REVERSE, and there are three: the tick's (AdvanceOnce), a dispatch's
	 * zero-second pose (AdmitDispatched) and a redirect's (RedirectAgent) - the last two are how every stand
	 * service cycle's route home, which OPENS with its reverse leg, has always armed, before any claim pass has run.
	 * A NO-OP for every agent not at a span's start, so the common frame pays one cursor check.
	 * ENFORCED BY: Airside.Model.Traffic.ReverseWaitsForHeldGround (the tick), Airside.Model.Traffic.
	 * ReverseFromDispatchWaitsForHeldGround, Airside.Model.Traffic.ReverseFromRedirectWaitsForHeldGround
	 */
	void GateReverseLeg(FRoadAgent& Agent) const;

	/**
	 * Offers every waiting aircraft (bAwaitingStand) the best free stand reachable from where
	 * its route ends, by the verb its phase calls for (below). Runs at the end of Advance when
	 * bStandsMayHaveFreed; one pass, then the flag clears whether or not anyone was placed.
	 *
	 * BY PHASE, NOT BY GOAL (issue #435): a TAXIING waiter is still moving, and its GoalNode is
	 * the end of the route it has not finished - so the way to a stand starts exactly where its live
	 * plan ends, which is ExtendRoute's own precondition, and it is extended IN PLACE: speed,
	 * heading and distance driven all carry on. RedirectAgent restarts an aircraft from REST at the
	 * new route's first point, which for a moving waiter is the far end of the route ahead of it -
	 * a teleport. So RedirectAgent is only for a waiter that is standing at that point already:
	 * Parked on the fallback junction, or Stranded. A Taxiing waiter whose extension is refused
	 * keeps waiting and is asked again the next time something frees - or when it stops at the end
	 * of its route (#455), which is what asks it again if nothing else does; it is never redirected
	 * while it is moving.
	 * ENFORCED BY: Airside.Model.Traffic.ReofferTaxiingWaiterDoesNotJump (the extension),
	 * Airside.Model.Traffic.ReofferRefusedExtensionKeepsWaiting (the refusal)
	 */
	void ReofferStands(const URoadNetwork& Network);

	/**
	 * Set when a stand claim is released (redirect, retire, Gone) or the graph is rebuilt;
	 * consumed by Advance's re-offer pass. A FLAG rather than an event: the table is rebuilt
	 * per tick and the model is world-free, so one bool checked per frame is the cheapest
	 * correct thing.
	 */
	bool bStandsMayHaveFreed = false;

	/** See OccupancyRevision. Not a UPROPERTY, the same reason LastStepsForTest above is
	 *  not: a session counter about calls made, not state a save would ever need. */
	uint32 OccupancyRevisionCount = 0;

	/**
	 * The freed diff - OnRunwayFreed, OnStandsFreed and OnPushGroundFreed. Runs at the end of Advance (after the substeps, so a
	 * release and a re-hold inside one frame fire nothing - nobody could have acted between them) and at the
	 * end of OnGraphRebuilt, bRebuilt true: a rebuild re-reads the runways at once, and whatever vanished is
	 * not held now, so it is freed - a deleted runway is no longer busy.
	 *
	 * AND AT THE END OF EVERY RELEASE OUTSIDE ADVANCE - RetireAgent, ReleaseHold, ClearAgents (PR D review M1),
	 * through DiffNow: a freed runway or stand waiting for the next Advance is an event that never comes while
	 * the motion is paused, and the arrival queue's safety net then lands the flight with a false Warning.
	 *
	 * A DIFF RATHER THAN A POLL BY EACH LISTENER: the question is asked once here, where the table lives, and
	 * AirportOps hears a change instead of asking every runway every frame. Cost: one IsChainHeld per runway
	 * and one IsStandHeld per stand, plus a set/map lookup each for the baseline, per call - 2 runways and 30
	 * stands on the largest test field (FTestAirport::BuildScale, 2026-09-30).
	 */
	void DiffFreedom(const URoadNetwork& Network, bool bRebuilt);

	/** DiffFreedom on the network the last one read (DiffNetwork), for a release made between ticks with no
	 *  network to hand. Nothing if there has been no diff yet, or that network is gone. */
	void DiffNow();

	/** The network DiffFreedom last read. WEAK: a level change can take it away between two calls. */
	TWeakObjectPtr<const URoadNetwork> DiffNetwork;

	/**
	 * Every runway's seed, one per strip (AirsideCapability::SummariseRunways), as of RunwaySeedsRevision on
	 * RunwaySeedsNetwork. RE-READ WHEN THE ROAD TOPOLOGY MOVES (GetEditRevision) or the network object
	 * changes, not per Advance: which segments make a strip is topology - FRunwayChainCache's own argument -
	 * and SummariseRunways walks every segment. Plain members: derived, never saved.
	 */
	TArray<FRoadSegmentId> RunwaySeeds;
	uint32 RunwaySeedsRevision = 0;
	const URoadNetwork* RunwaySeedsNetwork = nullptr;

	/** The strips held at the last DiffFreedom - the baseline the next one compares against. */
	TSet<FRoadSegmentId> HeldRunways;

	/**
	 * The stands held at the last DiffFreedom, BY ENTITY (review M2), each with the pose node it had then - the
	 * pose OnStandsFreed reports, which for a deleted stand no longer exists anywhere else. By entity rather than
	 * by pose so the baseline names the thing a flight holds, not a graph handle.
	 */
	TMap<FEntityInstanceId, FGuidelineNodeId> HeldStands;

	/** Re-reads RunwaySeeds when the network object or its topology moved, or bForce (a rebuild). */
	void RefreshRunwaySeeds(const URoadNetwork& Network, bool bForce);

	/** The strips IsChainHeld says are held now, over RunwaySeeds - the set DiffFreedom diffs, and the push
	 *  watch's record of what PlanAny ranked by. */
	TSet<FRoadSegmentId> RunwaysHeldNow(const URoadNetwork& Network) const;

	/**
	 * THE PLANNING HALF OF DepartAgent, with no log line and no side effect: PlanAny, the straight-out test, the
	 * push plan and the push-ground question, in DepartAgent's order. Defined in GroundTraffic.cpp beside it -
	 * the return type holds FPushbackPlan, a Private header's type, so it is only declared here. DepartAgent
	 * logs and acts on its answer; the push watch re-asks it. ONE copy of the decision, so the watch cannot
	 * call a push free that DepartAgent would refuse, or the reverse.
	 */
	FDepartureAsk AskDeparture(const FRoadAgent& Agent, const FAirframe& Aircraft, const URoadNetwork& Network) const;

	/**
	 * One aircraft DepartAgent refused PushbackBlocked: the push route it asked about, and every input its
	 * planning read besides the route's own ground - the network object and both revisions (topology, facts,
	 * AreGuidelinesBehindRoad) and the strips held when PlanAny ranked them. While they all hold, the plan is
	 * the same plan and only IsPushGroundFree(PushRoute) can change; when any moves, AskDeparture is asked
	 * whole. The parked aircraft's own inputs (goal node, airframe, class, heading) do not move while Parked.
	 */
	struct FPushWatch
	{
		FRoutePlan PushRoute;
		/** Identity only, never dereferenced; weak for DiffNetwork's reason. */
		TWeakObjectPtr<const URoadNetwork> Network;
		uint32 EditRevision = 0;
		uint32 GuidelineRevision = 0;
		TSet<FRoadSegmentId> RunwaysHeld;
	};

	/**
	 * THE PUSH WATCH, by agent id - see OnPushGroundFreed. NOT SAVED: agents are not saved, and neither is
	 * anything that names one. A REBUILD RE-DERIVES IT rather than clearing it: the rebuild moves the guideline
	 * revision, so DiffFreedom asks every entry whole. Clearing would leave the aircraft's wake-up to a caller
	 * retrying on its own NetworkChanged - a contract with a plugin this one must not know exists. The re-derive
	 * reads a WHOLE table: OnGraphRebuilt re-runs the tick's claim pass (Arbitrate) before its diff, so every
	 * non-stranded body on the push ground is there to be seen - a taxiing one used to be missing until the next
	 * Advance, and the watch read the ground free and woke the aircraft into it (review I1).
	 * ENFORCED BY: Airside.Model.Traffic.PushGroundFreed.RebuildRederives, Airside.Model.Traffic.PushGroundFreed.RebuildKeepsTaxiingBlocker
	 *
	 * Cost, per DiffFreedom: one IsPushGroundFree per entry - an IsAnyHeld over 2 resources per push step, and a
	 * push is 2-4 steps - plus a revision compare and a set compare of 0-2 strips. Entries: 0-2 in every test
	 * field (2026-09-30); a replan (PlanAny, a route search per departing runway) only
	 * when the key moves.
	 */
	TMap<int32, FPushWatch> PushWatch;

	/** See StandHoldChangeCount. A session counter, not saved. */
	uint32 StandHoldChanges = 0;

	/** See TaxiOutReplanAttemptsForTest. */
	int32 TaxiOutReplanAttempts = 0;

	/**
	 * The network ReplanHeldTaxiOuts last gated against. A revision numbers ONE network's edits, and a new network
	 * object (ClearNetwork, a load) starts its own count - so a refusal remembered on the old one is forgotten when
	 * this changes, rather than matched by coincidence against the new one's number. Identity only, never
	 * dereferenced; weak for the same reason DiffNetwork is.
	 */
	TWeakObjectPtr<const URoadNetwork> TaxiOutGateNetwork;

	// ResolveDeadlocks, CanReplanAtBlockedStep, EReResolve, ReResolvePlan and SpliceReplan ALL
	// MOVED to FDeadlockResolver / FPlanReResolver (issue #84) - both declared above, near
	// FTrafficRules. ReplanAt and OnGraphRebuilt (both still public, above) now forward into
	// them; see PlanReResolver and DeadlockResolver, the members that hold the instances.

	/**
	 * Makes AgentId's plan unusable where it stands, keeping the agent and its position.
	 * True if the id was known.
	 *
	 * IT STANDS IN FOR THE PAVEMENT GOING AWAY UNDER A MOVING AGENT - a rebuild that leaves
	 * a step with no live edge and no route to replace it, or a redirect that lands a bad
	 * plan on a live follower. Both end in FClaimPass::Run's dead-plan branch (issue #84
	 * moved it off UGroundTraffic's own ClaimAhead), which must release every claim the
	 * agent holds; without a hook there is no world-free way to reach that branch, and a
	 * seam no test reaches is one a later edit can quietly unwire (see
	 * Airside.Model.Traffic.DeadPlanReleases).
	 *
	 * Not public, and reached only through FGroundTrafficTestAccess (#104): nothing in
	 * production invalidates a plan by hand - OnGraphRebuilt truncates or strands through
	 * FPlanReResolver::ReResolvePlan, which is a decision, not an assignment.
	 */
	bool StrandForTest(int32 AgentId);

	/**
	 * Puts a Taxiing agent ON a runway the way the Vacated handover does - CrossingRunway
	 * set, phase OnStrip - so a world-free test can stage an aircraft that has just landed
	 * without a stand definition, which DispatchArrival needs and a bare automation run
	 * has not got. Stands in for FRoadAgent::Advance's Arriving -> Taxiing frame and
	 * nothing else; the geometric release then runs as in play. False for an unknown or
	 * non-Taxiing agent. Not public - see FGroundTrafficTestAccess (#104).
	 */
	bool BeginCrossingForTest(int32 AgentId, FRoadSegmentId RunwaySeed);

	/**
	 * Swaps a Vehicle-bodied agent's FVehicle in place, figures only - no re-lay of its tow, no
	 * re-plan. So a world-free test can make the vehicle ALREADY OUT no longer fit the road it
	 * will be sent home on: UJobBoard routes a truck home by the agent's own vehicle, and
	 * that is set once, at dispatch. False for an unknown agent or an aircraft. Not public -
	 * see FGroundTrafficTestAccess (#104).
	 */
	bool SetVehicleForTest(int32 AgentId, const FVehicle& Vehicle);

	/**
	 * Refuses AgentId on Resource, held by BlockerId, and sets its stall clock to StalledSeconds -
	 * the state one claim pass plus that long a wait would leave. So a test can stage a wait, or a
	 * two-aircraft deadlock ring, without the geometry that would jam two aircraft for real (and
	 * the resolver that would then replan them). The next Advance's claim pass overwrites it.
	 * False for an unknown agent. Not public - see FGroundTrafficTestAccess (#104).
	 */
	bool ScriptWaitForTest(int32 AgentId, const FTrafficResource& Resource, int32 BlockerId, double StalledSeconds);

	/**
	 * Points AgentId's GoalNode at Goal without touching its plan, so the goal no longer names
	 * where the live plan ends. The one state that makes ReofferStands' ExtendRoute REFUSE for an
	 * aircraft (its re-offered route starts at GoalNode, and Splice wants it to start where the
	 * live plan ends) - a state nothing in production can reach, which is exactly why the "keeps
	 * waiting, never redirected" path needs a hook to be tested at all (issue #435's review).
	 * False for an unknown agent. Not public - see FGroundTrafficTestAccess (#104).
	 */
	bool SetGoalForTest(int32 AgentId, FGuidelineNodeId Goal);

public:
	/** Route distance at which Step begins - the previous step's end, or 0. Public: FClaimPass,
	 *  FDeadlockResolver and FPlanReResolver all read plan geometry through this and the two
	 *  below, which is why the three share it rather than each keeping a copy. */
	static double StepStart(const FRoutePlan& Plan, int32 Step);

	/** The node Step leaves from: the previous step's To, or the plan's Start. */
	static FGuidelineNodeId StepFromNode(const FRoutePlan& Plan, int32 Step);

	/** Which step Travelled is on. The map from a distance to an edge, read off
	 *  FRouteStep::EndDistance so it cannot disagree with the polyline the follower walks. */
	static int32 CurrentStep(const FRoutePlan& Plan, double Travelled);
};

/**
 * Every mutation of a UGroundTraffic a test can make that production code never does by a
 * direct call - a plan invalidated by hand, an aircraft staged mid-crossing, a replan pinned
 * to a search a test chose rather than one the resolver decided on (#104).
 *
 * ONE friend struct rather than three public "ForTest" methods on UGroundTraffic itself: each
 * of the three used to carry its own paragraph explaining why a normal caller must never use
 * it, which is exactly the shape of fact a type system should enforce rather than a comment -
 * a test constructs this wrapper around the instance it wants to drive, and nothing else can.
 * Stateless and cheap to construct per call; it holds nothing but the reference.
 */
struct FGroundTrafficTestAccess
{
	explicit FGroundTrafficTestAccess(UGroundTraffic& InTraffic) : Traffic(InTraffic) {}

	/** See UGroundTraffic::StrandForTest's own comment. */
	bool Strand(int32 AgentId) { return Traffic.StrandForTest(AgentId); }

	/** See UGroundTraffic::SetVehicleForTest's own comment. */
	bool SetVehicle(int32 AgentId, const FVehicle& Vehicle) { return Traffic.SetVehicleForTest(AgentId, Vehicle); }

	/** See UGroundTraffic::ScriptWaitForTest's own comment. */
	bool ScriptWait(int32 AgentId, const FTrafficResource& Resource, int32 BlockerId, double StalledSeconds)
	{
		return Traffic.ScriptWaitForTest(AgentId, Resource, BlockerId, StalledSeconds);
	}

	/** See UGroundTraffic::BeginCrossingForTest's own comment. */
	bool BeginCrossing(int32 AgentId, FRoadSegmentId RunwaySeed)
	{
		return Traffic.BeginCrossingForTest(AgentId, RunwaySeed);
	}

	/** See UGroundTraffic::SetGoalForTest's own comment. */
	bool SetGoal(int32 AgentId, FGuidelineNodeId Goal) { return Traffic.SetGoalForTest(AgentId, Goal); }

	/**
	 * ReplanAt with no banned node, from outside the resolver. What the deadlock resolver
	 * and OnGraphRebuilt call; exposed so a test can pin what a replan's SEARCH is allowed
	 * to use - a free runway end, a held one - without staging the two-aircraft cycle that
	 * would otherwise be the only way to make the resolver replan on demand. Forwards to the
	 * already-public UGroundTraffic::ReplanAt(AgentId, ...) - that overload has no production
	 * caller of its own either (production replans through FPlanReResolver directly, by
	 * reference, not by agent id), but this task's scope is the three named ForTest members;
	 * narrowing ReplanAt itself is a separate change.
	 */
	bool ReplanAt(int32 AgentId, const URoadNetwork& Network, int32 SpliceStep, FGuidelineEdgeId BannedEdge)
	{
		return Traffic.ReplanAt(AgentId, Network, SpliceStep, BannedEdge, FGuidelineNodeId());
	}

private:
	UGroundTraffic& Traffic;
};
