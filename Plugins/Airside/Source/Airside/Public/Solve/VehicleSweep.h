#pragma once

#include "CoreMinimal.h"

/**
 * How much road a vehicle sweeps holding a turn (spec 2026-09-23 §6). Dependency-free, like
 * the rest of Solve/: Model/VehicleFit maps an FVehicle onto FBody.
 *
 * STEADY STATE, which is CONSERVATIVE: a 90 degree turn is over before the trailer settles to
 * the off-tracking computed here, so the real sweep is smaller. Chosen because it is a closed
 * form with no dependence on the path before the corner, and because the cost of being wrong
 * in this direction is a corner sized a little generously rather than a trailer over a kerb.
 *
 * THE STEERED AXLE RIDES THE LINE, as it does in FRouteFollower, so Radius is the steered
 * axle's; the fixed axle runs at sqrt(R^2 - L^2) and every body corner is placed from there.
 * ENFORCED BY: Airside.Model.FollowerMatchesSweep (follower and Trace agree on the cut-in)
 */
namespace VehicleSweep
{
	/**
	 * One link of a tow, uu - FTowLink's figures, which Solve/ cannot see (CoreMinimal only).
	 * HitchX along the PREVIOUS body from its fixed axle (negative behind); Length hitch to this
	 * link's axle; BodyFront ahead of the hitch, BodyRear behind the axle. Was FBody's
	 * KingpinX / KingpinToAxle / TrailerFront / TrailerRear / TrailerWidth, one semi-trailer.
	 */
	struct FLink
	{
		double HitchX = 0.0;
		double Length = 0.0;
		double BodyFront = 0.0;
		double BodyRear = 0.0;
		double Width = 0.0;
	};

	/**
	 * A body in the fixed axle's frame, uu, and what it pulls. An empty Tow means rigid.
	 *
	 * INLINE FOR TWO LINKS - the rig has one, a drawbar trailer two - because the agent maps an
	 * FVehicle onto this every sub-step (FRoadAgent::DescribeMotion, the tow step) and a heap
	 * allocation per call is a cost with nothing bought by it. A baggage train spills to the
	 * heap and still works.
	 */
	struct FBody
	{
		double Wheelbase = 0.0;
		double Width = 0.0;
		double FrontX = 0.0;
		double RearX = 0.0;
		TArray<FLink, TInlineAllocator<2>> Tow;
	};

	/**
	 * Past square to whatever pulls it, a link has JACK-KNIFED: 90 degrees. Driving forwards
	 * inside the lock never reaches it, so for forward driving it is a bug detector, and the
	 * reversing step will need it for real (spec §1).
	 *
	 * WHY 90 AND NOT A MEASURED LIMIT: the models carry no stop on the fifth wheel or the
	 * drawbar, and 90 is where the pursuit itself stops meaning anything - past square, a
	 * forward pull on the hitch drives the axle BACKWARDS. It is the same line StepTrailer's
	 * Dot < 0 draws, named, so a tighter limit (a real trailer's ~70-80 degrees of body
	 * clearance) is one edit here rather than a sign test somewhere else.
	 */
	constexpr double MaxHitchRadians = UE_HALF_PI;

	/**
	 * How far the chain is stepped at a time, uu: Trace's step, and the longest the agent's
	 * sub-step takes it (FRoadAgent's tow step divides this by the vehicle's speed cap).
	 *
	 * ONE NUMBER FOR BOTH because the pursuit is FIRST-ORDER in its step: two walkers at
	 * different step lengths trace measurably different trailer paths, and the router would
	 * admit the rig on one while the driver drove the other. 10 uu: a tenth of the lane margin,
	 * and the step the Python prototype that set the test figures used (2026-09-24).
	 */
	constexpr double TraceStep = 10.0;

	/** Where a link is, after a step: its hitch, its axle, and its heading (unit, axle to hitch). */
	struct FLinkPose
	{
		FVector2D Hitch = FVector2D::ZeroVector;
		FVector2D Axle = FVector2D::ZeroVector;
		FVector2D Heading = FVector2D::UnitX();
	};

	/**
	 * Lays a chain DEAD STRAIGHT behind a tractor whose fixed axle is at Fixed facing Heading
	 * (unit) - how a vehicle spawns, and how Trace starts its lead-in. OutAxles gets one axle
	 * per link.
	 */
	AIRSIDE_API void LayChainStraight(const FBody& Body, const FVector2D& Fixed, const FVector2D& Heading,
		TArray<FVector2D>& OutAxles);

