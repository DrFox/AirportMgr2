#pragma once

#include "CoreMinimal.h"
#include "Model/RouteSearch.h"
#include "Solve/TowReverse.h"
#include "TowReverseRun.generated.h"

struct FVehicle;

/**
 * A TOW CHAIN BACKING ALONG A REVERSE LEG (spec 2026-09-26 §1): FReverseRun's sibling for a
 * vehicle with a trailer, played inside the same EAgentPhase::Reversing. FRoadAgent chooses
 * between the two by FVehicle::HasTrailer() - a fact about the vehicle, not a second state flag
 * beside the phase (CLAUDE.md, "a phase is an enum").
 *
 * WHY NOT FReverseRun: that walks the FIXED axle along the line and moves no chain - the frozen
 * trailer step 1 left open (spec 2026-09-24, "The reverse chain"). Going backwards the leading
 * axle is the TRAILER's, and the tractor's path is whatever the hitch needs, which has no closed
 * form; TowReverse::Solve finds it once, and this plays the result back.
 *
 * WORLD-FREE, like its siblings. Airside.Model.TowReverse drives it through a bare FRoadAgent.
 */
USTRUCT()
struct AIRSIDE_API FTowReverseRun
{
	GENERATED_BODY()

	/** The reverse leg, a copy - FReverseRun::Plan's reason: one object for a rebuild to find. */
	UPROPERTY() FRoutePlan Plan;

	/** The ask, uu/s, written by Start. Applied to the LEADING axle - the one on the line. */
	UPROPERTY() double ReverseSpeed = 0.0;

	/** What the last Advance covered, uu/s, a MAGNITUDE - FReverseRun::Speed's contract. */
	UPROPERTY() double Speed = 0.0;

	/** The displayed road-wheel angle, degrees - TowReverse::FSample::SteerDegrees, interpolated. */
	UPROPERTY() double SteerDegrees = 0.0;

	/** The leading axle's distance along Plan.Polyline, uu. */
	UPROPERTY() double Along = 0.0;

	/**
	 * The solved manoeuvre. NOT REFLECTED: it is derived, all of it, by Start from the vehicle and
	 * the plan, and a duplicate of an agent mid-reverse is not a case this game has (single-
	 * player, no saves yet - no-player-saves-yet, 2026-09-23).
	 */
	TArray<TowReverse::FSample> Samples;

	/**
	 * Solves and arms the reverse from where the vehicle IS - its body origin and heading, and its
	 * chain's axles as they stand (the measured-hitch handover, ruled 2026-09-26). False, nothing
	 * armed, when the solve refuses; OutReason (optional) then carries TowReverse's one-line reason.
	 */
	bool Start(const FRoutePlan& InPlan, const FVehicle& Vehicle, const FVector2D& Origin, double Heading,
		TArrayView<const FVector2D> Axles, double InReverseSpeed, FString* OutReason = nullptr);

	/**
	 * One frame. FALSE MEANS THE MANOEUVRE IS OVER and NOTHING MOVED this call - FReverseRun's
	 * post-#297 contract, so FRoadAgent's handover reads the same for both. On false the outputs
	 * hold the final pose, the pose Park or the drive-on starts from. StopWithin caps the leading
	 * axle's travel, as arbitration caps the follower's.
	 */
	bool Advance(double DeltaSeconds, double StopWithin, const FVehicle& Vehicle, FVector2D& OutOrigin,
		double& OutHeading, TArray<FVector2D>& OutAxles);

	bool IsArmed() const { return Samples.Num() >= 2; }
	bool HasArrived() const { return IsArmed() && Along >= Samples.Last().Along - UE_KINDA_SMALL_NUMBER; }
	const TowReverse::FSample* Last() const { return IsArmed() ? &Samples.Last() : nullptr; }
	void Reset();

private:
	/** The sample at or before Along, walked forward only: Advance only ever adds to Along, and Start resets both. */
	int32 Cursor = 0;

	/** Writes the pose at Along, interpolated between the two samples that bracket it. */
	void PoseAt(const FVehicle& Vehicle, FVector2D& OutOrigin, double& OutHeading, TArray<FVector2D>& OutAxles);
};
