#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Solve/VehicleSweep.h"

struct FChassis;
struct FGuidelineEdge;
struct FRoutePlan;
struct FVehicle;
class URoadNetwork;

/**
 * Which of VehicleFit's rules refused an edge. A PLAIN enum: it travels on FFitVerdict, which
 * nothing reflects (CLAUDE.md on plain enums and UHT).
 */
enum class EFitRefusal : uint8
{
	None,
	/** The widest body plus its margins is wider than the lane. */
	LaneTooNarrow,
	/** The curve is tighter than the steering lock can follow. */
	TighterThanLock,
	/** The simulated tow folded past VehicleSweep::MaxHitchRadians on the curve. */
	Jackknife,
	/** At some sample the swept width is wider than the tarmac there. */
	SweptOverTarmac,
	/**
	 * THE WHOLE ROUTE, driven as the agent drives it with the chain carried across every edge
	 * boundary, folds a link past VehicleSweep::MaxHitchRadians (VehicleFit::JudgePlan).
	 * Jackknife above is ONE curve traced from a straight start; this is the fold that two
	 * curves, or a balloon's pieces, add up to - which no per-edge verdict can see.
	 */
	TrailerFolds
};

/**
 * VehicleFit's answer with its figures, so a refusal can say "swept 7.6 m vs tarmac 6.0 m"
 * rather than only "no" (spec 2026-09-24 §3, the test course's refusal line).
 *
 * Needed/Available are in uu and mean, per Refusal: LaneTooNarrow - body plus margins vs lane
 * width; TighterThanLock - the lock's radius vs the curve's MinRadius; SweptOverTarmac - swept
 * width vs tarmac width at the WORST sample (Sample). Zero where the rule has no figure.
 */
struct AIRSIDE_API FFitVerdict
{
	EFitRefusal Refusal = EFitRefusal::None;
	double Needed = 0.0;
	double Available = 0.0;
	int32 Sample = INDEX_NONE;

	/**
	 * WHOLE-ROUTE VERDICTS ONLY (JudgePlan; bWholeRoute): WHERE on the plan, since a route has
	 * no single edge to name. Edge is the plan step's edge the refusal lies on - for a fold, the
	 * one the CAB is driving when the trailer goes - and Node the node
	 * that step leaves; for TrailerFolds, Link and Radians are the folded link and its angle, At
	 * that link's axle (the point FRoadAgent's own jack-knife warning names) and Along the
	 * steered axle's route distance. For SweptOverTarmac, Sample indexes Edge's own samples, as
	 * the per-edge verdict's does. On a whole-route PASS, Radians is the worst hitch angle the
	 * drive reached - how much margin the route had.
	 */
	bool bWholeRoute = false;
	FGuidelineEdgeId Edge;
	FGuidelineNodeId Node;
	FVector2D NodeAt = FVector2D::ZeroVector;
	int32 Link = INDEX_NONE;
	double Radians = 0.0;
	FVector2D At = FVector2D::ZeroVector;
	double Along = 0.0;

	bool Fits() const { return Refusal == EFitRefusal::None; }

	/**
	 * One line for a log: "swept 7.6 m vs tarmac 6.0 m at sample 12", or "trailer folds at
	 * guideline node 41 / (1200, -300), link 0, angle 91 deg". Empty when it fits.
	 */
	FString Describe() const;
};

/**
 * Whether a vehicle's body fits an edge (spec 2026-09-23 §6) - the ONE rule route search
 * gates vehicles on, beside the wingspan rule it gates aircraft on.
 *
 *   A lane: the widest body plus WidthMargin each side within the edge's Width.
 *   A curve: its measured MinRadius no tighter than the steering lock, and the vehicle
 *   SIMULATED along the curve's own samples (VehicleSweep::Trace) without jack-knifing and
 *   with its swept width, at every sample, inside the tarmac's width there.
 *
 * SWEPT WIDTH AGAINST TARMAC WIDTH, not inside against inside (2026-09-24): the line is the
 * lane's centre, and a real rig turning on the near side swings OUT into the other lane to
 * keep its trailer off the kerb. Holding it to its own lane's line refused turns real rigs
 * make every day. The cost, stated: nothing here reserves the other lane, so an oncoming
 * vehicle can overlap a rig mid-turn.
 *
 * UNMEASURED GATES NOTHING: Width 0, MinRadius 0, no per-sample clearances - hand-drawn edges,
 * straight lanes, balloons over grass, anything saved before this existed - and a vehicle
 * with no measured body (BodyWidth 0) is checked for its lock only.
 */
namespace VehicleFit
{
	/** Kept clear each side of a body in a lane, uu: mirrors and the wobble of a real driver. */
	constexpr double WidthMargin = 15.0;

