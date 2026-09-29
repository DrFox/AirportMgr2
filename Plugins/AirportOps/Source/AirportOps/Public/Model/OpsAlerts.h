#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "OpsAlerts.generated.h"

class FOpsEventBus;
class UFlightBoard;
class UGroundTraffic;
class UJobBoard;
class ULedger;
class UOfferGenerator;
class URoadNetwork;
struct FAirlineOffers;

/** Which standing problem an alert is. Spec 2026-09-29-ops-alerts §1 - one per condition kind. */
UENUM(BlueprintType)
enum class EAlertKind : uint8
{
	FlightStranded,
	VehicleStranded,
	JobUnserviceable,
	HeldStandLost,
	AirlineCannotCome,
	Deadlock,
	Overdrawn
};

/** What "Go" moves the camera to. None for a problem with no place in the world. */
UENUM(BlueprintType)
enum class EAlertFocusKind : uint8
{
	None,
	Agent,
	Entity,
	Point
};

/**
 * One alert per condition per subject: re-detecting it never stacks a second one. Id is the subject's own
 * id (a flight, a vehicle, a job, a cycle's lowest member); Name is for the one kind whose subject has no
 * stable integer - an airline, by its definition's name (an index into the runtime's list would change on
 * a re-attach).
 */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FOpsAlertKey
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) EAlertKind Kind = EAlertKind::FlightStranded;
	UPROPERTY(BlueprintReadOnly) int32 Id = 0;
	UPROPERTY(BlueprintReadOnly) FName Name;

	bool operator==(const FOpsAlertKey& Other) const { return Kind == Other.Kind && Id == Other.Id && Name == Other.Name; }
	friend uint32 GetTypeHash(const FOpsAlertKey& Key)
	{
		return HashCombine(HashCombine(::GetTypeHash(static_cast<uint8>(Key.Kind)), ::GetTypeHash(Key.Id)), GetTypeHash(Key.Name));
	}
};

/** Where an alert's subject is. Point is filled for every kind with a place, even an Agent or an Entity,
 *  so "Go" can move the camera even when it cannot select the subject. */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FAlertFocus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) EAlertFocusKind Kind = EAlertFocusKind::None;
	/** The agent id, or the entity's INDEX (a selection is by index). */
	UPROPERTY(BlueprintReadOnly) int32 Id = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly) FVector2D Point = FVector2D::ZeroVector;
};

USTRUCT(BlueprintType)
struct AIRPORTOPS_API FOpsAlert
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FOpsAlertKey Key;
	/** The sentence the toast and the alert row show. Built here - the one wording owner (spec §1). */
	UPROPERTY(BlueprintReadOnly) FText Text;
	UPROPERTY(BlueprintReadOnly) FAlertFocus Focus;
	/** USimClock game seconds it was first raised. */
	UPROPERTY(BlueprintReadOnly) double RaisedAt = 0.0;

	/**
	 * Raised by the first recompute after a Reset (a load, an attach) rather than because the condition
	 * just started. The alert list shows it; the toast does not - a load re-announcing every standing
	 * problem as news would bury the ones that really are (stage 1 review).
	 */
	UPROPERTY(BlueprintReadOnly) bool bReRaised = false;
};

/** What a recompute reads. Any null skips the kinds it answers - a test, or a runtime not yet attached. */
struct FOpsAlertSources
{
	const UFlightBoard* Flights = nullptr;
	const UJobBoard* Jobs = nullptr;
	const UGroundTraffic* Traffic = nullptr;
	const URoadNetwork* Network = nullptr;
	const UOfferGenerator* Offers = nullptr;
	const ULedger* Ledger = nullptr;
	/** For an airline's display name; an airline missing here is named by its key. */
	TArrayView<const FAirlineOffers> Airlines;
};

/**
 * The standing problems the player must act on, DERIVED FROM STATE. Spec 2026-09-29-ops-alerts §1.
 *
 * Pattern: a derived view, recomputed on demand and diffed - not a registry fed by paired raise/clear
 * events. The paired design was rejected because every way a condition can END would need its own publish,
 * and one missed path leaves an alert stuck for ever (the trap the runway crossing set, GroundTraffic.h's
 * OccupancyRevision comment). Here an alert clears because its condition stopped being true, whatever made
 * it stop.
 *
 * RUN AS THE BUS PASS "Alerts" (UOpsRuntime::WireBus), dirtied by the events that can change a condition.
 * It publishes FAlertRaisedEvent / FAlertClearedEvent for the differences; the toast and the alert window
 * hear those, never this object.
 *
 * NOT SAVED: every alert is a function of saved state, so a load Resets and the next pass re-raises.
 */
UCLASS()
class AIRPORTOPS_API UOpsAlerts : public UObject
{
	GENERATED_BODY()

public:
	/** Where raises and clears are published. Set by UOpsRuntime; null in a bare NewObject. Raw: the
	 *  runtime owns both. */
	FOpsEventBus* Bus = nullptr;

	/** Work out every true condition, and publish what changed since the last call. */
	void Recompute(const FOpsAlertSources& Sources, double Now);

	/**
	 * Forget every alert - a load or an attach. Publishes FAlertsResetEvent (no per-alert clears: the
	 * subjects belong to the airport being replaced), so a UI list empties; the next Recompute re-raises
	 * whatever is still true, marked bReRaised.
	 * ENFORCED BY: AirportOps.Present.Alerts.PassRaisesThroughTheRuntime
	 */
	void Reset();

	const TArray<FOpsAlert>& GetAlerts() const { return Alerts; }

	/** How many times Recompute has run - for the pass's composition tests. */
	int32 RecomputeCountForTest() const { return RecomputeCount; }

private:
	UPROPERTY(Transient) TArray<FOpsAlert> Alerts;
	int32 RecomputeCount = 0;
	/** Set by Reset: the next Recompute's raises are re-raises. */
	bool bAfterReset = false;
};
