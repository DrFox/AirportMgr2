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
	 * purpose (controller ruling 5, 2026-09-25) to pin a builder defect rather than lay round
	 * it: the derived turn at such a node is a lane-offset jog with MinRadius 0 (unmeasured, so
	 * not gated) that FSpeedProfile reports as a sharp vertex and crawls. Not one of the 3 x 5.
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
	/** Where the goal lane end was, and where the vehicle's chassis origin stopped (Driven only). */
	FVector2D GoalPosition = FVector2D::ZeroVector;
	FVector2D EndPosition = FVector2D::ZeroVector;
	/**
	 * Plan length minus the distance travelled along it, uu, at arrival. THE ARRIVAL MEASURE:
	 * the follower walks the STEERED axle along the line, so the chassis origin (the fixed
	 * axle) stops a wheelbase short of the lane end - measured 370 uu for the rig, 149 for the
	 * utility on 2026-09-25 - and a position check would test the wheelbase, not the arrival.
	 */
	double DistanceLeft = 0.0;
	/** True if the agent was ever seen Reversing on this leg: the course has no reverse legs. */
	bool bReversed = false;
	/** The agent that drove it, or 0. Persistent: the same id drives leg after leg. */
	int32 AgentId = 0;
	/** FSpeedProfile::GetSharpVertexCount over the leg's plan: instantaneous heading changes. */
	int32 SharpVertexCount = 0;
	double SharpestDegrees = 0.0;
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

	/** The stop it is at, or leaving, counted in its own order from 0 (the loop's start). */
	int32 Position = 0;

	/**
	 * The stop it is driving to. Position + 1, unless the next leg was refused and it was routed
	 * on past it; never beyond the loop's end, so a loop's results are all its own.
	 */
	int32 Target = 0;

	/** Its agent, or 0 before the first dispatch and between a no-hang exit and the next. */
	int32 AgentId = 0;
	double Elapsed = 0.0;
	double Timeout = 0.0;
	bool bReversed = false;

	int32 LoopsCompleted = 0;

	/** Agents spawned for it: 1 while it drives continuously, plus one per no-hang exit. */
	int32 Dispatches = 0;

	/** This loop's results and the last completed loop's, by FORWARD leg index in both directions. */
	TArray<FRigLegResult> LoopResults;
	TArray<FRigLegResult> LastLoopResults;
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
 * THE DRIVER (spec §3, REVISED 2026-09-25 "continuous"): both vehicles are out AT ONCE, each
 * with ONE persistent agent. The rig runs the waypoints forwards; the utility runs them in
 * reverse, starting UtilityStartDelay later. At each waypoint the arrived agent is REDIRECTED
 * onto its next leg rather than retired and respawned, so its tow chain carries on from where
 * it is instead of being re-laid straight. A refused leg is skipped by routing on to the next
 * waypoint the vehicle CAN reach from where it stands; a vehicle that can reach none
 * (stranded), jack-knifes or sticks is retired and dispatched fresh from its next waypoint.
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
	 * hairpin the rig folds on, which no route search would ever hand out.
	 */
	TFunction<bool(int32 Leg, int32 Slot, FRoutePlan& OutPlan)> PlanOverrideForTest;

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
	 * Sends Runner on from its current Position: redirects its agent onto the next leg it can
	 * reach, or dispatches a fresh one when it has none. Records every leg it passes over.
	 */
	void SendOn(FRigCourseRunner& Runner);

	/** Plans the road between two stops with Slot's vehicle; false with Reason when it cannot. */
	bool PlanBetween(const FRigCourseWaypoint& From, const FRigCourseWaypoint& To, int32 Slot,
		FRoutePlan& OutPlan, FString& OutReason) const;

	/** Plans Runner's own leg at Position (the test override first); false with Reason when refused. */
	bool PlanOwnLeg(const FRigCourseRunner& Runner, int32 Position, FRoutePlan& OutPlan, FString& OutReason) const;

	/** Records and logs Runner's leg at Position as refused, and labels it on the road. */
	void RecordRefusal(FRigCourseRunner& Runner, int32 Position, const FString& Reason);

	/** Ends Runner's drive with Outcome: retires its agent (if any) and moves it on one stop. */
	void AbandonDrive(FRigCourseRunner& Runner, ERigLegOutcome Outcome, const FString& Reason = FString());

	/** Moves Runner's Position to Stop, ending the loop when Stop is the loop's end. */
	void ArriveAt(FRigCourseRunner& Runner, int32 Stop);
	void EndLoop(FRigCourseRunner& Runner);

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
