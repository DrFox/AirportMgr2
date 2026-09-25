#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "RigTestCourse.generated.h"

class ARoadNetworkActor;
class IRoadEditTarget;
class URoadNetwork;
struct FRoadAgent;

/**
 * What a course leg exercises. Plain, not a UENUM: it is carried on FRigCourseWaypoint, whose
 * reflected fields do not include it (CLAUDE.md on plain enums and UHT) - it is a test fact,
 * never saved.
 */
enum class ERigCourseFeature : uint8
{
	/** A join between tiers: Wide in the middle, the lane's own tier at each end. Not counted. */
	Connector,
	Straight,
	Right90,
	Left90,
	/** The T junction, into its stem or back out of it - the stem is driven both ways. */
	TeeJunction,
	/** The dead end's derived U-turn balloon. */
	DeadEnd,
	/**
	 * A width change on a STRAIGHT, at a degree-2 node: Narrow -> Wide mid-straight. Kept on
	 * purpose (controller ruling 5, 2026-09-25): laid to pin a builder defect - the derived turn
	 * was a lane-offset jog with MinRadius 0 that FSpeedProfile crawled - and kept now the
	 * builder tapers it on an S, to hold that fix (no sharp vertex). Not one of the 3 x 5.
	 */
	WidthStep
};

/**
 * One place the course sends a vehicle to: a road node, arrived at ALONG the segment from
 * From. A node alone is not enough - on a two-way road every node has a lane end arriving from
 * each side, and the T node is visited twice, once from each arm.
 */
USTRUCT()
struct FRigCourseWaypoint
{
	GENERATED_BODY()

	UPROPERTY() FRoadNodeId Node;
	UPROPERTY() FRoadNodeId From;

	/**
	 * The node the course LEAVES Node towards, on the leg that starts here: the first node of
	 * that leg's road after Node. (Node, Next) is this waypoint arrived at the OTHER way, which
	 * is what the vehicle running the course in reverse arrives along - From alone cannot give
	 * it, because a multi-segment leg's next node is not the next waypoint's node.
	 */
	UPROPERTY() FRoadNodeId Next;

	/** The leg that ENDS here, as the log names it: "Standard, right 90". */
	UPROPERTY() FString Label;

	/**
	 * 0 Narrow, 1 Standard, 2 Wide - the service-road width index the lane was laid at.
	 * INDEX_NONE for the return road's waypoints (the width step and the one before it), which
	 * belong to no single tier.
	 */
	UPROPERTY() int32 Tier = 0;

	ERigCourseFeature Feature = ERigCourseFeature::Connector;
};

/** How one vehicle's attempt at one leg ended, in the loop just finished. */
enum class ERigLegOutcome : uint8
{
	NotRun,
	Driven,
	Refused,
	/**
	 * Fits on its own, but the vehicle was routed PAST it: an earlier leg was refused and the
	 * vehicle drove on to a later waypoint it could reach, through this leg's road or round it.
	 */
	Bypassed,
	Jackknifed,
	Stuck
};

struct FRigLegResult
{
	ERigLegOutcome Outcome = ERigLegOutcome::NotRun;
	/** Refusal text, as logged. Empty unless Refused. */
	FString Reason;
	/** Where the goal lane end was, and where the vehicle's chassis origin was as it passed it (Driven only). */
	FVector2D GoalPosition = FVector2D::ZeroVector;
	FVector2D EndPosition = FVector2D::ZeroVector;
	/**
	 * The leg's end marker minus the distance travelled along the route, uu, on the tick the
	 * marker was passed - zero or a tick's travel BELOW zero, since the vehicle drives on
	 * through it. THE ARRIVAL MEASURE: the follower walks the STEERED axle along the line, so
	 * the chassis origin (the fixed axle) is a wheelbase behind the lane end - 370 uu for the
	 * rig, 149 for the utility (2026-09-25) - and a position check would test the wheelbase.
	 */
	double DistanceLeft = 0.0;
	/**
	 * WHERE, NOT HOW FAR (2026-09-25): the steered axle as the marker was passed, and the live
	 * route's own point at the marker's distance. DistanceLeft alone cannot see a route that
	 * no longer goes where the marker was planned - a rebuild's replan to the loop's end cut
	 * three dead ends out of the utility's route while every marker still "arrived" by distance.
	 * The test holds both to the waypoint's lane end.
	 */
	FVector2D SteeredPosition = FVector2D::ZeroVector;
	FVector2D RoutePosition = FVector2D::ZeroVector;
	/** Seconds from the previous marker (or the dispatch) to this one, and the speed it passed at, uu/s. */
	double Elapsed = 0.0;
	double PassSpeed = 0.0;
	/** True if the agent was ever seen Reversing on this leg: the course has no reverse legs. */
	bool bReversed = false;
	/** The agent that drove it, or 0. Persistent: one agent drives the whole route, loop after loop. */
	int32 AgentId = 0;
	/** FSpeedProfile::GetSharpVertexCount over the leg's plan: instantaneous heading changes. */
	int32 SharpVertexCount = 0;
	double SharpestDegrees = 0.0;
};

