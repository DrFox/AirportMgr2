#pragma once

#include "CoreMinimal.h"
#include "Model/OpsSave.h"
#include "UObject/Object.h"
#include "Airport.generated.h"

class FOpsEventBus;
class URoadNetwork;

/**
 * Whether the airport takes traffic, and why not. Spec 2026-09-29-ops-batch3 §3.
 *
 * AN ENUM, NOT TWO BOOLS ("closed", "has a runway"): the player's closure wins over the missing runway,
 * and the pair of flags could say both - this says the one that governs. Draining is NOT a state: aircraft
 * already on the ground are a readout (UFlightBoard::OnGroundCount), not a fourth value to keep in step.
 */
UENUM(BlueprintType)
enum class EAirportStatus : uint8
{
	Open,
	/** The player closed it. Only the player reopens it, whatever the runways do. */
	ClosedByPlayer,
	/** Open by intent, but no runway to land on - an airport without a runway is not an airport (ruling 2026-09-29). */
	NoRunway
};

/**
 * The airport's status: the player's intent, and the status DERIVED from it and the network.
 *
 * STORES ONLY INTENT. bClosedByPlayer is the one saved field (blob "Airport"; a snapshot without it is an
 * open airport); the status is re-derived from it and the runways, never saved, because a saved status
 * could disagree with the network it was loaded beside.
 *
 * RE-DERIVED ON A CHANGE THE PLAYER COULD SEE: the network changing (UOpsRuntime's FNetworkChangedEvent
 * subscription) and the close/open command, which re-derives at once rather than waiting for an event -
 * a command is not an event. A change publishes FAirportStatusChangedEvent; entering a closed status is
 * what cancels the unarrived flights (UOpsRuntime::WireBus). ATTACH AND LOAD RE-DERIVE SILENTLY (Reseat):
 * neither is a change the player made, and a load must never re-run the cancellation (spec §3 "Load").
 * ENFORCED BY: AirportOps.Present.Airport.LoadRederivesWithoutCancelling
 *
 * World-free, like the boards: a network in, a status out.
 */
UCLASS()
class AIRPORTOPS_API UAirport : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	// --- IOpsPersistent ---------------------------------------------------------------
	virtual FName SaveBlobName() const override { return TEXT("Airport"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/** A SNAPSHOT WITH NO "Airport" BLOB IS AN OPEN AIRPORT: cleared before any blob is read, so a save
	 *  from before this class existed does not inherit whatever the session had. */
	virtual void OnBeforeRestore() override { bClosedByPlayer = false; }

	/** Where a status change is announced. Set by UOpsRuntime::Attach; null in a bare NewObject, and the
	 *  publish checks. Raw: the runtime owns both this and the bus. */
	FOpsEventBus* Bus = nullptr;

	/** The rule, alone: the player's intent wins, then the runway. */
	static EAirportStatus Derive(bool bInClosedByPlayer, bool bHasRunway);

	/** True when Network has at least one runway - the one test Refresh and Reseat use. */
	static bool HasRunway(const URoadNetwork& Network);

	/** The status as last derived. */
	EAirportStatus Status() const { return Current; }
	bool IsClosedByPlayer() const { return bClosedByPlayer; }

	/** Re-derive against Network; on a change, log it and publish FAirportStatusChangedEvent. True if it changed. */
	bool Refresh(const URoadNetwork& Network);

	/** Re-derive with NO event - an attach or a load. See the class comment. */
	void Reseat(const URoadNetwork& Network);

	/** The player's close / open command: records the intent and re-derives at once (Refresh). True if the
	 *  status changed. */
	bool SetClosedByPlayer(bool bClosed, const URoadNetwork& Network);

	/** A new game opens: UOpsRuntime::Attach, beside ULedger::Open and UAirlineRoster::ResetForNewGame. THE DERIVED
	 *  STATUS TOO (review M2), so the old game's closure is not the status read before the attach's Reseat.
	 *  ENFORCED BY: AirportOps.Model.Airport.NewGameForgetsTheStatus */
	void ResetForNewGame() { bClosedByPlayer = false; Current = EAirportStatus::Open; }

	/** How many status changes Refresh has published this session - for the attach/load "no event" tests. */
	int32 ChangeCountForTest() const { return ChangeCount; }

private:
	/** The player's intent. THE saved state - see the class comment. */
	UPROPERTY() bool bClosedByPlayer = false;

	/** The derived status Refresh diffs against. Transient: re-derived after every load. */
	UPROPERTY(Transient) EAirportStatus Current = EAirportStatus::Open;

	/** See ChangeCountForTest. A session counter, not saved. */
	int32 ChangeCount = 0;
};
