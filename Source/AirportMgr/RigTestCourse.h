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
	/** The agent that drove it, or 0 - retired by the time the result is read. */
	int32 AgentId = 0;
	/** FSpeedProfile::GetSharpVertexCount over the leg's plan: instantaneous heading changes. */
	int32 SharpVertexCount = 0;
	double SharpestDegrees = 0.0;
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
 * THE DRIVER, per leg and per vehicle, one vehicle out at a time: plan the leg with the
 * vehicle's FVehicle (route search gates on VehicleFit), dispatch a fresh agent, and on
 * Parked retire it and take the next. Each leg is driven by the rig, then by the utility +
 * trailer, so both vehicles cover the whole course every loop and a refusal is per vehicle.
 * A refused leg is SKIPPED, not stopped on; a jack-knifed or stuck agent is retired and
 * skipped. The loop can never wait on an arrival that cannot come.
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
	 * Lays the course on Target. Resets THIS ACTOR's own bookkeeping (Waypoints, loop
	 * results, refusal labels, the leg/slot/agent counters) and re-resolves Vehicles - it
	 * does NOT remove any road geometry a previous call already placed on Target, so calling
	 * it twice on the same Target lays a second course on top of the first.
	 */
	void BuildCourse(IRoadEditTarget& Target);

	/** BuildCourse, named for the test that calls it without a BeginPlay. */
	void BuildCourseForTest(IRoadEditTarget& Target) { BuildCourse(Target); }

	/** Legs per loop per vehicle: one per waypoint, the last wrapping to the first. */
	int32 LegCountForTest() const { return Waypoints.Num(); }

	/** Waypoints in visiting order. */
	const TArray<FRigCourseWaypoint>& GetWaypoints() const { return Waypoints; }

	/** The vehicles, in the order they take each leg. */
	const TArray<FVehicle>& GetVehicles() const { return Vehicles; }

	/**
	 * The lane end a waypoint names in Network's CURRENT guideline graph, or unset. Resolved
	 * per leg, never cached: every rebuild reallocates guideline nodes.
	 */
	static FGuidelineNodeId ResolveWaypoint(const URoadNetwork& Network, const FRigCourseWaypoint& Waypoint);

	/** Distinct (tier, feature) pairs laid, connectors excluded: 3 tiers x 5 features. */
	int32 FeatureCountForTest() const;

	/** Completed loops. */
	int32 LoopsCompletedForTest() const { return LoopsCompleted; }

	/** The last COMPLETED loop's results, [Leg * Vehicles.Num() + Vehicle]. */
	const TArray<FRigLegResult>& LastLoopResultsForTest() const { return LastLoopResults; }

	/**
	 * A leg's allowance, seconds: Factor times the time to drive Length at the cap speed from
	 * rest to rest. Past it the leg is logged stuck and skipped.
	 */
	static double LegTimeoutSeconds(const FVehicle& Vehicle, double Length, double Factor);

	/** The stuck exit's test: a tiny factor makes every leg time out. */
	void SetLegTimeoutFactorForTest(double Factor) { LegTimeoutFactor = Factor; }

	/**
	 * Consulted in StartAttempt BEFORE route search: return true having filled the plan to
	 * drive that (leg, vehicle slot) on it instead. The jack-knife exit's test feeds a hairpin
	 * the rig folds on, which no route search would ever hand out.
	 */
	TFunction<bool(int32 Leg, int32 Slot, FRoutePlan& OutPlan)> PlanOverrideForTest;

	/** The agent out now (0 when none), and the leg and vehicle slot it is on. */
	int32 GetActiveAgentIdForTest() const { return ActiveAgentId; }
	int32 GetActiveLegForTest() const { return Leg; }
	int32 GetActiveSlotForTest() const { return VehicleSlot; }

	/** ConnectNodes calls refused while the course was laid. */
	int32 GetRefusedConnectsForTest() const { return RefusedConnects; }

private:
	void TickDriver(double DeltaSeconds);
	void StartAttempt();
	void FinishAttempt(ERigLegOutcome Outcome, const FString& Reason = FString());
	void EndLoop();
	void DrawRefusals() const;
	FString DescribeRefusal(const URoadNetwork& Network, const struct FRoutePlan& Plan, const FVehicle& Vehicle) const;
	ARoadNetworkActor* ResolveNetworkActor();

	/** Found at BeginPlay, or lazily by the first Tick (the headless test has no BeginPlay). */
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> NetworkActor;

	UPROPERTY(Transient) TArray<FRigCourseWaypoint> Waypoints;

	/** The rig, then the utility + trailer. Resolved in BuildCourse, never in the constructor (the CDO). */
	UPROPERTY(Transient) TArray<FVehicle> Vehicles;

	/** Log names, one per Vehicles entry. */
	TArray<FString> VehicleNames;

	int32 Leg = 0;
	int32 VehicleSlot = 0;
	int32 LoopsCompleted = 0;

	/** The agent out now, or 0. */
	int32 ActiveAgentId = 0;
	double ActiveElapsed = 0.0;
	double ActiveTimeout = 0.0;
	FVector2D ActiveGoal = FVector2D::ZeroVector;
	bool bActiveReversed = false;

	/** This loop's results, and the last completed loop's. */
	TArray<FRigLegResult> LoopResults;
	TArray<FRigLegResult> LastLoopResults;

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

	/** Where each refused (leg, vehicle) starts, for the red label - kept until it drives. */
	TMap<int32, TPair<FVector, FString>> RefusalLabels;
};