	/**
	 * Where every link of a chain IS, given its axles and the tractor's fixed axle and heading,
	 * without moving anything: each hitch sits HitchX along its puller from the puller's axle,
	 * and each link faces from its axle to its hitch. What the view is shown, and what Trace
	 * puts body corners on.
	 */
	AIRSIDE_API void PoseChain(const FBody& Body, const FVector2D& Fixed, const FVector2D& Heading,
		TArrayView<const FVector2D> Axles, TArray<FLinkPose, TInlineAllocator<2>>& OutPoses);

	/**
	 * THE CHAIN STEP, the one walk both Trace and FRoadAgent make. The tractor's fixed axle has
	 * moved to Fixed facing Heading (unit); every link, in order, is pulled by its puller's hitch
	 * point with StepTrailer - link 0 by the tractor, link k by link k-1 as it has JUST moved.
	 *
	 * False on a JACK-KNIFE: StepTrailer's own fold (Dot < 0), or a link past MaxHitchRadians to
	 * its puller. OutFoldedLink and OutFoldRadians then name the first link that folded and its
	 * angle; links after it are not stepped. Otherwise they are INDEX_NONE and the worst angle.
	 */
	AIRSIDE_API bool StepChain(const FBody& Body, const FVector2D& Fixed, const FVector2D& Heading,
		TArrayView<FVector2D> InOutAxles, int32& OutFoldedLink, double& OutFoldRadians);

	/**
	 * Offsets from the steered axle's path, uu: Inner toward the turn centre, Outer away from
	 * it. bHolds false when the turn cannot be held at all - tighter than the wheelbase, or a
	 * kingpin circle smaller than the trailer (it would jack-knife; since the chain, ANY link's
	 * hitch circle smaller than that link) - and then the offsets are meaningless.
	 */
	struct FEnvelope
	{
		double Inner = 0.0;
		double Outer = 0.0;
		bool bHolds = false;
	};

	AIRSIDE_API FEnvelope Envelope(const FBody& Body, double SteerRadius);

	/**
	 * Advance a trailer whose kingpin has moved to Kingpin: the axle is pulled toward it and
	 * kept KingpinToAxle behind (discrete tractrix, the pursuit Trace already used). Returns
	 * false when the trailer has jack-knifed (its heading opposes CabHeading).
	 */
	AIRSIDE_API bool StepTrailer(const FVector2D& Kingpin, const FVector2D& CabHeading,
		double KingpinToAxle, FVector2D& InOutTrailerAxle);

	/** Trailer heading (unit) from axle to kingpin. */
	AIRSIDE_API FVector2D TrailerHeading(const FVector2D& Kingpin, const FVector2D& TrailerAxle);

	/**
	 * THE TURN AS DRIVEN, not the steady state (review of 2026-09-24). The steered axle walks
	 * Path - lead in straight along its first tangent, out straight along its last, far enough
	 * for the whole vehicle - the fixed axle pursues it at the wheelbase, the kingpin rides the
	 * tractor, and the trailer axle pursues the kingpin at KingpinToAxle. At every step each
	 * body corner (and each axle end) is projected onto Path; its reach toward the turn's
	 * centre and away from it is kept against the NEAREST SAMPLE, so OutInner[i]/OutOuter[i]
	 * say how far the body reached either side of Path[i]. Points that project beyond either
	 * end are in the straight lanes and are not recorded - lane Width gates those.
	 *
	 * WHY: a 90 degree corner ends before the trailer settles, so it cuts in less than
	 * Envelope says - the steady-state model made the first Wide corner 30 m, where real rigs
	 * turn in yards. It also cannot say WHERE the cut-in happens, which Trace does.
	 *
	 * False when the trailer folds past square to the tractor (a jack-knife): not drivable.
	 * Measured on the SAME samples the follower walks, per the sample-once rule.
	 *
	 * A CHAIN since 2026-09-24: every link is stepped by StepChain and every link's body adds
	 * its corners, so a drawbar trailer is gated exactly as the rig is. OutAxles, when given,
	 * receives every link's axle at every step, link-major within a step (step i, link k at
	 * i * Tow.Num() + k) - what Airside.Model.Tow.FollowerMatchesTraceForRig holds the driving
	 * agent to. Nothing in production asks for it.
	 */
	AIRSIDE_API bool Trace(const FBody& Body, TArrayView<const FVector2D> Path,
		TArray<double>& OutInner, TArray<double>& OutOuter, TArray<FVector2D>* OutAxles = nullptr);
}