/**
 * One leg's end on a vehicle's live route - a PROGRESS MARKER, not a stop (spec §3 REVISED
 * "one route per loop"). The loop's legs are joined into one route and driven without stopping;
 * a leg is arrived when the agent's distance along that route passes EndDistance.
 */
struct FRigLegMarker
{
	/** The loop (1-based) and the runner's stops this marker covers: Position -> Target. */
	int32 Loop = 1;
	int32 Position = 0;
	int32 Target = 0;
	/** Along the agent's live plan, uu, and this leg's own length (for its timeout). */
	double EndDistance = 0.0;
	double Length = 0.0;
};

/**
 * One vehicle's run of the course: its own agent, its own direction, its own place and its
 * own loop. Plain, not a USTRUCT: nothing in it is a UObject and nothing saves it.
 */
struct FRigCourseRunner
{
	/** Index into ARigTestCourse's Vehicles and VehicleNames. */
	int32 Slot = 0;

	/** Runs the waypoints last to first, arriving at each along (Node, Next). */
	bool bReverse = false;

	/** Seconds still to wait before the first dispatch. */
	double StartDelay = 0.0;

	/** The stop it last passed, counted in its own order from 0 (the loop's start). */
	int32 Position = 0;

	/**
	 * The stop it is driving to: the next marker's. Position + 1, unless the next leg was
	 * refused and it was routed on past it; never beyond the loop's end.
	 */
	int32 Target = 0;

	/** Its agent, or 0 before the first dispatch and between a no-hang exit and the next. */
	int32 AgentId = 0;
	/** Seconds since the last marker (or the dispatch), and the next marker's allowance. */
	double Elapsed = 0.0;
	double Timeout = 0.0;
	bool bReversed = false;

	int32 LoopsCompleted = 0;

	/** Agents spawned for it: 1 while it drives continuously, plus one per no-hang exit. */
	int32 Dispatches = 0;

	/** Loop routes spliced onto the live one before it ran out (UAirsideTraffic::ExtendRoute). */
	int32 Extensions = 0;

	/** Continuations that had to restart from rest (RedirectAgent): the route ran out first. */
	int32 Redirects = 0;

	/** The legs a join with a sharp vertex AT the weld led into (FSpeedProfile), in order. */
	TArray<int32> SharpJoinLegs;

	/** The leg ends still ahead on the live route, in order. See FRigLegMarker. */
	TArray<FRigLegMarker> Markers;

	/** Where the live route ends: the loop and the stop. A stop of N is that loop's end. */
	int32 RouteEndLoop = 1;
	int32 RouteEndStop = 0;

	/**
	 * The live route's length as the course last made it (dispatch, extension or redirect), uu.
	 * Anything else changing it - a rebuild's replan, a deadlock replan - moves the route under
	 * markers that are distances along it, so TickRunner warns when it differs.
	 */
	double RouteLength = 0.0;

	/** Set once an extension was tried for the current route end and could not be made, so it is not re-planned every tick. */
	bool bExtendFailed = false;

	/** Results per loop number (1-based), filled as legs are planned and passed; a loop's move to LastLoopResults at its end. */
	TMap<int32, TArray<FRigLegResult>> ResultsByLoop;
	TArray<FRigLegResult> LastLoopResults;

