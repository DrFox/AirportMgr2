#pragma once

#include "CoreMinimal.h"
#include "Model/ReverseTurn.h"
#include "Model/RoadHandles.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"

class ARoadNetworkActor;
class IRoadEditTarget;
class URoadNetwork;

/** The yard's three reverse manoeuvres, in the order a runner visits them. */
enum class ERigYardFeature : uint8
{
	/** Pull past, stop in line, back straight in (option B, ruled 2026-09-24). */
	StraightBay,
	/** Pull past, back round a 90 degree fillet into a stub. */
	Bay90,
	/** Drive down a dead-end spur past a side stub, back into the stub, drive out the way it came. */
	Hammerhead,
	Count
};

/** One reverse, as the yard saw it. */
struct FRigYardReverse
{
	ERigYardFeature Feature = ERigYardFeature::StraightBay;
	/** Entered Reversing on the way to this bay. */
	bool bArmed = false;
	/** The solved reverse's worst hitch angle, degrees (from its samples). */
	double WorstHitchDegrees = 0.0;
	/** Where the trailer's rearmost axle came to rest against the bay end, uu, and its heading against the bay's, degrees. */
	double EndError = 0.0;
	double EndHeadingDegrees = 0.0;
};

/** One vehicle going round the yard: its own agent, its own next bay. Plain, like FRigCourseRunner. */
struct FRigYardRunner
{
	int32 Slot = 0;
	double StartDelay = 0.0;
	int32 AgentId = 0;
	ERigYardFeature Goal = ERigYardFeature::StraightBay;
	double Elapsed = 0.0;
	/** The live leg saw Reversing, and the reverse's figures once it did. */
	bool bSawReversing = false;
	double LegWorstHitchDegrees = 0.0;
	int32 Dispatches = 0;
	int32 Jackknifes = 0;
	int32 Refusals = 0;
	int32 Stuck = 0;
	TArray<FRigYardReverse> Completed;
	/** The chain last tick, and the worst jump of any axle beyond a tick's travel since - the handover check. */
	TArray<FVector2D> LastAxles;
	double WorstAxleJump = 0.0;
};

/**
 * THE REVERSING YARD on M_RigTest (spec 2026-09-26 §4): a separate island of Wide service road
 * south of the loop course, with a straight bay, a 90 degree bay and a hammerhead, and one runner
 * per tow going round the three for ever. Owned and ticked by ARigTestCourse, which lays it
 * beside the loop in BeginPlay.
 *
 * ITS OWN CLASS, NOT MORE OF ARigTestCourse: that actor is the loop's driver and was already
 * ~1250 lines on 2026-09-26; the yard shares its vehicles and its network actor and nothing else
 * - no waypoints, no markers, no splicing. A runner here plans ONE LEG AT A TIME, to a bay end,
 * because every leg ends in a reverse and a reverse is the end of a route (the agent parks at
 * the bay end), so there is nothing to splice across.
 *
 * AN ISLAND: nothing joins it to the loop, so no loop route can detour through a bay and no yard
 * route through the loop (Review Focus 4 - reverse edges carry no cost penalty yet).
 *
 * THE LAYOUT, uu (drawn to scale by Tools/Python/draw_rig_yard.py - keep the two in step):
 *
 *   W ==== NW ------ P1 ------ J ------ P2 ---- NE       | (hammer stub, north)
 *  (bay)   |                   | (90 bay)        |       |
 *          |                                     S ----- H ------ D
 *          |                                     |
 *          SW ---------------------------------- SE
 */
