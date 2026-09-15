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

	/**
	 * 620, which is fueltruck1's own length: 6.200 m, the one dimension that model's README
	 * fixes exactly. It was 500, chosen before there was a truck to measure.
	 *
	 * IT HAS TO MATCH THE MESH, because UAirsideContent::VehicleMesh says the placeholder box
	 * is sized from this figure so that "what is on screen is the length the arbiter actually
	 * keeps clear". A 6.2 m truck reserving 5 m is that promise broken in the direction that
	 * hurts: the arbiter would let a second agent into road this one is occupying.
	 */
	UPROPERTY(EditAnywhere) double VehicleFootprint = 620.0;

	/** Clear line kept ahead of the nose, beyond the braking distance, uu. */
	UPROPERTY(EditAnywhere) double AircraftGap = 1500.0;
	UPROPERTY(EditAnywhere) double VehicleGap = 300.0;

	/**
	 * How fast a push off a stand runs, uu/s. 1 uu is 1 cm - see UAircraftType::MainWheelRadius.
	 *
	 * ON THE RULES AND NOT THE AIRFRAME, unlike the braking figure the claim window reads:
	 * push speed is a property of what is doing the PUSHING, so a hand tug is slower than a
	 * tug vehicle whatever it has on the bar. That is also why they are named for the tug
	 * rather than for the aeroplane. Slice 2 moves the two tug figures onto the depot's own
	 * vehicle types and leaves SelfManoeuvre here, where it belongs.
	 *
	 * FIGURES, NOT MEASUREMENTS. Nothing about a real tug is modelled yet; these exist so the
	 * manoeuvre reads at the right pace on screen, and they are EditAnywhere so it can be
	 * tuned against what the player actually sees rather than against a specification.
	 */
	UPROPERTY(EditAnywhere) double SelfManoeuvrePushSpeed = 200.0;  // 2.0 m/s, on the engine
	UPROPERTY(EditAnywhere) double HandTugPushSpeed       = 80.0;   // 0.8 m/s, walking pace
	UPROPERTY(EditAnywhere) double VehicleTugPushSpeed    = 150.0;  // 1.5 m/s

	/** Into and out of a push, uu/s^2. Gentle: a towbar does not snatch. */
	UPROPERTY(EditAnywhere) double PushAccel              = 30.0;   // 0.3 m/s^2

	/**
	 * Within this of the parked heading, the way out is forward and no push is needed, degrees.
	 *
	 * A MEASUREMENT OF THE GROUND AHEAD, not a property of the stand: it answers a
	 * taxi-through stand, a taxiway a player happened to draw past a stand, and a graph
	 * rebuilt since the aeroplane parked, all with one question. Nothing in Model/ reads
	 * UEntityDefinition::bTaxiThrough for this, and that is deliberate.
	 */
	UPROPERTY(EditAnywhere) double StraightOutDegrees     = 45.0;

	/**
	 * Fraction of MaxRPM a SelfManoeuvre airframe needs before it will move, 0..1.
	 *
	 * A POWERBACK IS THE ENGINE DOING THE WORK, so it cannot begin until there is thrust. An
	 * aeroplane on a tug bar moves from the first frame whatever its propeller is doing,
	 * because the tug supplies the force - which is the ONE place in this slice where the
	 * pushback need changes what happens, and it is justified because it is a fact about the
	 * aeroplane rather than about a tug that does not exist yet.
	 *
	 * HERE AND NOT ON FEnginePerformance, which is per-type authored content: this is a rule
	 * about when a manoeuvre may begin, not a fact about any engine, and putting it there
	 * would mean re-authoring every aircraft asset to carry a number none of them vary.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	double PowerbackRPMFraction = 0.6;

	/** Weight on held length in the routing cost. See FRouteQuery::CongestionWeight. */
	UPROPERTY(EditAnywhere) double CongestionWeight = 2.0;

	/** Stopped-and-waiting this long before deadlock detection looks. A normal junction
	 *  wait must never trip it. */
	UPROPERTY(EditAnywhere) double StallSeconds = 3.0;

	/** An unresolvable waiter re-tries its replan this often, sim seconds. */
	UPROPERTY(EditAnywhere) double RetrySeconds = 5.0;

	/** After a graph rebuild, how near a live node must be to a step's end to be it. */
	UPROPERTY(EditAnywhere) double ResolveRadius = 25.0;

	/**
	 * The longest step the model will take in one go, in sim seconds.
	 *
	 * THE FRAME IS NOT THE STEP. UAirsideTraffic hands this the frame time multiplied by the
	 * player's speed, so at x8 a 16 ms frame arrives as 133 ms of simulation - and an
	 * aeroplane crossing 133 ms of ground in one jump can pass the waypoint it was turning
	 * onto and be pulled back onto the line next frame. That is the rubber-banding reported
	 * from play, and it appeared at x2 and got worse from there while x1 looked perfect,
	 * which is exactly the signature of a step that scales with the multiplier.
	 *
	 * Substepping costs arbitration and motion passes in proportion to the speed multiplier,
	 * which is the right place to spend: the player asked for more simulation per second.
	 *
	 * MOVED HERE FROM UGroundTraffic (#107 item 6): a UPROPERTY(EditAnywhere) on that
	 * Transient, non-instanced UObject never reached the Details panel - the exact trap
	 * RoadNetworkActor.h documents for Presenter/Facade/Traffic, one layer further down
	 * (UGroundTraffic itself is a subobject of a subobject, neither exposed EditAnywhere).
	 * FTrafficRules already IS the level-authored knob (ARoadNetworkActor::TrafficRules,
	 * copied into the model every tick by UAirsideTraffic::Advance - see its header), so
	 * living here instead makes both fields reachable for free.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "0.001")) double MaxSubstepSeconds = 1.0 / 30.0;

	/**
	 * The most substeps one call will take, whatever the delta.
	 *
	 * A CEILING RATHER THAN A PROMISE. A frame that hitches badly - a level loading, a
	 * breakpoint - would otherwise ask for hundreds of steps and hitch the next frame too,
	 * which is the spiral that turns one stutter into a freeze. Past this the delta is
	 * divided evenly (see Advance), so every step is longer than MaxSubstepSeconds: slightly
	 * wrong every step beats compounding.
	 *
	 * SIZED FROM THE SPEED LADDER (#107 item 4), not merely for a hitch: USimClock's ladder
	 * (AirportOps/SimClock.h) reaches X32, and UAirsideTraffic::Advance's caller hands it the
	 * real frame time TIMES that multiplier every frame, hitch or not. X32 at 30 fps - the
	 * slowest rate this project treats as ordinary play - is 1.067 s of sim time needing 32
	 * steps to hold MaxSubstepSeconds; the OLD default of 8 clamped that to 8 steps of 133 ms
	 * each, four times the documented target, on EVERY frame at that speed - the same
	 * rubber-banding this substep split exists to remove, just moved to a higher speed
	 * setting instead of fixed. 32 keeps a genuine hitch exactly as bounded as before; it
	 * only stops ordinary top-speed play from being treated as one.
	 */
	UPROPERTY(EditAnywhere, meta = (ClampMin = "1")) int32 MaxSubsteps = 32;

	double FootprintFor(ETraversalClass Class) const;
	double GapFor(ETraversalClass Class) const;

	/** Which of the three push speeds above applies. The ONE consumer that has to agree with
	 *  EPushbackNeed - see its body for why that matters. */
	double PushSpeedFor(EPushbackNeed Need) const;
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
 * The replan MECHANISM shared by the deadlock resolver and a graph rebuild - spec §4 and
 * §6. Issue #84 pulled it off UGroundTraffic, which used to own ReplanAt, ReResolvePlan and
 * SpliceReplan directly: "HOW, NEVER WHEN" was already the split UGroundTraffic::ReplanAt's
 * own header drew between the resolver deciding an agent is stuck and this deciding what to
 * do about it, so making it a struct of its own says the same thing in the type system.
 *
 * NO PERSISTENT STATE. Every method takes what it needs (Rules for the congestion weight
 * and resolve radius, Occupancy for the table) and leaves nothing behind between calls, so
 * UGroundTraffic's own instance and one a test constructs fresh behave identically.
 */
