#pragma once

#include "CoreMinimal.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsSave.h"
#include "FuelSupply.generated.h"

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
	/** Adds up to Litres, stopping at capacity; returns what was added. TAKE-OR-PAY: the caller has already paid. */
	double Receive(double Litres);
};
