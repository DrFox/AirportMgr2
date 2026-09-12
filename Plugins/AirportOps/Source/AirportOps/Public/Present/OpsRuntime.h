#pragma once

#include "CoreMinimal.h"
#include "Model/OfferGenerator.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsRuntime.generated.h"

class ARoadNetworkActor;
class UOpsCatalog;
class UOpsEvents;
class UFuelService;
class UFlightBoard;
class UOfferGenerator;
enum class EAgentPhase : uint8;
enum class EArrivalRefusal : uint8;

/**
 * The AirportOps composition root. Owns the clock and the event bus, attaches to the one
 * ARoadNetworkActor, relays Airside delegates onto the bus, pushes the speed multiplier
 * down into the actor each tick, and performs save/load end to end.
 *
 * A UObject rather than the subsystem itself so a test can NewObject one, Attach a spawned
 * actor and Tick it by hand - a UGameInstanceSubsystem needs a UGameInstance, which a
 * CreateWorld test does not have. UOpsRuntimeSubsystem is the forwarder that gives this
 * a lifetime in play. Same split as ARoadNetworkActor (composition root) over
 * UAirsideTraffic (testable subobject), for the same reason.
 *
 * It GROWS BY FORWARDING. UFuelService is the first such subobject: this class gained a
 * pointer, three lines in Attach/Tick/OnAgentPhase, and no logic at all. Flight board, job
 * board and ledger arrive the same way in later milestones - logic lands in them, not here.
 */
UCLASS()
class AIRPORTOPS_API UOpsRuntime : public UObject
{
	GENERATED_BODY()

public:
	UOpsRuntime();

	USimClock* GetClock() const { return Clock; }
	UOpsEvents* GetEvents() const { return Events; }
	UOpsCatalog* GetCatalog() const { return Catalog; }

	/** The fuel jobs. See UFuelService - this runtime owns it, feeds it the phase events and
	 *  ticks it, and that is the whole of the wiring. */
	UFuelService* GetFuelService() const { return FuelService; }

	/** Every flight, and the one caller of DispatchArrival. See UFlightBoard. */
	UFlightBoard* GetFlightBoard() const { return FlightBoard; }

	/** Where offers come from. Fed the catalog's airlines by this runtime, on the clock. */
	UOfferGenerator* GetOfferGenerator() const { return OfferGenerator; }

	ARoadNetworkActor* GetTarget() const { return Target; }

	/** Binds to the actor's traffic delegates. Safe to call again with a new actor (unbinds the old). */
	void Attach(ARoadNetworkActor* Actor);

	/** Advances the clock and pushes the speed multiplier into the actor. Real seconds in. */
	void Tick(double RealDeltaSeconds);

	/**
	 * The rungs StepSpeed walks, in order, fastest last. Paused is deliberately NOT a rung:
	 * it is TogglePause's business, and "step faster" from paused means resume, not unpause
	 * into the slowest speed.
	 *
	 * Public because it is a list that must agree with ESimSpeed and something has to be
	 * able to check that - see its definition.
	 */
	static TArrayView<const ESimSpeed> SpeedLadder();

	/** Speed control. StepSpeed(+1) climbs SpeedLadder() and stops at the top; -1 descends. */
	void StepSpeed(int32 Delta);
	/** Paused <-> the speed that was set before pausing. */
	void TogglePause();

	bool SaveToSlot(const FString& SlotName);
	/** Restores clock and network, clears agents and undo history, rebuilds the actor's mesh. */
	bool LoadFromSlot(const FString& SlotName);

private:
	UPROPERTY() TObjectPtr<USimClock> Clock;
	UPROPERTY() TObjectPtr<UOpsEvents> Events;
	UPROPERTY() TObjectPtr<UOpsCatalog> Catalog;
	UPROPERTY() TObjectPtr<UFuelService> FuelService;
	UPROPERTY() TObjectPtr<UFlightBoard> FlightBoard;
	UPROPERTY() TObjectPtr<UOfferGenerator> OfferGenerator;
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/** The repeating offer callback, so Detach can cancel it. INDEX_NONE when unattached. */
	int32 OfferHandle = INDEX_NONE;

	/** The catalog's airlines, flattened into airframes Model/ may read. See the .cpp. */
	TArray<FOfferCandidate> CandidatesFromCatalog() const;

	/** One offer, on the clock. Bound in Attach. */
	void GenerateOffer();

	/** What TogglePause returns to. X1 if nothing was ever set. */
	UPROPERTY() ESimSpeed ResumeSpeed = ESimSpeed::X1;

	FDelegateHandle PhaseHandle;
	FDelegateHandle RefusalHandle;

	void Detach();
	void ApplySpeed(ESimSpeed Speed);
	void OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void OnArrivalRefused(EArrivalRefusal Why);
};