struct AIRSIDE_API FPlanReResolver
{
	/**
	 * Re-routes Agent from SpliceStep onward, forbidding BannedEdge/BannedNode. Implements
	 * the full contract UGroundTraffic::ReplanAt documents - guards, PRECONDITION, the two
	 * rejected alternatives - which stays there as the PUBLIC description callers read;
	 * that function now forwards to this one.
	 */
	bool ReplanAt(FRoadAgent& Agent, const URoadNetwork& Network, int32 SpliceStep,
		FGuidelineEdgeId BannedEdge, FGuidelineNodeId BannedNode, const FTrafficRules& Rules,
		FTrafficOccupancy& Occupancy);

	/** What one re-resolution did. Counted by UGroundTraffic::OnGraphRebuilt for its one
	 *  log line. */
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
	 * Re-points Plan's steps from FromStep onward at the rebuilt graph. See
	 * UGroundTraffic::OnGraphRebuilt for the outer contract (three outcomes, the fourth
	 * that is not a degree of them, why guideline claims go and surface claims stay).
	 *
	 * FromStep IS THE FIRST STEP THE AGENT HAS NOT FINISHED, and the steps behind it keep
	 * their dead handles - with ONE exception that is not optional. The node the current step
	 * LEAVES FROM is Steps[FromStep-1].To (or Plan.Start at step 0), it is what StepFromNode
	 * answers, and FOUR live readers ask for it every tick:
	 *
	 *   - FClaimPass::Run's crossing arm, which asks whether that node carries a
	 *     HoldingPositionFor bar. A dead handle reads as no bar, so no crossing would ever
	 *     arm again after a rebuild;
	 *   - FClaimPass::Run's tail-node claim, OfNode(From), held while the body is still
	 *     within Footprint/2 of it. On a dead handle that claim protects nothing and the
	 *     junction BEHIND the agent is open for somebody to drive into;
	 *   - FClaimPass::RankAt, which falls back to the class order when the node cannot be
	 *     found - so a node's PriorityOverride would silently stop applying;
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
	EReResolve ReResolvePlan(FRoadAgent& Agent, FRoutePlan& Plan, int32 FromStep,
		const URoadNetwork& Network, const FTrafficRules& Rules, FTrafficOccupancy& Occupancy);

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
	static bool SpliceReplan(const URoadNetwork& Network, const FRouteQuery& Query, int32 KeepSteps,
		FRoutePlan& Plan);
};

