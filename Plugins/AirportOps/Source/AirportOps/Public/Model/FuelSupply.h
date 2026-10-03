#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsSave.h"
#include "Model/Ledger.h"
#include "FuelSupply.generated.h"

/** Why a fuel order was refused. None is the only success. */
enum class EFuelOrderRefusal : uint8 { None, NoRoom, CannotAfford, UnknownTier, AlreadyContracted, NoContract };

/** The running contract: which tier, and how much of the term is left. Tier INDEX_NONE = none. Saved. */
USTRUCT()
struct AIRPORTOPS_API FFuelContract
{
	GENERATED_BODY()
	UPROPERTY() int32 Tier = INDEX_NONE;
	UPROPERTY() int32 DaysLeft = 0;
};

/** A spot order paid for and on its way. Saved: a load mid-delay must still deliver. */
USTRUCT()
struct AIRPORTOPS_API FFuelSpotOrder
{
	GENERATED_BODY()
	UPROPERTY() double Litres = 0.0;
	UPROPERTY() double DueAt = 0.0;
};

/**
 * The airport's fuel: what it holds, what it has contracted, what it has ordered (spec 2026-10-02-progression-and-
 * fuel-supply §7). Before this the depot held unlimited fuel for free.
 *
 * ONE POOL FOR THE AIRPORT, not one per depot - a named deviation from the spec's "the depot holds litres". Per-depot
 * stock would need a rule for which depot a delivery fills, and the player could not see it; every vehicle already
 * refills only at its own Home, so a shared pool changes nothing about where trucks drive.
 *
 * CAPACITY IS ASKED, NEVER STORED: CapacityOf reads the seated tanks every time (UJobBoard::FuelCapacityLitres via
 * UOpsRuntime). A stored capacity would be a second source of truth about what is built, and the mutator that forgot
 * it - a bulldozed depot, an undo - would leave a phantom tank.
 */
UCLASS()
class AIRPORTOPS_API UFuelSupply : public UObject, public IOpsPersistent
{
	GENERATED_BODY()

public:
	virtual FName SaveBlobName() const override { return TEXT("FuelSupply"); }
	virtual UObject& AsPersistentObject() override { return *this; }

	/** Litres held. May exceed Capacity() after a tank is sold - see FreeSpace. */
	UPROPERTY() double StockLitres = 0.0;

	/** Design figures, from the scenario. Transient - see UPricing for why figures are never saved. */
	UPROPERTY(Transient) FFuelSupplyFigures Figures;

	/** Seated tank capacity in litres. Unset: unbounded, for a test that does not care. */
	TFunction<double()> CapacityOf;

	double Capacity() const;
	double Available() const { return FMath::Max(StockLitres, 0.0); }
	/** Room for a delivery; zero, never negative, while the stock is above a capacity that shrank. */
	double FreeSpace() const { return FMath::Max(Capacity() - StockLitres, 0.0); }
	/** Below the half-litre a fuel job is judged done within - FFuelRolePolicy::FuelledWithinLitres, the ONE tolerance. */
	bool IsDry() const;

	/** Takes up to Litres from the stock; returns what was granted. */
	double Draw(double Litres);
	/** The running contract, if any. Saved. */
	UPROPERTY() FFuelContract Contract;
	/** Spot orders paid and not yet delivered. Saved. */
	UPROPERTY() TArray<FFuelSpotOrder> SpotOrders;

	/** The books fuel is paid from. Transient, set by the runtime; null (a bare test) buys for free. */
	UPROPERTY(Transient) TObjectPtr<ULedger> Ledger;
	/** Set and cleared with the runtime's other publishers (UOpsRuntime::Publishers). Null in a bare NewObject. */
	class FOpsEventBus* Bus = nullptr;

	double PendingSpotLitres() const;
	EFuelOrderRefusal JudgeSpot(double Litres) const;
	EFuelOrderRefusal OrderSpot(double Litres, double Now);
	int32 ReceiveDueSpot(double Now);

	EFuelOrderRefusal JudgeContract(int32 Tier) const;
	EFuelOrderRefusal SignContract(int32 Tier, double Now);
	EFuelOrderRefusal CancelContract(double Now);
	/** One day of the contract: charged whole (take-or-pay), added as far as the tanks allow. Called at day end. */
	void DeliverContractDay(double Now);

	/** Adds up to Litres, stopping at capacity; returns what was added. TAKE-OR-PAY: the caller has already paid. */
	double Receive(double Litres);
};