struct FRigYardLayout
{
	// Nodes. The north side runs at Y -7000, 30 m south of the loop's return road (Y -4000); the
	// floor reaches Y -17000 (build_rig_test_level.py), which the hammer stub's balloon clears.
	static constexpr double NorthY = -7000.0;
	static constexpr double SouthY = -16000.0;
	static constexpr double WestX = -2000.0;
	static constexpr double EastX = 13000.0;
	/**
	 * The straight bay's stub, west of NW: 45 m. The Wide tier's junction corners are laid for the
	 * rig and cut each arm back ~19 m (measured 2026-09-26: a 30 m arm left 8.9 m of lane), and the
	 * bay must hold the rig's 13.4 m chain beyond that - see FRoadGuidelineBuilder's bay check.
	 */
	static constexpr double StraightBayX = -6500.0;
	/**
	 * Pull-pasts, each longer than the rig's chain (13.4 m) plus the fillet's tangent (15 m) plus
	 * margin - FRoadGuidelineBuilder checks it, and refuses to lay a turn that is short.
	 */
	static constexpr double P1X = 3000.0;
	static constexpr double JX = 7000.0;
	static constexpr double P2X = 10500.0;
	/**
	 * The bays: 40 m each, to hold the rig WHOLE - trailer axle at the end, cab still on the bay's
	 * lane short of the junction (FRoadGuidelineBuilder refuses a shorter bay; the Wide corners
	 * take ~19 m of each arm). 25 m left the cab out on the junction, 2026-09-26. Their balloons
	 * reach 27 m further: the ring's south side at -16000 clears the 90 degree bay's; the hammer
	 * stub points NORTH, and its balloon stops ~8 m short of the loop's return road.
	 */
	static constexpr double Bay90Y = -11000.0;
	/**
	 * The spur leaves the east side 50 m below NE and 40 m above SE. At 20 m below NE (2026-09-26)
	 * the two junctions' corners squeezed each other to 5.1 m turns - the bowser's, under the
	 * rig's 5.8 m lock - and the rig was routed round U-turns in balloons it folds in.
	 * ENFORCED BY: AirportMgr.RigCourse.YardTurnsFitTheRig
	 */
	static constexpr double SpurY = -12000.0;
	static constexpr double HX = 16000.0;
	static constexpr double DX = 19500.0;
	static constexpr double HammerY = -8000.0;
	/** Every yard road is the Wide tier: the rig is its design vehicle (FRoadDesignVehicles). */
	static constexpr int32 Tier = 2;

	/** Lays the yard on Target and records its three reverse turns, in ERigYardFeature order. */
	static bool Lay(IRoadEditTarget& Target, TArray<FReverseTurn>& OutTurns, FRoadNodeId& OutStartNode,
		FRoadNodeId& OutStartFrom, int32& OutSegmentsLaid, int32& OutRefused);
};

class FRigYard
{
public:
	/** Lays the yard and resets the runners, one per vehicle, the second StartStagger later. */
	void Build(IRoadEditTarget& Target, int32 VehicleCount);

	/** One tick of every runner. Actor gives the network, the traffic and dispatch. */
	void Tick(ARoadNetworkActor& Actor, const TArray<FVehicle>& Vehicles, const TArray<FString>& Names, double DeltaSeconds);

	bool IsBuilt() const { return Turns.Num() == static_cast<int32>(ERigYardFeature::Count); }
	const FRigYardRunner& GetRunner(int32 Slot) const { return Runners[Slot]; }
	int32 RunnerCount() const { return Runners.Num(); }
	int32 GetSegmentsRefused() const { return SegmentsRefused; }

	/** Where Feature's reverse leg ends in Network's current graph - a runner's goal - or unset. */
	FGuidelineNodeId GoalNode(const URoadNetwork& Network, ERigYardFeature Feature) const;

	static const TCHAR* FeatureName(ERigYardFeature Feature);

	/**
	 * Seconds the second runner waits: about half a lap at the rig's pace (2026-09-26), so the two
	 * are rarely at one bay at once - a tow reversing across a junction another tow is queued at
	 * is traffic the claim model was not written for, and the yard is here to show reversing.
	 */
	static constexpr double StartStagger = 90.0;

	/** A leg's allowance, seconds, before it is logged stuck and redispatched. Generous: a leg ends in a crawl. */
	static constexpr double LegTimeout = 600.0;

private:
	void TickRunner(FRigYardRunner& Runner, ARoadNetworkActor& Actor, const FVehicle& Vehicle, const FString& Who, double DeltaSeconds);
	/** Parked, when set: the agent leaving the last bay - its live chain seeds the router's judgement. */
	bool PlanTo(const URoadNetwork& Network, FGuidelineNodeId From, ERigYardFeature Feature, const FVehicle& Vehicle,
		FRoutePlan& OutPlan, FString& OutWhy, const struct FRoadAgent* Parked = nullptr) const;
	void Dispatch(FRigYardRunner& Runner, ARoadNetworkActor& Actor, const FVehicle& Vehicle, const FString& Who);

	TArray<FReverseTurn> Turns;
	FRoadNodeId StartNode;
	FRoadNodeId StartFrom;
	int32 SegmentsLaid = 0;
	int32 SegmentsRefused = 0;
	TArray<FRigYardRunner> Runners;
};