/**
 * The wait-for graph, its cycles, and one replan per cycle per retry window - spec §5.
 * Issue #84 pulled this off UGroundTraffic, whose nine ForTest doors existed because these
 * six fields - CyclesSeen through DeadlockLogLines, all TEST-FACING bookkeeping, see their
 * own comments - lived as private members of the god class. They are public fields of a
 * plain struct now: still forwarded from UGroundTraffic under their old ForTest names (so
 * nothing that already reads them has to change), but also reachable directly by a test
 * that constructs an FDeadlockResolver of its own and drives it with no UGroundTraffic at all.
 */
struct AIRSIDE_API FDeadlockResolver
{
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
	void Resolve(TArray<FRoadAgent>& Agents, const URoadNetwork& Network, const FTrafficRules& Rules,
		FTrafficOccupancy& Occupancy, FNodeReachCache& Reach, FPlanReResolver& PlanReResolver,
		double SimSeconds);

	/**
	 * Every cycle key (the lowest member id) this session has LOGGED. Its only reader is
	 * UGroundTraffic::GetCyclesDetectedForTest.
	 *
	 * NOT reflected: it is test-facing bookkeeping about log lines, not state the simulation
	 * reads - nothing in the tick branches on it, and an agent list that never reaches disk
	 * cannot leave a meaningful key behind for a later session. Reflecting it would say it
	 * mattered to the model, which it does not.
	 */
	TSet<int32> CyclesSeen;

	/** Last agent whose deadlock replan succeeded; 0 until one does. Test-facing, as above. */
	int32 LastResolvedAgent = 0;

	/**
	 * When each cycle key last settled by a YIELD (SimSeconds). Read by Resolve, unlike the
	 * sets above: a cycle that re-forms within Rules.RetrySeconds of yielding is one a yield
	 * did not fix - the other member wanted something a third party holds - and goes to the
	 * replan path instead of yielding for ever. Test-facing for the same reason as CyclesSeen:
	 * agents never reach disk, so a key could not mean anything to a later session.
	 */
	TMap<int32, double> YieldedAt;

	/** Reservation cycles settled by a yield; and who yielded last. Test-facing. */
	int32 Yields = 0;
	int32 LastYieldedAgent = 0;

	/** Deadlock lines emitted, resolved and unresolvable alike. Test-facing, as above. */
	int32 DeadlockLogLines = 0;

private:
	/**
	 * Can this member of a cycle turn where it stands? Spec §5's refined resolver rule.
	 *
	 * True only for a Taxiing agent that is STOPPED, was refused something (BlockedStep), and
	 * is AT the node that step leaves from - within Gap + Footprint/2 short of it and not
	 * past it. The alternative to a banned edge is another edge OUT of that node, so an agent
	 * that has already entered the edge cannot take it without reversing, and one still a
	 * whole edge short of the node would be replanned from a node it is nowhere near.
	 */
	bool CanReplanAtBlockedStep(const FRoadAgent& Agent, const URoadNetwork* Network,
		const FTrafficRules& Rules, FNodeReachCache& Reach) const;
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

	/** See UGroundTraffic::BeginCrossingForTest's own comment. */
	bool BeginCrossing(int32 AgentId, FRoadSegmentId RunwaySeed)
	{
		return Traffic.BeginCrossingForTest(AgentId, RunwaySeed);
	}

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
