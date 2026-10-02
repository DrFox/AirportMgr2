# Fuel Supply Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fuel stops being free. The airport holds a litre STOCK bounded by its tanks, buys fuel on a daily take-or-pay CONTRACT or as a dearer delayed SPOT order, and a flight whose fuel the stock cannot cover LEAVES WITHOUT IT.

**Architecture:** One new world-free model object, `UFuelSupply` (AirportOps `Model/`), owns the stock, the contract and the pending spot orders, and is persistent like `UPricing`. Capacity is DERIVED each time it is asked - seated Tank modules across live depots, through `UJobBoard::CapabilityOf` - never stored. The job board draws litres from the supply when a vehicle begins refilling; the bid prices the same limit through one extra `Available` argument on the two facility methods of `IServiceRolePolicy`. A dry depot is a new appended `EServiceRefusal::NoFuelStock`, re-opened when fuel arrives.

**Tech Stack:** UE 5.8 C++, AirportOps plugin (Model/Present), Airside plugin (`FDepotCapability`), automation tests in `AirportOpsTests`.

**Spec:** `docs/superpowers/specs/2026-10-02-progression-and-fuel-supply-design.md` §7 (rulings) and §9 (figures).

## Global Constraints

- Stock is ONE AIRPORT-WIDE POOL, not per depot. Named deviation from the spec's "the depot holds litres": deliveries would otherwise have to be split across depots by a rule the player cannot see, and every vehicle already refills only at its own `Home`. Capacity is the SUM of every live depot's seated tanks.
- Take-or-pay: contract litres that do not fit are PAID FOR and NOT ADDED.
- Dry depot: the flight leaves at its turnaround deadline with what it got. No wait, no new fine. (The existing airline `ShortfallPenalty` on an unfuelled departure is UNCHANGED - see Open questions.)
- Contract: fixed term in game days, cancellable for a charge.
- Spot: premium price, arrives after a game-time delay; a timer (no tanker, no landside road yet).
- Sell price stays `UPricing::FuelPricePerLitre` (2.0, PR #531). Buy prices from the pacing model: contract 0.9, spot 1.2 per litre.
- Design figures are `UPROPERTY(Transient)` on the model and authored on `UScenario` (rule 37 / `DesignFiguresAreNotSaved` pattern). State is `UPROPERTY()`.
- Any figure typed at two sites lives in `OpsDesignDefaults.h` (Check-Architecture rule 4).
- Every class with `FOpsEventBus* Bus` is listed in `UOpsRuntime::Publishers()` (rule 71).
- Enums are APPENDED, never inserted (`EServiceRefusal`, `ELedgerCategory`, `EAlertKind`, `EPurchaseRefusal`) - they are saved by value.
- Every task: worktree build `Build.bat ... -NoHotReloadFromIDE`, then `./Tools/Run-AirsideTests.ps1 -Project <slot>\AirportMgr.uproject -Filter AirportOps`. A new test `.cpp` needs TWO builds (memory: the first says Succeeded without compiling it) - this plan adds tests to existing files where it can.
- RoadNet plans ship real defects: each task's FIRST step is to open the named sites and confirm the quoted declarations still read as quoted. If one does not, stop and adapt; do not transcribe.

## Review Focus

1. **A truck mid-job that comes home to a dry depot** must not loop zero-second facility visits for ever; the job ends `NoFuelStock` with what was delivered kept, and the aircraft leaves at its deadline. (Task 4 test `DryMidJobEndsTheJob`.)
2. **Fuel arriving re-opens a `NoFuelStock` job** whose aircraft is still on stand - a refusal that never re-opens strands every flight after the first dry hour. (Task 4 test `DeliveryReopensStockRefusals`.)
3. **A save mid-contract and mid-spot-order** restores the stock, the contract's days left and the pending order, and the order still arrives (the clock queue is NOT saved). (Task 6 test `SpotOrderSurvivesASave`.)
4. **Selling or bulldozing tanks below the stock** - capacity drops under what is held. The stock is NOT destroyed (no free refund, no silent loss); it simply cannot be topped up until it falls below capacity. (Task 2 test `StockAboveCapacityIsKeptNotTopped`.)
5. **A dry depot must not stop offers arriving** - `CouldServe`/offer admission must not read stock; a dry airport still gets flights, which leave unfuelled. (Task 4 test `DryDepotStillAdmitsOffers`.)

---

## File Structure

| File | Responsibility |
|---|---|
| Create `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelSupply.h` / `Private/Model/FuelSupply.cpp` | `UFuelSupply`: stock, contract, spot orders, day-end delivery, refusals. World-free. |
| Modify `Public/Model/OpsDefinition.h` | `FFuelSupplyFigures` on `UScenario`; Tank `FModuleOffer`. |
| Modify `Public/Model/OpsDesignDefaults.h` | Shared default figures. |
| Modify `Plugins/Airside/Source/Airside/Public/Model/DepotCapability.h` | `Tanks()`. |
| Modify `Public/Model/ServiceRolePolicy.h` / `Private/Model/ServiceRolePolicy.cpp` | `Available` on `FacilitySeconds` / `CargoAfterFacility`. |
| Modify `Public/Model/JobBoard.h` / `Private/Model/JobBoard.cpp` / `JobBoardBid.cpp` / `ServiceBid.cpp` / `Public/Model/ServiceBid.h` | Draw on refill; bid sees stock; `NoFuelStock`; reopen. |
| Modify `Public/Model/ServiceVehicle.h` | `RefillLitres` (saved). |
| Modify `Public/Model/ServiceJob.h`, `Private/Model/ServiceText.cpp` | `EServiceRefusal::NoFuelStock` + text. |
| Modify `Public/Model/Ledger.h`, `Source/AirportMgr/LedgerViewModels.cpp` | `ELedgerCategory::FuelPurchase`. |
| Modify `Public/Model/OpsEventBus.h` / `.cpp` | `FFuelDeliveredEvent`. |
| Modify `Public/Present/OpsRuntime.h` / `Private/Present/OpsRuntime.cpp` | Create, wire, persist, day-end, re-arm, forwarders. |
| Modify `Public/Model/OpsAlerts.h` / `.cpp` | `EAlertKind::FuelLow`. |
| Modify `Source/AirportMgr/BuildActions.*`, `InspectorFacilityRows.*` | Depot card: fuel line, buy tank, spot order, contract tier. |
| Tests: `AirportOpsTests/Private/FuelSupplyTest.cpp` (new), `FuelServiceTest.cpp`, `FacilityPurchasesTest.cpp`, `OpsSaveTest.cpp`, `Source/AirportMgr/InspectorCardsTest.cpp`, `BuildActionsTest.cpp` | |

---

### Task 1: Figures - tank offer, supply figures, tank capacity

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDesignDefaults.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h` (UScenario, near `ModuleOffers` ~:251)
- Modify: `Plugins/Airside/Source/Airside/Public/Model/DepotCapability.h` (beside `Pumps()` ~:90)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FacilityPurchasesTest.cpp` (`ScenarioPricesTheCatalogue`)

**Interfaces:**
- Produces: `struct FFuelContractTier { double LitresPerDay; double PricePerLitre; }`, `struct FFuelSupplyFigures { double LitresPerTank; double StartingStockLitres; double SpotPricePerLitre; double SpotDelaySeconds; TArray<FFuelContractTier> ContractTiers; int32 ContractTermDays; double CancelFraction; }` as `UScenario::FuelSupply`; `int32 FDepotCapability::Tanks() const`; a Tank row in `UScenario::ModuleOffers`.

- [ ] **Step 1: Verify sites.** Open `OpsDefinition.h`; confirm `TMap<EDepotModule, FModuleOffer> ModuleOffers` holds only the Shed row and `FModuleOffer(double InPrice, double InUpkeep, int32 InSlots, FText InName, FText InPlural)`. Open `DepotCapability.h`; confirm `int32 Pumps() const { return bLegacyPlotless ? 1 : FMath::Max(SeatedOf(EDepotModule::Pump), 1); }`.

- [ ] **Step 2: Write the failing test.** In `ScenarioPricesTheCatalogue`, replace the lines asserting one offer and "a tank is not for sale":

```cpp
	// THE TANK JOINS THE SHED (spec 2026-10-02-progression-and-fuel-supply §7): storage is what bounds the stock and
	// gates the contract tiers. Pumps are still not for sale.
	TestEqual(TEXT("two module offers"), Scenario->ModuleOffers.Num(), 2);
	const FModuleOffer* Tank = Scenario->ModuleOffers.Find(EDepotModule::Tank);
	if (!TestNotNull(TEXT("a tank is offered"), Tank)) { return false; }
	TestEqual(TEXT("a tank costs 20,000"), Tank->Price, 20000.0, 1e-9);
	TestEqual(TEXT("and 100 a day to keep"), Tank->UpkeepPerDay, 100.0, 1e-9);
	TestEqual(TEXT("and grants no vehicle bay"), Tank->VehicleSlots, 0);
	TestNull(TEXT("nor a pump"), Scenario->ModuleOffers.Find(EDepotModule::Pump));

	const FFuelSupplyFigures& Fuel = Scenario->FuelSupply;
	TestEqual(TEXT("a tank holds 30,000 L"), Fuel.LitresPerTank, 30000.0, 1e-9);
	TestEqual(TEXT("a new game starts with one tank full"), Fuel.StartingStockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("spot fuel costs 1.2 a litre"), Fuel.SpotPricePerLitre, 1.2, 1e-9);
	TestEqual(TEXT("and arrives two game hours after the order"), Fuel.SpotDelaySeconds, 7200.0, 1e-9);
	TestEqual(TEXT("four contract tiers"), Fuel.ContractTiers.Num(), 4);
	TestEqual(TEXT("the smallest is 5,000 L a day"), Fuel.ContractTiers[0].LitresPerDay, 5000.0, 1e-9);
	TestEqual(TEXT("at 0.9 a litre"), Fuel.ContractTiers[0].PricePerLitre, 0.9, 1e-9);
	TestEqual(TEXT("a contract runs seven game days"), Fuel.ContractTermDays, 7);
	TestEqual(TEXT("cancelling owes half of what is left"), Fuel.CancelFraction, 0.5, 1e-9);
```

Also remove the old `TestNull(TEXT("a tank is not for sale"), ...)` line. If any other test asserts `ModuleOffers.Num() == 1` (grep `ModuleOffers.Num()` in `AirportOpsTests` and `Source/AirportMgr`), update it the same way.

- [ ] **Step 3: Run to see it fail.** Build; `Run-AirsideTests.ps1 -Filter AirportOps.Model.Facility.ScenarioPricesTheCatalogue`. Expected: compile error (`FFuelSupplyFigures` unknown).

- [ ] **Step 4: Implement.** In `OpsDesignDefaults.h`, inside the namespace:

```cpp
	/** Litres one Tank module holds (spec 2026-10-02 §7). A starter depot's one tank is a working morning's fuel at a
	 *  small field: the pacing model sold ~15,000-25,000 L a day at three stands (2026-10-02). */
	inline constexpr double LitresPerTank = 30000.0;
```

In `OpsDefinition.h`, ABOVE `UScenario` (beside `FModuleOffer`):

```cpp
/** One rung of the fuel contract (spec 2026-10-02-progression-and-fuel-supply §7): litres delivered each game day at a
 *  price per litre. A tier needs the tanks to hold one day's delivery - see UFuelSupply::JudgeContract. */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FFuelContractTier
{
	GENERATED_BODY()

	FFuelContractTier() = default;
	FFuelContractTier(double InLitres, double InPrice) : LitresPerDay(InLitres), PricePerLitre(InPrice) {}

	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double LitresPerDay = 0.0;
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double PricePerLitre = 0.0;
};

/**
 * What fuel costs the airport and how it arrives (spec 2026-10-02 §7). FIRST GUESSES from the pacing model
 * (Tools/pacing_model.py, 2026-10-02): contract 0.9, spot 1.2 against a 2.0 sell price. Larger tiers will sit on the
 * cargo milestone track when milestones exist; until then every tier is open to anyone with the tanks for it.
 */
USTRUCT(BlueprintType)
struct AIRPORTOPS_API FFuelSupplyFigures
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1.0")) double LitresPerTank = OpsDesignDefaults::LitresPerTank;
	/** A new game opens with the starter tank full, so the first flights can be fuelled before any order arrives. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double StartingStockLitres = OpsDesignDefaults::LitresPerTank;
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double SpotPricePerLitre = 1.2;
	/** GAME seconds from order to delivery. A timer until there is a landside road for a tanker to drive. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0")) double SpotDelaySeconds = 7200.0;
	UPROPERTY(EditAnywhere, Category = "Fuel") TArray<FFuelContractTier> ContractTiers = {
		FFuelContractTier(5000.0, 0.9), FFuelContractTier(10000.0, 0.9),
		FFuelContractTier(20000.0, 0.9), FFuelContractTier(40000.0, 0.9) };
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "1")) int32 ContractTermDays = 7;
	/** Cancelling owes this share of the days left at the tier's daily cost. */
	UPROPERTY(EditAnywhere, Category = "Fuel", meta = (ClampMin = "0.0", ClampMax = "1.0")) double CancelFraction = 0.5;
};
```

In `UScenario`, after `ModuleOffers`, add `UPROPERTY(EditAnywhere, Category = "Fuel") FFuelSupplyFigures FuelSupply;` with a one-line doc comment, and add the Tank row to `ModuleOffers`:

```cpp
		{ EDepotModule::Tank, FModuleOffer(20000.0, 100.0, 0, NSLOCTEXT("Scenario", "Tank", "Fuel tank"), NSLOCTEXT("Scenario", "Tanks", "Fuel tanks")) } };
```

Update the `ModuleOffers` doc comment: "THE SHED AND THE TANK (2026-10-02, spec ...§7); pumps are still not for sale."

In `DepotCapability.h`, beside `Pumps()`:

```cpp
	/** Seated tanks: what bounds the airport's fuel stock (spec 2026-10-02 §7). One for a plotless depot, by the same
	 *  legacy exemption Pumps() reads - a depot placed before modules existed has always had somewhere to keep fuel. */
	int32 Tanks() const { return bLegacyPlotless ? 1 : SeatedOf(EDepotModule::Tank); }
```

- [ ] **Step 5: Run.** Build; run `-Filter AirportOps.Model.Facility`. Expected: PASS. Then run `-Filter AirportOps` to catch upkeep/refund tests that change now a Tank has an offer (`RemoveUnseated` now refunds tanks, `DailyUpkeep` now charges them - risk 11). Fix such tests by DERIVING from `ModuleOffers[Tank]` as the shed tests already do, never by retyping figures.

- [ ] **Step 6: Commit.**

```bash
git add Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDesignDefaults.h Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h Plugins/Airside/Source/Airside/Public/Model/DepotCapability.h Plugins/AirportOps/Source/AirportOpsTests
git commit -m "feat(fuel): tank module for sale, supply figures on the scenario, FDepotCapability::Tanks"
```

---

### Task 2: `UFuelSupply` - stock, capacity, draw and receive

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/FuelSupply.h`
- Create: `Plugins/AirportOps/Source/AirportOps/Private/Model/FuelSupply.cpp`
- Create test: `Plugins/AirportOps/Source/AirportOpsTests/Private/FuelSupplyTest.cpp` (NEW FILE: needs TWO builds before its tests appear)

**Interfaces:**
- Consumes: `FFuelSupplyFigures` (Task 1).
- Produces:
  - `UFuelSupply : UObject, IOpsPersistent`, `SaveBlobName() == "FuelSupply"`
  - `UPROPERTY() double StockLitres`
  - `UPROPERTY(Transient) FFuelSupplyFigures Figures`
  - `TFunction<double()> CapacityOf` (unset: unbounded - a bare `NewObject` behaves as before this feature)
  - `double Capacity() const`, `double Available() const`, `double FreeSpace() const`
  - `double Draw(double Litres)` - returns litres granted
  - `double Receive(double Litres)` - returns litres added (the rest is lost)
  - `bool IsDry() const` - `Available() < FFuelRolePolicy::FuelledWithinLitres`

- [ ] **Step 1: Verify sites.** Confirm `IOpsPersistent` in `OpsSave.h` (`SaveBlobName`, `AsPersistentObject`, `OnAfterRestore(int32)`), and `UPricing`'s pattern (`Pricing.h:32`).

- [ ] **Step 2: Write the failing tests** in the new `FuelSupplyTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FuelSupply.h"
#include "Model/ServiceRolePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

// FUEL SUPPLY (spec 2026-10-02-progression-and-fuel-supply §7): world-free, on a NewObject supply whose capacity is a
// number the test sets - the tanks' arithmetic is UJobBoard's and is tested there (Task 3).

namespace
{
	UFuelSupply* SupplyHolding(double Stock, double Capacity)
	{
		UFuelSupply* Supply = NewObject<UFuelSupply>(GetTransientPackage());
		Supply->CapacityOf = [Capacity]() { return Capacity; };
		Supply->StockLitres = Stock;
		return Supply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyDrawTest, "AirportOps.Model.FuelSupply.DrawGrantsWhatIsHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyDrawTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyHolding(1000.0, 30000.0);
	TestEqual(TEXT("a draw within the stock is granted whole"), Supply->Draw(600.0), 600.0, 1e-9);
	TestEqual(TEXT("and leaves the rest"), Supply->StockLitres, 400.0, 1e-9);
	TestEqual(TEXT("a draw beyond it is granted only what is there"), Supply->Draw(600.0), 400.0, 1e-9);
	TestEqual(TEXT("leaving nothing - never a negative stock"), Supply->StockLitres, 0.0, 1e-9);
	TestTrue(TEXT("and the supply is dry"), Supply->IsDry());
	TestEqual(TEXT("a negative draw grants nothing and adds nothing"), Supply->Draw(-50.0), 0.0, 1e-9);
	TestEqual(TEXT("CONTROL: the stock did not move"), Supply->StockLitres, 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyReceiveTest, "AirportOps.Model.FuelSupply.ReceiveStopsAtCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyReceiveTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyHolding(25000.0, 30000.0);
	TestEqual(TEXT("TAKE-OR-PAY: only what fits is added"), Supply->Receive(10000.0), 5000.0, 1e-9);
	TestEqual(TEXT("and the tanks are full"), Supply->StockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("a full supply has no free space"), Supply->FreeSpace(), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyOverCapacityTest, "AirportOps.Model.FuelSupply.StockAboveCapacityIsKeptNotTopped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyOverCapacityTest::RunTest(const FString&)
{
	// A TANK SOLD UNDER THE STOCK (review focus 4): the fuel is not destroyed and not refunded - it is held, and nothing
	// more is taken until it is drawn below the new capacity.
	UFuelSupply* Supply = SupplyHolding(50000.0, 30000.0);
	TestEqual(TEXT("free space is never negative"), Supply->FreeSpace(), 0.0, 1e-9);
	TestEqual(TEXT("a delivery adds nothing"), Supply->Receive(5000.0), 0.0, 1e-9);
	TestEqual(TEXT("and the held stock is untouched"), Supply->StockLitres, 50000.0, 1e-9);
	TestEqual(TEXT("it can still all be drawn"), Supply->Draw(50000.0), 50000.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyUnwiredTest, "AirportOps.Model.FuelSupply.UnwiredCapacityIsUnbounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyUnwiredTest::RunTest(const FString&)
{
	// A BARE NewObject (every pre-existing fuel test's board) must behave as fuel did before stock existed only when
	// the board has NO supply at all - see UJobBoard::FuelSupply. A supply with no capacity reader holds anything.
	UFuelSupply* Supply = NewObject<UFuelSupply>(GetTransientPackage());
	TestTrue(TEXT("no capacity reader: capacity unbounded"), Supply->Capacity() > 1e12);
	TestEqual(TEXT("so a delivery is taken whole"), Supply->Receive(1e6), 1e6, 1e-6);
	return true;
}

#endif
```

- [ ] **Step 3: Build twice, run.** `-Filter AirportOps.Model.FuelSupply`. Expected: compile error, `FuelSupply.h` missing.

- [ ] **Step 4: Implement** `FuelSupply.h`:

```cpp
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
```

`FuelSupply.cpp`:

```cpp
#include "Model/FuelSupply.h"

#include "Model/ServiceRolePolicy.h"

double UFuelSupply::Capacity() const
{
	return CapacityOf ? FMath::Max(CapacityOf(), 0.0) : TNumericLimits<double>::Max();
}

bool UFuelSupply::IsDry() const
{
	return Available() < FFuelRolePolicy::FuelledWithinLitres;
}

double UFuelSupply::Draw(double Litres)
{
	const double Granted = FMath::Clamp(Litres, 0.0, Available());
	StockLitres -= Granted;
	return Granted;
}

double UFuelSupply::Receive(double Litres)
{
	const double Added = FMath::Clamp(Litres, 0.0, FreeSpace());
	StockLitres += Added;
	return Added;
}
```

- [ ] **Step 5: Run.** Expected: 4 PASS.

- [ ] **Step 6: Commit.** `git commit -m "feat(fuel): UFuelSupply - airport-wide stock bounded by asked capacity"`

---

### Task 3: Refills draw from the stock; the bid prices the same limit

**Files:**
- Modify: `Public/Model/ServiceRolePolicy.h` (:68, :71, FFuelRolePolicy overrides), `Private/Model/ServiceRolePolicy.cpp` (:36-48)
- Modify: `Public/Model/ServiceVehicle.h` (beside `UPROPERTY() double Cargo` :191)
- Modify: `Public/Model/JobBoard.h` (beside `ModuleCeilingOf` :197), `Private/Model/JobBoard.cpp` (`BeginFacility` :462, `Step` :929)
- Modify: `Public/Model/ServiceBid.h` (`FInput`, :34-50), `Private/Model/ServiceBid.cpp` (:47-48), `Private/Model/JobBoardBid.cpp` (:321, :352-356)
- Test: `AirportOpsTests/Private/FuelServiceTest.cpp`

**Interfaces:**
- Consumes: `UFuelSupply` (Task 2), `FDepotCapability::Tanks()` (Task 1).
- Produces:
  - `virtual double FacilitySeconds(double Cargo, const FServiceVehicleType& Type, int32 Pumps, double Available) const`
  - `virtual double CargoAfterFacility(double Cargo, const FServiceVehicleType& Type, double Available) const`
  - `UPROPERTY() double FServiceVehicle::RefillLitres`
  - `UPROPERTY(Transient) TObjectPtr<UFuelSupply> UJobBoard::FuelSupply` (null: unlimited, as before)
  - `double UJobBoard::FuelAvailable() const`
  - `double UJobBoard::FuelCapacityLitres(const URoadNetwork& Network) const`
  - `double FInput::FacilityAvailable`

- [ ] **Step 1: Verify sites.** The four call sites of `FacilitySeconds`/`CargoAfterFacility` must be exactly: `JobBoard.cpp` BeginFacility (~:466) and Step (~:929), `JobBoardBid.cpp` (~:352, :355-356), `ServiceBid.cpp` (~:47-48). `grep -rn "FacilitySeconds\|CargoAfterFacility" Plugins Source` - if there are more (e.g. a non-fuel policy or a test double), each gets the new argument.

- [ ] **Step 2: Write the failing tests** in `FuelServiceTest.cpp`, using `FFuelFixture` (`:84`). Add to the fixture a `UFuelSupply* Supply = nullptr;` and in `Build`, when a new flag `double SupplyLitres = -1.0` is `>= 0`, create the supply, set `Supply->StockLitres = SupplyLitres`, leave `CapacityOf` unset, and assign `Service->FuelSupply = Supply`.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelRefillDrawsStockTest, "AirportOps.Fuel.RefillDrawsTheStock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelRefillDrawsStockTest::RunTest(const FString&)
{
	// THE FREE-FUEL LINE GOES (spec 2026-10-02 §7): a returning truck takes what the airport holds, and the airport
	// holds that much less.
	FFuelFixture F;
	F.SupplyLitres = 100000.0;
	F.Build(true);
	F.ParkAircraft();
	F.AdvanceUntil([&]() { return F.Service->GetJobs().Num() > 0 && F.Service->GetJobs()[0].State == EServiceJobState::Done; }, 4000.0);
	const FServiceVehicle& Truck = F.Service->GetVehicles()[0];
	F.AdvanceUntil([&]() { return Truck.State == EServiceVehicleState::Idle; }, 4000.0);
	TestTrue(FString::Printf(TEXT("the stock fell by what the truck took back on (stock %.0f)"), F.Supply->StockLitres),
		F.Supply->StockLitres < 100000.0);
	TestEqual(TEXT("and the truck is full again"), Truck.Cargo, FFuelRolePolicy::CapacityOf(F.Service->TypeFor(Truck.TypeCode)), 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelRefillLimitedTest, "AirportOps.Fuel.RefillIsLimitedByTheStock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelRefillLimitedTest::RunTest(const FString&)
{
	FFuelFixture F;
	F.SupplyLitres = 120.0;
	F.Build(true);
	F.ParkAircraft();
	const FServiceVehicle& Truck = F.Service->GetVehicles()[0];
	F.AdvanceUntil([&]() { return Truck.State == EServiceVehicleState::AtFacility; }, 4000.0);
	F.AdvanceUntil([&]() { return Truck.State != EServiceVehicleState::AtFacility; }, 4000.0);
	TestEqual(TEXT("the airport gave all it had"), F.Supply->StockLitres, 0.0, 1e-9);
	TestTrue(FString::Printf(TEXT("and the truck carries no more than it had plus 120 (%.1f)"), Truck.Cargo),
		Truck.Cargo <= F.CargoBeforeRefill + 120.0 + 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelPolicyAvailableTest, "AirportOps.Service.Policy.FacilityHonoursAvailable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelPolicyAvailableTest::RunTest(const FString&)
{
	// ONE RULE FOR THE TRUCK AND THE BID: both ask these two, so a bid can never promise a refill the stock cannot give.
	FFuelRolePolicy Policy;
	Policy.RefillLitresPerMinutePerPump = 100.0;
	FServiceVehicleType Type;
	Type.Capacity = 1000.0;
	TestEqual(TEXT("plenty: refilled to the brim"), Policy.CargoAfterFacility(200.0, Type, 1e9), 1000.0, 1e-9);
	TestEqual(TEXT("short: refilled by what is there"), Policy.CargoAfterFacility(200.0, Type, 300.0), 500.0, 1e-9);
	TestEqual(TEXT("dry: nothing added"), Policy.CargoAfterFacility(200.0, Type, 0.0), 200.0, 1e-9);
	TestEqual(TEXT("pumping time is the litres actually pumped"), Policy.FacilitySeconds(200.0, Type, 1, 300.0), 180.0, 1e-9);
	TestEqual(TEXT("CONTROL: unbounded is the old time"), Policy.FacilitySeconds(200.0, Type, 1, 1e9), 480.0, 1e-9);
	return true;
}
```

`RefillIsLimitedByTheStock` reads `F.CargoBeforeRefill`: add `double CargoBeforeRefill = 0.0;` to the fixture and, in the test, capture `F.CargoBeforeRefill = Truck.Cargo;` right after the first `AdvanceUntil` (the truck has just arrived at the facility). Check `TypeFor` is public on `UJobBoard`; if not, read the capacity from `F.Service->GetCatalogue()` the way existing tests in this file do.

- [ ] **Step 3: Run to see them fail.** `-Filter AirportOps.Fuel` and `-Filter AirportOps.Service.Policy`. Expected: compile errors on the new signatures and fields.

- [ ] **Step 4: Implement the policy.** In `ServiceRolePolicy.h` change both pure virtuals and the two overrides:

```cpp
	/** GAME seconds at the facility, arriving with Cargo, at a facility with Pumps service points that can give at most
	 *  Available of what the role carries (fuel: the airport's stock, spec 2026-10-02 §7). */
	virtual double FacilitySeconds(double Cargo, const FServiceVehicleType& Type, int32 Pumps, double Available) const = 0;

	/** What the vehicle carries leaving the facility, given at most Available. */
	virtual double CargoAfterFacility(double Cargo, const FServiceVehicleType& Type, double Available) const = 0;
```

In `ServiceRolePolicy.cpp`:

```cpp
double FFuelRolePolicy::FacilitySeconds(double Cargo, const FServiceVehicleType& Type, int32 Pumps, double Available) const
{
	// WHAT IT PUMPED OUT, back in at the depot's pumps - and since 2026-10-02 no more than the airport holds. A full tank
	// or a dry airport takes no time, which is what lets a vehicle that went home without serving be free at once.
	const double Pumped = FMath::Min(FMath::Max(CapacityOf(Type) - Cargo, 0.0), FMath::Max(Available, 0.0));
	return Pumped / (FMath::Max(Pumps, 1) * FMath::Max(RefillLitresPerMinutePerPump, 1.0)) * 60.0;
}

double FFuelRolePolicy::CargoAfterFacility(double Cargo, const FServiceVehicleType& Type, double Available) const
{
	// WAS CapacityOf(Type), unconditionally - the line that made fuel free (spec 2026-10-02 §7).
	return FMath::Min(CapacityOf(Type), Cargo + FMath::Max(Available, 0.0));
}
```

- [ ] **Step 5: Implement the board.** In `ServiceVehicle.h`, after `Cargo`:

```cpp
	/** Litres granted from the airport's stock when this refill began - what CargoAfterFacility adds when it ends. SAVED:
	 *  a save mid-refill must not grant the litres twice. Zero outside AtFacility. */
	UPROPERTY() double RefillLitres = 0.0;
```

In `JobBoard.h`, beside `ModuleCeilingOf`:

```cpp
	/** The airport's fuel stock (spec 2026-10-02 §7). NULL means unlimited - every board built before fuel had a cost,
	 *  and every test that is not about stock. UOpsRuntime sets it. */
	UPROPERTY(Transient) TObjectPtr<UFuelSupply> FuelSupply;

	/** What a refill may take now: the stock, or unbounded when there is no supply. */
	double FuelAvailable() const;

	/** Seated tanks across every live depot, in litres (FDepotCapability::Tanks x PerTank). UOpsRuntime wires
	 *  UFuelSupply::CapacityOf to this. */
	double FuelCapacityLitres(const URoadNetwork& Network, double LitresPerTank) const;
```

Forward-declare `class UFuelSupply;` at the top of `JobBoard.h`. In `JobBoard.cpp` include `Model/FuelSupply.h` and add:

```cpp
double UJobBoard::FuelAvailable() const
{
	return FuelSupply != nullptr ? FuelSupply->Available() : TNumericLimits<double>::Max();
}

double UJobBoard::FuelCapacityLitres(const URoadNetwork& Network, double LitresPerTank) const
{
	// THE SAME LIVE-DEPOT WALK UFacilityPurchases::DailyUpkeep makes (FacilityPurchases.cpp ~:350), so a depot that
	// pays upkeep for a tank is the depot whose tank holds fuel.
	int32 Tanks = 0;
	for (int32 Index = 0; Index < Network.GetEntities().Num(); ++Index)
	{
		const FEntityInstance& Entity = Network.GetEntities()[Index];
		if (Entity.bAlive && Entity.IsDepot())
		{
			Tanks += CapabilityOf(FEntityInstanceId(Index), Entity).Tanks();
		}
	}
	return Tanks * FMath::Max(LitresPerTank, 0.0);
}
```

Check how `FacilityPurchases.cpp:350` builds an `FEntityInstanceId` from an index and copy that exactly (the constructor above is an assumption).

Replace `BeginFacility`'s seconds line and log:

```cpp
	// DRAWN NOW, not when the pumping ends: two trucks home together must not both be promised the last 500 L. What is
	// granted is held on the vehicle (RefillLitres) and added when the refill ends.
	const double Missing = Policy != nullptr ? FMath::Max(FFuelRolePolicy::CapacityOf(TypeFor(Vehicle.TypeCode)) - Vehicle.Cargo, 0.0) : 0.0;
	Vehicle.RefillLitres = FuelSupply != nullptr ? FuelSupply->Draw(Missing) : Missing;
	const double Seconds = Policy != nullptr && Home != nullptr
		? Policy->FacilitySeconds(Vehicle.Cargo, TypeFor(Vehicle.TypeCode), PumpsAt(Vehicle.Home, *Home), Vehicle.RefillLitres) : 0.0;
```

and change the log's litres argument to `Vehicle.RefillLitres`. Keep the log line - add `, stock left %.0f L` with `FuelSupply != nullptr ? FuelSupply->Available() : -1.0`.

In `Step`'s AtFacility branch:

```cpp
				Vehicle.Cargo = Policy->CargoAfterFacility(Vehicle.Cargo, TypeFor(Vehicle.TypeCode), Vehicle.RefillLitres);
			}
			Vehicle.RefillLitres = 0.0;
```

- [ ] **Step 6: Implement the bid.** In `ServiceBid.h` `FInput`, after `Pumps`:

```cpp
		/** What its facility can give right now (fuel: the airport's stock). A SNAPSHOT: two vehicles bidding at once
		 *  are both priced against the whole stock, and the one that refills second may get less than it was priced for.
		 *  Accepted - the bid is a ranking, re-run every decision, and the live draw (UJobBoard::BeginFacility) is exact. */
		double FacilityAvailable = TNumericLimits<double>::Max();
```

In `JobBoardBid.cpp` after `In.Pumps = PumpsAt(...)` (:321): `In.FacilityAvailable = FuelAvailable();`. At :352: `CargoAfterFacility(Vehicle.Cargo, Type, Vehicle.RefillLitres)` (AtFacility: the litres already granted). At :355-356: `FacilitySeconds(Vehicle.Cargo, Type, In.Pumps, In.FacilityAvailable)` and `CargoAfterFacility(Vehicle.Cargo, Type, In.FacilityAvailable)`. In `ServiceBid.cpp` :47-48: the same `In.FacilityAvailable` on both, and decrement a local copy as the simulation refills so a multi-trip bid does not draw the same litres twice:

```cpp
			const double Before = Cargo;
			Time += Policy.FacilitySeconds(Cargo, Type, In.Pumps, Available);
			Cargo = Policy.CargoAfterFacility(Cargo, Type, Available);
			Available = FMath::Max(Available - (Cargo - Before), 0.0);
```

with `double Available = In.FacilityAvailable;` declared beside `Cargo` at the top of the simulation.

- [ ] **Step 7: Run.** `-Filter AirportOps.Fuel`, `-Filter AirportOps.Service`, then `-Filter AirportOps`. Expected: all PASS - every pre-existing fuel test has no supply, so `FuelAvailable()` is unbounded and nothing they measure changes.

- [ ] **Step 8: Commit.** `git commit -m "feat(fuel): refills draw the airport's stock; bid and truck share one Available rule"`

---

### Task 4: A dry depot - `NoFuelStock`, the flight leaves, delivery re-opens

**Files:**
- Modify: `Public/Model/ServiceJob.h` (`EServiceRefusal` :82, APPEND), `Private/Model/ServiceText.cpp` (:60 table)
- Modify: `Private/Model/JobBoardBid.cpp` (bid refusal; ~:506 Unserviceable), `Private/Model/JobBoard.cpp` (`Step` AtFacility branch, new `ReopenStockRefusals`), `Public/Model/JobBoard.h`
- Test: `AirportOpsTests/Private/FuelServiceTest.cpp`

**Interfaces:**
- Produces: `EServiceRefusal::NoFuelStock`; `int32 UJobBoard::ReopenStockRefusals()` (returns jobs re-opened).

- [ ] **Step 1: Verify sites.** `EServiceRefusal` ends `..., NoVehicles, UnknownVehicleKind`; `RefusalOf(Judged)` and the judged struct in `JobBoardBid.cpp` (~:150-230); `FTurnarounds::IsBeingServed` treats `Unserviceable` as not serving (`Turnarounds.cpp:256`); offer admission `CouldServe` (`OfferInbox.cpp:112`) does not read the board's stock.

- [ ] **Step 2: Write the failing tests.**

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDryLeavesTest, "AirportOps.Fuel.DryDepotFlightLeavesUnfuelled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDryLeavesTest::RunTest(const FString&)
{
	// THE RULING (spec 2026-10-02 §7): no stock, no wait - the job is refused NoFuelStock and the aircraft goes at its
	// deadline with nothing, earning no fuel fee.
	FFuelFixture F;
	F.SupplyLitres = 0.0;
	F.bEmptyDepot = true;   // the truck starts with nothing either
	F.Build(true);
	F.ParkAircraft();
	F.Advance(60.0);
	const FServiceJob& Job = F.Service->GetJobs()[0];
	TestEqual(TEXT("the fuel job is unserviceable"), Job.State, EServiceJobState::Unserviceable);
	TestEqual(TEXT("for want of stock"), Job.Why, EServiceRefusal::NoFuelStock);
	TestEqual(TEXT("and nothing was delivered"), Job.QuantityDelivered, 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelDryMidJobTest, "AirportOps.Fuel.DryMidJobEndsTheJob",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelDryMidJobTest::RunTest(const FString&)
{
	// REVIEW FOCUS 1: a truck that delivers what it carried, goes home for more and finds none must not loop zero-second
	// refills for ever. The job ends NoFuelStock with its delivery kept.
	FFuelFixture F;
	F.SupplyLitres = 0.0;
	F.FixtureLitres = 30000.0;   // more than one tank-load, so a second trip is owed
	F.Build(true);
	F.ParkAircraft();
	const FServiceJob* Job = nullptr;
	F.AdvanceUntil([&]() { Job = &F.Service->GetJobs()[0]; return Job->State == EServiceJobState::Unserviceable; }, 20000.0);
	TestEqual(TEXT("the job ended for want of stock"), Job->Why, EServiceRefusal::NoFuelStock);
	TestTrue(FString::Printf(TEXT("and kept what the first trip delivered (%.0f L)"), Job->QuantityDelivered), Job->QuantityDelivered > 0.0);
	TestEqual(TEXT("the truck is idle, not cycling the depot"), F.Service->GetVehicles()[0].State, EServiceVehicleState::Idle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelReopenTest, "AirportOps.Fuel.DeliveryReopensStockRefusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelReopenTest::RunTest(const FString&)
{
	// REVIEW FOCUS 2: NoFuelStock is NOT terminal the way a road refusal is - stock arriving changes no graph revision,
	// so the board is told, and an aircraft still on stand is served.
	FFuelFixture F;
	F.SupplyLitres = 0.0;
	F.bEmptyDepot = true;
	F.Build(true);
	F.ParkAircraft();
	F.Advance(60.0);
	F.Supply->Receive(100000.0);
	TestEqual(TEXT("one refused job re-opened"), F.Service->ReopenStockRefusals(), 1);
	F.AdvanceUntil([&]() { return F.Service->GetJobs()[0].State == EServiceJobState::Done; }, 8000.0);
	TestEqual(TEXT("and it was served"), F.Service->GetJobs()[0].State, EServiceJobState::Done);
	TestEqual(TEXT("CONTROL: a second call re-opens nothing"), F.Service->ReopenStockRefusals(), 0);
	return true;
}
```

Also add `DryDepotStillAdmitsOffers` beside the existing offer-admission tests (find the test that drives `CouldServe`/`bFuelServable` in `OfferInboxTest.cpp` or equivalent; give its board a supply with `StockLitres = 0` and assert the offer is still admitted - review focus 5). If `bEmptyDepot` or `FixtureLitres` do not do what these tests need, read their use in `FFuelFixture::Build` and adapt the setup, keeping each test's stated condition.

- [ ] **Step 3: Run to see them fail.** Expected: compile error on `NoFuelStock` / `ReopenStockRefusals`.

- [ ] **Step 4: Implement.** Append `NoFuelStock` after `UnknownVehicleKind` in `EServiceRefusal`, with the comment `/** The airport holds no fuel to refill with (spec 2026-10-02 §7). Re-opened by UJobBoard::ReopenStockRefusals when fuel arrives, not by a graph change. */`. Add to `ServiceText.cpp`'s switch: `case EServiceRefusal::NoFuelStock: return TEXT("the airport has no fuel in its tanks - order spot fuel or sign a contract");`.

In the bid simulation (`ServiceBid.cpp` `OneTrip`), a facility visit that would grant less than `Policy.DoneWithin()` (`Available` below it) is NOT a failure - PARTIAL SERVICE BEATS NONE: the simulation stops there and the job counts as finished with what was delivered so far (break out of the trip loop as the "job done" branch does, keeping `Time`). Only a vehicle that would deliver NOTHING - it needs the facility first, before any trip, and `Available < DoneWithin()` - fails its bid (`OneTrip` returns `-1.0`). Record that case in the judged struct so `RefusalOf(Judged)` returns `NoFuelStock` when every vehicle failed for that reason alone. This is what lets `DryMidJobEndsTheJob` deliver its first tank-load: the bid wins on the truck's cargo, and the live mid-job rule below ends the job when the truck comes home to nothing. Follow how `VehicleTooLarge` is recorded and preferred in `RefusalOf` - add `bool bNoStock = false;` beside it and give `NoFuelStock` LOWER priority than every road/vehicle refusal (a dry airport with no road is a road problem first).

In `Step`'s AtFacility branch, after the refill: if `Vehicle.Role == EServiceRole::Fuel`, `Vehicle.RefillLitres` was below `DoneWithin()`, and the vehicle's cargo is below `DoneWithin()` and it has a current job, release it - set the job `Unserviceable`, `Why = NoFuelStock`, `++RevisionCount`, clear the vehicle's current job and queue the same way the existing Unserviceable path at `JobBoardBid.cpp:~506` and the vehicle withdraw path do (read `Lifecycle(Vehicle)` for the method that drops a job), and log:

```cpp
UE_LOG(LogAirportOps, Log, TEXT("Fuel: depot dry - vehicle %d gives up job %d after %.0f L"), Vehicle.Id, Job->Id, Job->QuantityDelivered);
```

Add to `JobBoard.h`:

```cpp
	/** Re-opens every Unserviceable job refused NoFuelStock (spec 2026-10-02 §7) and returns how many. Fuel arriving
	 *  changes no graph revision, so the refusal's usual re-judge never fires; UOpsRuntime calls this on FFuelDeliveredEvent. */
	int32 ReopenStockRefusals();
```

Implement by walking jobs, setting `State = EServiceJobState::Open`, `Why = EServiceRefusal::None`, `++RevisionCount` per job, and logging `Fuel: %d job(s) re-opened by a delivery` once when the count is > 0.

- [ ] **Step 5: Run.** `-Filter AirportOps.Fuel`, then `-Filter AirportOps`. Expected: PASS. Also confirm the departure: in `DryDepotFlightLeavesUnfuelled`, advance past the turnaround and assert the `FTurnaroundRecorder` (`:360`) saw `EFuelOutcome::Unfuelled`.

- [ ] **Step 6: Commit.** `git commit -m "feat(fuel): a dry airport refuses NoFuelStock; the flight leaves; delivery re-opens"`

---

### Task 5: Buying fuel - contract and spot, on the ledger

**Files:**
- Modify: `Public/Model/FuelSupply.h`, `Private/Model/FuelSupply.cpp`
- Modify: `Public/Model/Ledger.h` (`ELedgerCategory` :22, APPEND), `Source/AirportMgr/LedgerViewModels.cpp` (`WordFor` :20-35)
- Modify: `Public/Model/OpsEventBus.h` (event + `FOpsEvent` variant :518), `Private/Model/OpsEventBus.cpp` (`Describe`)
- Test: `AirportOpsTests/Private/FuelSupplyTest.cpp`

**Interfaces:**
- Consumes: `ULedger::Post(double At, ELedgerCategory, double Amount, FText What)` (Ledger.h:151), `ULedger` balance accessor (verify its name, e.g. `Balance()`).
- Produces:
  - `ELedgerCategory::FuelPurchase`
  - `enum class EFuelOrderRefusal : uint8 { None, NoRoom, CannotAfford, UnknownTier, AlreadyContracted, NoContract }`
  - `USTRUCT FFuelContract { int32 Tier = INDEX_NONE; int32 DaysLeft = 0; }` (saved)
  - `USTRUCT FFuelSpotOrder { double Litres; double DueAt; }` (saved)
  - `UPROPERTY() FFuelContract Contract; UPROPERTY() TArray<FFuelSpotOrder> SpotOrders;`
  - `UPROPERTY(Transient) TObjectPtr<ULedger> Ledger; FOpsEventBus* Bus = nullptr;`
  - `EFuelOrderRefusal JudgeSpot(double Litres) const; EFuelOrderRefusal OrderSpot(double Litres, double Now);`
  - `EFuelOrderRefusal JudgeContract(int32 Tier) const; EFuelOrderRefusal SignContract(int32 Tier, double Now); EFuelOrderRefusal CancelContract(double Now);`
  - `void DeliverContractDay(double Now);` (called at day end)
  - `int32 ReceiveDueSpot(double Now);` (returns orders delivered)
  - `double PendingSpotLitres() const;`
  - `struct FFuelDeliveredEvent { double Litres; double Added; bool bContract; }`

- [ ] **Step 1: Verify sites.** `ELedgerCategory` ends `..., BroughtForward, Fleet`; `WordFor` is a switch with no default; how `ULedger` exposes the balance; `FDayEndedEvent` at `OpsEventBus.h:173`; the `FOpsEvent` `TVariant` list.

- [ ] **Step 2: Write the failing tests.** Add a helper that gives the supply a `ULedger` opened with a balance and a `USimClock`-free `Now`:

```cpp
namespace
{
	UFuelSupply* SupplyWithLedger(double Stock, double Capacity, double Balance)
	{
		UFuelSupply* Supply = SupplyHolding(Stock, Capacity);
		Supply->Figures = FFuelSupplyFigures();
		Supply->Ledger = NewObject<ULedger>(GetTransientPackage());
		Supply->Ledger->Open(Balance);
		return Supply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSpotTest, "AirportOps.Model.FuelSupply.SpotPaysNowArrivesLater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSpotTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 100000.0);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	TestEqual(TEXT("an order that fits is taken"), Supply->OrderSpot(10000.0, 0.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("paid at once, at the spot price"), Supply->Ledger->Balance(), 100000.0 - 10000.0 * Fig.SpotPricePerLitre, 1e-6);
	TestEqual(TEXT("nothing in the tanks yet"), Supply->StockLitres, 0.0, 1e-9);
	TestEqual(TEXT("an hour early, nothing arrives"), Supply->ReceiveDueSpot(Fig.SpotDelaySeconds - 3600.0), 0);
	TestEqual(TEXT("at the delay, it does"), Supply->ReceiveDueSpot(Fig.SpotDelaySeconds), 1);
	TestEqual(TEXT("and fills the tanks"), Supply->StockLitres, 10000.0, 1e-9);
	TestEqual(TEXT("an order bigger than the room left (counting orders on the way) is refused"),
		Supply->OrderSpot(25000.0, Fig.SpotDelaySeconds), EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("CONTROL: and charged nothing"), Supply->Ledger->Balance(), 100000.0 - 10000.0 * Fig.SpotPricePerLitre, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelContractTest, "AirportOps.Model.FuelSupply.ContractIsTakeOrPay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelContractTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyWithLedger(28000.0, 30000.0, 100000.0);
	const FFuelContractTier Tier = Supply->Figures.ContractTiers[0];
	TestEqual(TEXT("the smallest tier is signed"), Supply->SignContract(0, 0.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("signing costs nothing - each day is paid on delivery"), Supply->Ledger->Balance(), 100000.0, 1e-6);
	Supply->DeliverContractDay(86400.0);
	TestEqual(TEXT("TAKE-OR-PAY: the whole day is charged"), Supply->Ledger->Balance(), 100000.0 - Tier.LitresPerDay * Tier.PricePerLitre, 1e-6);
	TestEqual(TEXT("but only what fitted was added"), Supply->StockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("a day of the term is used"), Supply->Contract.DaysLeft, Supply->Figures.ContractTermDays - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelContractEndsTest, "AirportOps.Model.FuelSupply.ContractEndsAndCancels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelContractEndsTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyWithLedger(0.0, 1e9, 1e9);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	Supply->SignContract(1, 0.0);
	for (int32 Day = 1; Day <= Fig.ContractTermDays; ++Day) { Supply->DeliverContractDay(Day * 86400.0); }
	TestEqual(TEXT("at the end of its term the contract is over"), Supply->Contract.Tier, INDEX_NONE);
	const double Stock = Supply->StockLitres;
	Supply->DeliverContractDay((Fig.ContractTermDays + 1) * 86400.0);
	TestEqual(TEXT("and delivers nothing after"), Supply->StockLitres, Stock, 1e-9);

	Supply->SignContract(0, 0.0);
	Supply->DeliverContractDay(86400.0);
	const double Before = Supply->Ledger->Balance();
	TestEqual(TEXT("a running contract may be cancelled"), Supply->CancelContract(86400.0), EFuelOrderRefusal::None);
	const FFuelContractTier& T0 = Fig.ContractTiers[0];
	TestEqual(TEXT("for CancelFraction of the days left at the daily cost"), Before - Supply->Ledger->Balance(),
		(Fig.ContractTermDays - 1) * T0.LitresPerDay * T0.PricePerLitre * Fig.CancelFraction, 1e-6);
	TestEqual(TEXT("and nothing is left to cancel"), Supply->CancelContract(86400.0), EFuelOrderRefusal::NoContract);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelContractTierTest, "AirportOps.Model.FuelSupply.TierNeedsTheTanksForADay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelContractTierTest::RunTest(const FString&)
{
	// STORAGE GATES THE TIERS (spec §7): a tier whose daily delivery the tanks cannot hold is refused - it would pour
	// fuel away from the first day.
	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 1e9);
	TestEqual(TEXT("20,000 L a day fits 30,000 L of tanks"), Supply->JudgeContract(2), EFuelOrderRefusal::None);
	TestEqual(TEXT("40,000 does not"), Supply->JudgeContract(3), EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("there is no fifth tier"), Supply->JudgeContract(4), EFuelOrderRefusal::UnknownTier);
	Supply->SignContract(0, 0.0);
	TestEqual(TEXT("one contract at a time"), Supply->JudgeContract(1), EFuelOrderRefusal::AlreadyContracted);
	return true;
}
```

Add `#include "Model/Ledger.h"` to the test file. Replace `Balance()` with the verified accessor name.

- [ ] **Step 3: Run to see them fail.**

- [ ] **Step 4: Implement.** Append `FuelPurchase` to `ELedgerCategory` (comment: "Fuel bought - contract days, spot orders and contract cancellations (spec 2026-10-02 §7). Appended."). Add `case ELedgerCategory::FuelPurchase: return LOCTEXT("CatFuelPurchase", "Fuel bought");` to `WordFor`.

Event in `OpsEventBus.h` beside `FDayEndedEvent`, appended to the `FOpsEvent` variant:

```cpp
/** Fuel reached the tanks - a contract day or a spot order (spec 2026-10-02 §7). Litres arrived, Added fitted. */
struct AIRPORTOPS_API FFuelDeliveredEvent
{
	double Litres = 0.0;
	double Added = 0.0;
	bool bContract = false;
	static const TCHAR* EventName() { return TEXT("FuelDelivered"); }
	FString Describe() const;
};
```

`Describe` in `OpsEventBus.cpp`: `return FString::Printf(TEXT("%s %.0f L, %.0f L added"), bContract ? TEXT("contract") : TEXT("spot"), Litres, Added);`

In `FuelSupply.h` add the enum, the two USTRUCTs (each with `GENERATED_BODY()` and `UPROPERTY()` fields), the members and the methods listed under Interfaces. Implementation:

```cpp
double UFuelSupply::PendingSpotLitres() const
{
	double Total = 0.0;
	for (const FFuelSpotOrder& Order : SpotOrders) { Total += Order.Litres; }
	return Total;
}

EFuelOrderRefusal UFuelSupply::JudgeSpot(double Litres) const
{
	// ROOM COUNTS WHAT IS ON THE WAY: two orders that each fit alone must not together overflow - a spot order is not
	// take-or-pay, and refusing at the order is the only place the player can still change their mind.
	if (Litres <= 0.0 || Litres > FreeSpace() - PendingSpotLitres()) { return EFuelOrderRefusal::NoRoom; }
	if (Ledger != nullptr && Ledger->Balance() < Litres * Figures.SpotPricePerLitre) { return EFuelOrderRefusal::CannotAfford; }
	return EFuelOrderRefusal::None;
}

EFuelOrderRefusal UFuelSupply::OrderSpot(double Litres, double Now)
{
	const EFuelOrderRefusal Why = JudgeSpot(Litres);
	if (Why != EFuelOrderRefusal::None) { return Why; }
	if (Ledger != nullptr)
	{
		Ledger->Post(Now, ELedgerCategory::FuelPurchase, -Litres * Figures.SpotPricePerLitre,
			FText::Format(NSLOCTEXT("Ledger", "FuelSpot", "Spot fuel, {0} L"), FText::AsNumber(FMath::RoundToInt(Litres))));
	}
	SpotOrders.Add({ Litres, Now + Figures.SpotDelaySeconds });
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: spot order %.0f L, due at %.0f"), Litres, Now + Figures.SpotDelaySeconds);
	return EFuelOrderRefusal::None;
}

int32 UFuelSupply::ReceiveDueSpot(double Now)
{
	int32 Delivered = 0;
	for (int32 Index = SpotOrders.Num() - 1; Index >= 0; --Index)
	{
		if (SpotOrders[Index].DueAt <= Now)
		{
			const double Litres = SpotOrders[Index].Litres;
			const double Added = Receive(Litres);
			SpotOrders.RemoveAt(Index);
			++Delivered;
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: spot delivery %.0f L (%.0f added), stock %.0f L"), Litres, Added, StockLitres);
			if (Bus != nullptr) { Bus->Publish(FFuelDeliveredEvent{ Litres, Added, false }); }
		}
	}
	return Delivered;
}

EFuelOrderRefusal UFuelSupply::JudgeContract(int32 Tier) const
{
	if (Contract.Tier != INDEX_NONE) { return EFuelOrderRefusal::AlreadyContracted; }
	if (!Figures.ContractTiers.IsValidIndex(Tier)) { return EFuelOrderRefusal::UnknownTier; }
	if (Figures.ContractTiers[Tier].LitresPerDay > Capacity()) { return EFuelOrderRefusal::NoRoom; }
	return EFuelOrderRefusal::None;
}

EFuelOrderRefusal UFuelSupply::SignContract(int32 Tier, double Now)
{
	const EFuelOrderRefusal Why = JudgeContract(Tier);
	if (Why != EFuelOrderRefusal::None) { return Why; }
	Contract.Tier = Tier;
	Contract.DaysLeft = Figures.ContractTermDays;
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: contract signed, %.0f L a day for %d day(s)"),
		Figures.ContractTiers[Tier].LitresPerDay, Contract.DaysLeft);
	return EFuelOrderRefusal::None;
}

void UFuelSupply::DeliverContractDay(double Now)
{
	if (Contract.Tier == INDEX_NONE || !Figures.ContractTiers.IsValidIndex(Contract.Tier)) { return; }
	const FFuelContractTier& Tier = Figures.ContractTiers[Contract.Tier];
	// TAKE-OR-PAY (spec §7): the whole day is charged before any of it is measured against the tanks.
	if (Ledger != nullptr)
	{
		Ledger->Post(Now, ELedgerCategory::FuelPurchase, -Tier.LitresPerDay * Tier.PricePerLitre,
			NSLOCTEXT("Ledger", "FuelContract", "Fuel contract delivery"));
	}
	const double Added = Receive(Tier.LitresPerDay);
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: contract delivery %.0f L (%.0f added, %.0f poured away), stock %.0f L, %d day(s) left"),
		Tier.LitresPerDay, Added, Tier.LitresPerDay - Added, StockLitres, Contract.DaysLeft - 1);
	if (Bus != nullptr) { Bus->Publish(FFuelDeliveredEvent{ Tier.LitresPerDay, Added, true }); }
	if (--Contract.DaysLeft <= 0) { Contract = FFuelContract(); }
}

EFuelOrderRefusal UFuelSupply::CancelContract(double Now)
{
	if (Contract.Tier == INDEX_NONE || !Figures.ContractTiers.IsValidIndex(Contract.Tier)) { return EFuelOrderRefusal::NoContract; }
	const FFuelContractTier& Tier = Figures.ContractTiers[Contract.Tier];
	const double Charge = Contract.DaysLeft * Tier.LitresPerDay * Tier.PricePerLitre * Figures.CancelFraction;
	if (Ledger != nullptr)
	{
		Ledger->Post(Now, ELedgerCategory::FuelPurchase, -Charge, NSLOCTEXT("Ledger", "FuelCancel", "Fuel contract cancelled"));
	}
	UE_LOG(LogAirportOps, Log, TEXT("Fuel: contract cancelled with %d day(s) left, charge %.0f"), Contract.DaysLeft, Charge);
	Contract = FFuelContract();
	return EFuelOrderRefusal::None;
}
```

Include `AirportOpsLog.h`, `Model/Ledger.h` and `Model/OpsEventBus.h` in the .cpp. CannotAfford for a CONTRACT is deliberately not checked: like upkeep, a contract day posts whatever the balance (a negative balance locks placement, GDD §11).

- [ ] **Step 5: Run.** `-Filter AirportOps.Model.FuelSupply`, then `-Filter AirportOps` and `Tools/Check-Architecture.ps1` (rule 71 will fail until Task 6 lists the supply - if so, do Task 6's `Publishers()` line now and say so in the commit).

- [ ] **Step 6: Commit.** `git commit -m "feat(fuel): contract (take-or-pay, term, cancel) and spot orders on the ledger"`

---

### Task 6: Wire it into the runtime - create, figures, capacity, day end, re-arm, save

**Files:**
- Modify: `Public/Present/OpsRuntime.h` (members :310-345; forwarders beside `QuoteFacility` :113), `Private/Present/OpsRuntime.cpp` (constructor :56-131, `ApplyScenarioFigures` :963, Attach :987, `WireBus` :365, `RearmRepeatingSchedules` :1215, `Persistents()` :1478, `Publishers()` :1461, new-game opening where `Ledger->Open(StartingBalance)` is called)
- Test: `AirportOpsTests/Private/OpsSaveTest.cpp`, an `AirportOps.Present.*` test file that attaches a runtime (find the one holding `AttachCopiesTheOffers`)

**Interfaces:**
- Consumes: everything above.
- Produces: `UPROPERTY() TObjectPtr<UFuelSupply> FuelSupply;` on `UOpsRuntime`; forwarders `const UFuelSupply* GetFuelSupply() const; EFuelOrderRefusal OrderSpotFuel(double Litres); EFuelOrderRefusal SignFuelContract(int32 Tier); EFuelOrderRefusal CancelFuelContract();`

- [ ] **Step 1: Verify sites.** Every site in Files. Find where a NEW game opens the ledger with `StartingBalance` (vs a load) - the starting stock must be set there and ONLY there.

- [ ] **Step 2: Write the failing tests.**
  - `AirportOps.Present.Fuel.AttachWiresTheSupply`: attach a runtime as the neighbouring Present tests do; assert `GetFuelSupply()` non-null, its `Figures.LitresPerTank` equals the scenario's, its `Capacity()` equals the starter depot's seated tanks x `LitresPerTank` (derive from the network, do not type 30000), the board's `FuelSupply` is the same object, and a new game's stock is `StartingStockLitres`.
  - `AirportOps.Present.Fuel.DayEndDeliversTheContract`: sign tier 0 through the forwarder, advance the clock past `NextDayStart()`, assert stock rose and a `FuelPurchase` ledger entry exists.
  - `AirportOps.Present.Fuel.SpotOrderSurvivesASave` (review focus 3): order spot, capture with `OpsSave::Capture`, restore into a fresh runtime, advance past `SpotDelaySeconds`, assert the stock rose - the clock queue is not saved, so this goes red unless the re-arm exists.
  - In `OpsSaveTest.cpp` `DesignFiguresAreNotSaved`: add a `UFuelSupply` block - serialize one with non-default `Figures` and `StockLitres`, deserialize into a fresh one with a CONTROL figure, assert `StockLitres` restored and `Figures` NOT.

- [ ] **Step 3: Run to see them fail.**

- [ ] **Step 4: Implement.**
  - Constructor: `FuelSupply = CreateDefaultSubobject<UFuelSupply>(TEXT("FuelSupply")); FuelSupply->Ledger = Ledger; JobBoard->FuelSupply = FuelSupply;` (beside the FacilityPurchases block :123-127).
  - `ApplyScenarioFigures`: `FuelSupply->Figures = Scenario.FuelSupply;`.
  - Attach, beside the other hooks: `TWeakObjectPtr<UOpsRuntime> WeakThis = this; FuelSupply->CapacityOf = [WeakThis]() { const UOpsRuntime* Self = WeakThis.Get(); return Self != nullptr && Self->Network != nullptr ? Self->JobBoard->FuelCapacityLitres(*Self->Network, Self->FuelSupply->Figures.LitresPerTank) : 0.0; };` - match how other hooks reach the network (verify the member name). Detach nulls it.
  - New game: `FuelSupply->StockLitres = Scenario.FuelSupply.StartingStockLitres;` next to the ledger's opening.
  - `Publishers()`: `Out.Add({ TEXT("FuelSupply"), &FuelSupply->Bus });`.
  - `Persistents()`: add `FuelSupply`.
  - `WireBus`: subscribe to `FDayEndedEvent` -> `FuelSupply->DeliverContractDay(Clock->Now())`; subscribe to `FFuelDeliveredEvent` -> `JobBoard->ReopenStockRefusals()`. Use the tier and `FName` style of the neighbouring subscriptions.
  - `RearmRepeatingSchedules`: cancel a `FuelSpotHandle` and re-book `Clock->Every(60.0, [this]() { FuelSupply->ReceiveDueSpot(Clock->Now()); })` - a minute poll, because the orders' due times are SAVED and the clock queue is not; one repeating poll re-arms with the rest and needs no per-order handle. Add `int32 FuelSpotHandle = INDEX_NONE;` beside `UpkeepHandle`.
  - Forwarders: thin calls passing `Clock->Now()`; each logs nothing (the supply logs).
  - Bump `FOpsSnapshot::Version` only if the existing comment at `OpsSave.h` says a new blob requires it (a missing blob already "starts fresh").

- [ ] **Step 5: Run.** `-Filter AirportOps.Present.Fuel`, `-Filter AirportOps.Model.Save`, full `-Filter AirportOps`, `Check-Architecture.ps1`.

- [ ] **Step 6: Commit.** `git commit -m "feat(fuel): runtime owns the supply - capacity from tanks, contract at day end, spot poll survives a load"`

---

### Task 7: Alert - fuel low

**Files:**
- Modify: `Public/Model/OpsAlerts.h` (`EAlertKind` :19 APPEND; `FOpsAlertSources` :107), `Private/Model/OpsAlerts.cpp` (`Recompute`, beside NoRunway :187), `Private/Present/OpsRuntime.cpp` (`RecomputeAlerts` ~:735)
- Test: the alerts test file (grep `EAlertKind::NoRunway` in `AirportOpsTests`)

**Interfaces:**
- Produces: `EAlertKind::FuelLow`; `const UFuelSupply* FOpsAlertSources::FuelSupply`.

- [ ] **Step 1: Verify sites.**
- [ ] **Step 2: Failing test** `AirportOps.Model.Alerts.FuelLowWhenUnderAQuarter`: sources with a supply holding 20% of capacity and no contract -> one `FuelLow`; 30% -> none; 20% WITH a contract -> none (the player has acted); a supply whose capacity is 0 (no tanks) -> none (NoRunway-style conditions are someone else's alert).
- [ ] **Step 3: Run, see it fail.**
- [ ] **Step 4: Implement.** Append `FuelLow` to `EAlertKind`; in `Recompute`:

```cpp
	// FUEL LOW (spec 2026-10-02 §7): under a quarter of the tanks and nothing contracted - the moment ordering still
	// helps. Not raised for a contracted airport, whose deliveries are already on the way, nor one with no tanks.
	if (Sources.FuelSupply != nullptr && Sources.FuelSupply->Capacity() > 0.0
		&& Sources.FuelSupply->Contract.Tier == INDEX_NONE
		&& Sources.FuelSupply->Available() + Sources.FuelSupply->PendingSpotLitres() < 0.25 * Sources.FuelSupply->Capacity())
	{
		Found.Add(OpsAlertOf(EAlertKind::FuelLow, 0, NAME_None,
			NSLOCTEXT("OpsAlerts", "FuelLow", "Fuel low - order spot fuel or sign a contract at the depot")));
	}
```

Match `OpsAlertOf`'s real signature. Wire `Sources.FuelSupply = FuelSupply;` in `RecomputeAlerts`. If alerts recompute only on named bus events, add `FFuelDeliveredEvent` and the refill draw to what wakes it (follow how `NoRunway`'s wake-up is wired; rule 'network-change-announced' and the alerts pass show the pattern).
- [ ] **Step 5: Run.** `-Filter AirportOps.Model.Alerts`, full `-Filter AirportOps`.
- [ ] **Step 6: Commit.** `git commit -m "feat(fuel): FuelLow alert under a quarter of the tanks with nothing contracted"`

---

### Task 8: The depot card - stock line, buy tank, spot order, contract

**Files:**
- Modify: `Source/AirportMgr/BuildActions.h` / `.cpp` (:204-222 `CanBuyModule`/`BuyModule`; registration :439-466)
- Modify: `Source/AirportMgr/InspectorFacilityRows.h` / `.cpp` (`EAction` :72, id table :19, `Show` ~:130-150)
- Test: `Source/AirportMgr/BuildActionsTest.cpp` (`RegistryIsComplete` :46), `Source/AirportMgr/InspectorCardsTest.cpp` (:607-627 tables)

**Interfaces:**
- Consumes: `UOpsRuntime::GetFuelSupply / OrderSpotFuel / SignFuelContract / CancelFuelContract` (Task 6); `FBuildActionArg { FName Code; int32 Id; }` (BuildActions.h:28).
- Produces: actions `selection.buy_module` taking `Arg.Code` = module name (`Shed`/`Tank`); `selection.fuel_spot` (orders `OpsDesignDefaults::SpotOrderLitres`, add it = 10000.0); `selection.fuel_contract_up`; `selection.fuel_contract_cancel`.

- [ ] **Step 1: Verify sites**, including how `selection.buy_vehicle` reads `Ctx.Arg.Code` and how a facility row's button passes an arg.
- [ ] **Step 2: Failing tests.**
  - `InspectorCardsTest`: the depot card shows a fuel line reading `Fuel 12,000 / 30,000 L` (format via `FText::AsNumber`) when the supply holds 12,000 of 30,000, plus `contract 5,000 L/day, 6 days` when contracted; a SECOND module row for the tank (`Fuel tanks 1 / 2 space`); the tank's buy button carries `Code = Tank`.
  - `BuildActionsTest`: `RegistryIsComplete` passes with the three new actions; `BuyModuleUsesItsArgument` - with Shed and Tank both offered, executing `selection.buy_module` with `Arg.Code = "Tank"` buys a tank (the depot's Tank count rises by one, Shed count unchanged); `FuelSpotRefusalIsShown` - with the tanks full, `selection.fuel_spot` is disabled and its tooltip reads the `NoRoom` sentence.
- [ ] **Step 3: Run, see them fail** (`Run-AirsideTests.ps1 -Filter AirportMgr`).
- [ ] **Step 4: Implement.**
  - `BuyModule`/`CanBuyModule`: pick the quote row whose `Module` matches `Ctx.Arg.Code` (`StaticEnum<EDepotModule>()->GetNameStringByValue`); NO arg keeps `Modules[0]` so existing callers and the chord still work. Replace the "A second becomes a menu" comment with the reason for the argument.
  - `InspectorFacilityRows`: turn the single `ShedsRow`/`ShedsText`/`BuyModuleButton` trio into one trio PER `Quote.Modules` entry (an array built in `Show`, collapsing unused rows), each button bound to `selection.buy_module` with its module's `Code`. Add a fuel text row, and three buttons for the fuel actions. Extend `EAction` and the static-asserted id table.
  - Fuel action enable/tooltip: from `JudgeSpot(OpsDesignDefaults::SpotOrderLitres)`, `JudgeContract(next tier)` (next tier = `Contract.Tier + 1` when none is running is tier 0; while contracted, the up action is disabled with `AlreadyContracted` - one contract at a time), and `Contract.Tier != INDEX_NONE` for cancel. One `FText` per `EFuelOrderRefusal`, in a `static FText FuelOrderRefusalText(EFuelOrderRefusal)` beside `UFacilityPurchases::RefusalText`.
  - Toasts: when an action succeeds, notify through the existing purchase toast path (`EOpsPurchaseKind`, APPEND `FuelOrdered`, `FuelContractSigned`, `FuelContractCancelled`).
- [ ] **Step 5: Run.** `-Filter AirportMgr`, then the FULL suite: `./Tools/Run-AirsideTests.ps1 -Project <slot>\AirportMgr.uproject`. Read `N test(s) run, N failed, N crashed` and `registered but never ran: 0`.
- [ ] **Step 6: Commit.** `git commit -m "feat(fuel): depot card - stock line, tank buy, spot order, contract"`

---

### Task 9: Pacing model and docs agree with what shipped

**Files:**
- Modify: `Tools/pacing_model.py` (fuel constants), `docs/superpowers/specs/2026-10-02-progression-and-fuel-supply-design.md` §7 (one-pool deviation, figures), `docs/AirportManagerGDD.md` §9 table (Fuel depot "Stocks: fuel, litres" is now true; tank add-on)

- [ ] **Step 1:** Set `FUEL_CONTRACT_PRICE`/`FUEL_SPOT_PRICE` to the scenario's figures; run `python Tools/pacing_model.py --demand-scales --contract-share 0.8 --fuel-loss 0.1 --fee-scale 5 --fuel-price 2.0 --build-scale 0.4 --start-balance 260000`; record the PAVE line in the spec §9 if it moved, noting the starting stock is now 30,000 L (worth 27,000 at contract price) and tank upkeep exists.
- [ ] **Step 2:** Spec §7: add the one-pool deviation, the 25% FuelLow rule, the tier gate (`LitresPerDay <= capacity`), the poll re-arm.
- [ ] **Step 3: Commit.** `git commit -m "docs(fuel): spec and model follow the shipped supply"`

---

## Open questions (for the owner, before or during execution)

1. **Airline penalty on an unfuelled departure.** `UAirlineRoster` applies `ShortfallPenalty` today. The ruling was "no fine" - is a satisfaction hit acceptable (this plan keeps it), or should a `NoFuelStock` departure be exempt?
2. **Vehicles bought arrive full** (`FServiceFleet::Create(..., Cargo)`) - fuel from nowhere. Keep (a bowser comes with its first load), or deliver them empty so they draw the stock?
3. **Starting stock 30,000 L** (one full starter tank) - right gift, or start empty with a pending spot order?
4. **Contract tiers ignore milestones** until the cargo track exists - acceptable for now?