	/**
	 * The rule, with its reason. Fits() below is this call's bool - ONE evaluator, so a refusal
	 * line can never name a figure the router did not actually judge on.
	 * ENFORCED BY: Airside.Model.VehicleFitClearance (Judge and Fits agree case for case)
	 */
	AIRSIDE_API FFitVerdict Judge(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network);

	AIRSIDE_API bool Fits(const FGuidelineEdge& Edge, const FVehicle& Vehicle, const URoadNetwork& Network);

	/**
	 * The vehicle as VehicleSweep sees it: the tractor's body and its tow, link by link.
	 *
	 * ONE MAPPING, shared by the router (Fits, above) and the driver (FRoadAgent's tow step and
	 * DescribeMotion). Two hand-written copies of this would be two chains that could disagree
	 * about a hitch - the second evaluator the one-stepper rule exists to prevent.
	 */
	AIRSIDE_API VehicleSweep::FBody BodyOf(const FVehicle& Vehicle);

	/**
	 * THE TOW'S TIME STEP, s: VehicleSweep::TraceStep at the chassis's speed cap, so no sub-step
	 * pulls the chain further than TraceStep. FRoadAgent's tow step divides a frame by it and
	 * JudgePlan steps by it - one number, not two that could drift.
	 */
	AIRSIDE_API double TowSubStepSeconds(const FChassis& Chassis);

	/**
	 * The tractor's FIXED AXLE for a body whose origin is at Origin facing Heading (radians) -
	 * FRouteFollower's output - and that heading as a unit vector. What StepTow pulls the chain
	 * with and JudgePlan puts the cab's corners on.
	 */
	AIRSIDE_API FVector2D FixedAxleAt(const FChassis& Chassis, const FVector2D& Origin, double Heading,
		FVector2D& OutForward);

	/**
	 * Lays a vehicle's chain DEAD STRAIGHT behind a cab whose STEERED axle is at Steered facing
	 * Forward (unit): how FRoadAgent::StartDrive spawns a tow, and how JudgePlan starts one at
	 * the plan's first point. Empty for anything rigid.
	 */
	AIRSIDE_API void LayTow(const FVehicle& Vehicle, const FVector2D& Steered, const FVector2D& Forward,
		TArray<FVector2D>& OutAxles);

	/**
	 * ONE SUB-STEP OF THE TOW: the chain pulled by a cab whose origin the follower has just put at
	 * Origin facing Heading (radians). VehicleSweep::StepChain's verdict and outputs. THE ONE
	 * CALL both FRoadAgent's FollowAndTow and JudgePlan make after each follower advance, so the
	 * router's whole-route verdict and the driven agent's fold are one evaluator.
	 * ENFORCED BY: Airside.Model.Tow.WholeRouteVerdictIsTheAgents (verdict == agent folds, plan for plan)
	 */
	AIRSIDE_API bool StepTow(const VehicleSweep::FBody& Body, const FChassis& Chassis, const FVector2D& Origin,
		double Heading, TArrayView<FVector2D> InOutAxles, int32& OutFoldedLink, double& OutFoldRadians);

	/**
	 * THE WHOLE-ROUTE TOW CHECK (spec 2026-09-24 Open, "one evaluator for a whole route's tow",
	 * built 2026-09-25): drives Plan as FRoadAgent would - a FRouteFollower from rest, the chain
	 * laid straight ONCE at the plan's first point (StartDrive) and then stepped with StepTow
	 * every TowSubStepSeconds, never re-laid at an edge boundary - and refuses it when:
	 *   TrailerFolds    - a link folds (StepChain false, or the angle guard);
	 *   SweptOverTarmac - the swept body, projected onto the plan's own polyline, is wider than
	 *                     the tarmac at a sample of an edge that HAS per-sample clearances. The
	 *                     same rule as Judge's, on the same samples; edges without data (balloons,
	 *                     straights, hand-drawn) gate nothing here either.
	 *
	 * WHY THE FOLLOWER AND NOT Trace's PURSUIT: Trace puts the fixed axle a wheelbase behind a
	 * steered axle pinned to the line; the agent's follower slews its heading at its lock. The
	 * two agree to 5 uu on one corner (Tow.FollowerMatchesTraceForRig), but at the fold line -
	 * the one place this check decides anything - 5 uu of hitch is the difference. Running the
	 * follower is what makes "the router admitted it" and "the agent drove it" the same fact.
	 *
	 * A plan's REVERSE LEGS are not driven: the check stops at the first, as FRoadAgent hands
	 * those to FReverseRun, which does not step the chain (spec Open: the reverse chain).
	 * Rigid vehicles fit trivially; RouteSearch does not call this for them at all.
	 */
	AIRSIDE_API FFitVerdict JudgePlan(const FRoutePlan& Plan, const FVehicle& Vehicle, const URoadNetwork& Network);
}
