#pragma once

#include "CoreMinimal.h"
#include "Model/SimClock.h"
#include "UObject/Object.h"
#include "OpsRuntime.generated.h"

class ARoadNetworkActor;
class UOpsEvents;
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
 * It GROWS BY FORWARDING. Flight board, job board, ledger arrive as further owned
 * subobjects in later milestones; logic lands in them, not here.
 */
UCLASS()
class AIRPORTOPS_API UOpsRuntime : public UObject
{
	GENERATED_BODY()

public:
	UOpsRuntime();

	USimClock* GetClock() const { return Clock; }
	UOpsEvents* GetEvents() const { return Events; }
	ARoadNetworkActor* GetTarget() const { return Target; }

	/** Binds to the actor's traffic delegates. Safe to call again with a new actor (unbinds the old). */
	void Attach(ARoadNetworkActor* Actor);

	/** Advances the clock and pushes the speed multiplier into the actor. Real seconds in. */
	void Tick(double RealDeltaSeconds);

	/** Speed control. StepSpeed(+1) goes X1->X2->X4->X8 and stops; -1 the other way down to X1. */
	void StepSpeed(int32 Delta);
	/** Paused <-> the speed that was set before pausing. */
	void TogglePause();

	bool SaveToSlot(const FString& SlotName);
	/** Restores clock and network, clears agents and undo history, rebuilds the actor's mesh. */
	bool LoadFromSlot(const FString& SlotName);

private:
	UPROPERTY() TObjectPtr<USimClock> Clock;
	UPROPERTY() TObjectPtr<UOpsEvents> Events;
	UPROPERTY(Transient) TObjectPtr<ARoadNetworkActor> Target;

	/** What TogglePause returns to. X1 if nothing was ever set. */
	UPROPERTY() ESimSpeed ResumeSpeed = ESimSpeed::X1;

	FDelegateHandle PhaseHandle;
	FDelegateHandle RefusalHandle;

	void Detach();
	void ApplySpeed(ESimSpeed Speed);
	void OnAgentPhase(int32 AgentId, EAgentPhase From, EAgentPhase To);
	void OnArrivalRefused(EArrivalRefusal Why);
};