	/**
	 * (loop, leg) pairs already logged refused, bypassed or stranded: once per loop, however often
	 * a no-hang exit or a retried extension re-plans the rest of it. A loop's keys go when it ends.
	 */
	TSet<int64> Logged;
};

/**
 * A pre-laid road course that loops the articulated rig and the utility + fuel trailer over
 * every road feature at every service-road width tier, and says which legs each cannot fit
 * (spec 2026-09-24 §3, §4). Dev tooling, in the game module beside the road-build driver.
 *
 * THE COURSE IS CODE, NOT LEVEL DATA: laid at BeginPlay through IRoadEditTarget, so the same
 * course runs headless in AirportMgr.RigCourse.OneLoopHeadless, and none of the silent-success
 * traps of editing a .umap headlessly apply.
 *
 * THE DRIVER (spec §3, REVISED 2026-09-25 "continuous" and "one route per loop"): both
 * vehicles are out AT ONCE, each with ONE persistent agent, and each drives ONE ROUTE PER LOOP,
 * as a job in the game drives one plan to a real destination: every leg is planned for its
 * verdict, the admissible ones are joined with RouteSearch::Splice, and waypoints are progress
 * markers along that route, never stops. Before the route runs out the next loop is spliced
 * onto it (UAirsideTraffic::ExtendRoute), so the loop boundary is not a stop either. The rig
 * runs the waypoints forwards; the utility runs them in reverse, starting UtilityStartDelay
 * later. A refused leg is skipped by routing on to the next
 * waypoint the vehicle CAN reach from where it stands, and a leg into a stop with nothing
 * onward (a dead end it cannot turn in) is bypassed the same way before it is driven; a
 * vehicle that can reach nothing (stranded, the fallback), jack-knifes or sticks is retired
 * and dispatched fresh from its next waypoint.
 * The loop can never wait on an arrival that cannot come.
 */
UCLASS()
class ARigTestCourse : public AActor
{
	GENERATED_BODY()

public:
	ARigTestCourse();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * Lays the course on Target. Resets THIS ACTOR's own bookkeeping (Waypoints, runners, loop
	 * results, refusal labels) and re-resolves Vehicles - it does NOT remove any road geometry a
	 * previous call already placed on Target, nor retire agents a previous run left out, so
	 * calling it twice on the same Target lays a second course on top of the first.
	 */
	void BuildCourse(IRoadEditTarget& Target);

	/** BuildCourse, named for the test that calls it without a BeginPlay. */
	void BuildCourseForTest(IRoadEditTarget& Target) { BuildCourse(Target); }

	/** Legs per loop per vehicle: one per waypoint, the last wrapping to the first. */
	int32 LegCountForTest() const { return Waypoints.Num(); }

	/** Waypoints in the FORWARD visiting order. */
	const TArray<FRigCourseWaypoint>& GetWaypoints() const { return Waypoints; }

	/** The vehicles: slot 0 the rig (forwards), slot 1 the utility + trailer (in reverse). */
	const TArray<FVehicle>& GetVehicles() const { return Vehicles; }

	/**
	 * The lane end a waypoint names in Network's CURRENT guideline graph, or unset. Resolved
	 * per leg, never cached: every rebuild reallocates guideline nodes.
	 */
	static FGuidelineNodeId ResolveWaypoint(const URoadNetwork& Network, const FRigCourseWaypoint& Waypoint);

	/**
	 * The waypoint a runner stops at at Position in its own order: Waypoints[Position] forwards;
	 * in reverse Waypoints[(N - Position) % N] arrived along (Node, Next), so leg Position runs
	 * the node pair of forward leg N - 1 - Position the other way.
	 */
	FRigCourseWaypoint StopAt(bool bReverse, int32 Position) const;

	/** The forward leg index a runner's leg at Position drives, in either direction. */
	int32 LegAt(bool bReverse, int32 Position) const;

	/** Distinct (tier, feature) pairs laid, connectors excluded: 3 tiers x 5 features. */
	int32 FeatureCountForTest() const;

	/** Loops completed by the vehicle in Slot. */
	int32 LoopsCompletedForTest(int32 Slot) const { return Runners.IsValidIndex(Slot) ? Runners[Slot].LoopsCompleted : 0; }

	/** Loops completed by EVERY vehicle: the fewest. */
	int32 LoopsCompletedByAllForTest() const;

	/** Slot's last COMPLETED loop's results, by forward leg index. */
	const TArray<FRigLegResult>& LastLoopResultsForTest(int32 Slot) const { return Runners[Slot].LastLoopResults; }

	/** Slot's runner, for the test to read its agent, position and dispatch count. */
	const FRigCourseRunner& GetRunnerForTest(int32 Slot) const { return Runners[Slot]; }

	/**
	 * A leg's allowance, seconds: Factor times the time to drive Length at the cap speed from
	 * rest to rest. Past it the leg is logged stuck and skipped.
	 */
	static double LegTimeoutSeconds(const FVehicle& Vehicle, double Length, double Factor);

	/** The stuck exit's test: a tiny factor makes every leg time out. */
	void SetLegTimeoutFactorForTest(double Factor) { LegTimeoutFactor = Factor; }

	/**
	 * Consulted BEFORE route search on every leg's own plan: return true having filled the plan
	 * to drive that (forward leg, vehicle slot) on it instead. The jack-knife exit's test feeds a
	 * hairpin the rig folds on, which no route search would ever hand out. bDispatching is true
	 * only for the call whose plan is then driven; the course also asks to JUDGE legs (look-ahead,
	 * legs routed past), and a one-shot override spent there would never be driven.
	 */
	TFunction<bool(int32 Leg, int32 Slot, bool bDispatching, FRoutePlan& OutPlan)> PlanOverrideForTest;

	/**
	 * The fallback's test: every extension is treated as refused, so each route runs out and the
	 * next is started from rest by RedirectAgent (ContinueRoute).
	 */
	bool bRefuseExtensionsForTest = false;

	/** ConnectNodes calls refused while the course was laid. */
	int32 GetRefusedConnectsForTest() const { return RefusedConnects; }

	/**
	 * How long the utility waits before its first dispatch, seconds. 20 s, WHY: both vehicles
	 * start at the same node (tier 0's entry, on opposite lanes), and the rig's first leg - the
	 * 80 m straight, ~12 s from rest at the bowser's figures - takes it clear of that junction
	 * before the utility's trailer is laid there; after that the two meet head-on somewhere
	 * mid-course and pass on opposite lanes, which is the traffic this change is to show.
	 * Not zero: two chains laid into one junction on the same tick is a start-up artefact,
	 * not a course feature.
	 */
	static constexpr double UtilityStartDelay = 20.0;

private:
	void TickRunner(FRigCourseRunner& Runner, double DeltaSeconds);

	/**
	 * Plans the stops from From to the end of Loop as ONE route - each leg judged on its own
	 * plan (look-ahead, bypass, refusal, as the legs always were), the admissible ones joined
	 * with RouteSearch::Splice. OutMarkers' EndDistance is along OutPlan. OutEndStop is where
	 * the route ends: N, or earlier when a leg does not join or the vehicle is stranded. False
	 * when not even the first leg could be planned. bHeadIsDispatch: the first leg is about to
	 * be driven by a fresh agent, so a test override may be spent on it (PlanOverrideForTest).
	 */
	bool PlanLoopRoute(FRigCourseRunner& Runner, int32 Loop, int32 From, bool bHeadIsDispatch,
		FRoutePlan& OutPlan, TArray<FRigLegMarker>& OutMarkers, int32& OutEndStop);

	/** Dispatches a fresh agent on a route from Runner's Position, or moves it on past a stop it is stranded at. */
	void DispatchFresh(FRigCourseRunner& Runner);

	/** Splices the next stretch onto the live route before it runs out; restarts from rest when it already has. */
	void ContinueRoute(FRigCourseRunner& Runner, const FRoadAgent& Agent, bool bFromRest);

	/** The key a (loop, leg or N + stop) once-per-loop log line is remembered under in Logged. */
	int64 LogKey(int32 Loop, int32 Slot) const;

	/** How much driven route to keep behind the vehicle when an extension trims it (the chain plus the probe's window). */
	static double HistoryToKeep(const FVehicle& Vehicle);

	/** Logs, and counts on Runner, a sharp vertex AT the weld between Head and Tail (per-leg profiles cannot see it). */
	void ReportSharpJoin(FRigCourseRunner& Runner, int32 Loop, const FRoutePlan& Head, const FRoutePlan& Tail, int32 IntoLeg);

	/** The results array for Loop, created on first use. */
	TArray<FRigLegResult>& ResultsFor(FRigCourseRunner& Runner, int32 Loop);

	/** Records a marker passed: the arrival log, the Driven result, and the loop's end when it is. */
	void PassMarker(FRigCourseRunner& Runner, const FRoadAgent& Agent, const FRigLegMarker& Marker);

	/** Plans the road between two stops with Slot's vehicle; false with Reason when it cannot. */
	bool PlanBetween(const FRigCourseWaypoint& From, const FRigCourseWaypoint& To, int32 Slot,
		FRoutePlan& OutPlan, FString& OutReason) const;

	/**
	 * Plans Runner's own leg at Position (the test override first); false with Reason when refused.
	 * bDispatching: the plan is about to be driven, not only judged - see PlanOverrideForTest.
	 */
	bool PlanOwnLeg(const FRigCourseRunner& Runner, int32 Position, FRoutePlan& OutPlan, FString& OutReason,
		bool bDispatching) const;

	/** True when some later stop can be reached from Stop: false marks a trap to route past. */
	bool HasOnward(const FRigCourseRunner& Runner, int32 Stop) const;

	/** Records and logs Runner's leg at Position of Loop as fitting but not driven. */
	void RecordBypass(FRigCourseRunner& Runner, int32 Loop, int32 Position, const TCHAR* Why);

	/** Records and logs Runner's leg at Position of Loop as refused, and labels it on the road. */
	void RecordRefusal(FRigCourseRunner& Runner, int32 Loop, int32 Position, const FString& Reason);

	/** Ends Runner's drive with Outcome: retires its agent and moves it on to the stop it was driving to. */
	void AbandonDrive(FRigCourseRunner& Runner, ERigLegOutcome Outcome, const FString& Reason = FString());

	/** Moves Runner's Position to Stop of its current loop, ending the loop when Stop is the loop's end. */
	void ArriveAt(FRigCourseRunner& Runner, int32 Stop);
	void EndLoop(FRigCourseRunner& Runner, int32 Loop);

	/** What the log calls Runner's leg at Position: the forward leg's label, marked when reversed. */
	FString LegLabel(const FRigCourseRunner& Runner, int32 Position) const;
	void DrawRefusals() const;
	FString DescribeRefusal(const URoadNetwork& Network, const struct FRoutePlan& Plan, int32 Slot) const;
	ARoadNetworkActor* ResolveNetworkActor();

	/** Found at BeginPlay, or lazily by the first Tick (the headless test has no BeginPlay). */
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> NetworkActor;

	UPROPERTY(Transient) TArray<FRigCourseWaypoint> Waypoints;

	/** The rig, then the utility + trailer. Resolved in BuildCourse, never in the constructor (the CDO). */
	UPROPERTY(Transient) TArray<FVehicle> Vehicles;

	/** Log names, one per Vehicles entry. */
	TArray<FString> VehicleNames;

	/** One per vehicle, in the same order. */
	TArray<FRigCourseRunner> Runners;

	/**
	 * THREE TIMES the ideal - generous on purpose: this is a hang detector, not a speed test,
	 * and corners slow a vehicle well below its cap. The ideal includes getting up to speed and
	 * stopping again (Cap/Accel + Cap/Decel), because on a 25 m leg that is most of the time
	 * and a bare Length/Cap would call a healthy truck stuck. Measured 2026-09-25: the slowest
	 * healthy leg used 0.33 of it. An INSTANCE member, not a constant, only so a test can
	 * shrink it and make the stuck exit fire; nothing else sets it.
	 * ENFORCED BY: AirportMgr.RigCourse.StuckIsSkipped (RigTestCourseTest.cpp), which calls
	 * SetLegTimeoutFactorForTest - the only setter - and goes red if a leg stops timing out.
	 */
	double LegTimeoutFactor = 3.0;

	int32 RefusedConnects = 0;
	bool bWarnedNoNetwork = false;

	/** Where each refused (vehicle, leg) starts, for the red label - kept until it drives. */
	TMap<int32, TPair<FVector, FString>> RefusalLabels;
};
