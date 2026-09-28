# Fuel by the Litre Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Fuel loads drawn per flight, pumped at the vehicle's flow rate in game time, in as many trips as the vehicle's tank needs, refilled at the depot by pumps, paid per litre.

**Spec:** `docs/superpowers/specs/2026-09-28-fuel-litres-design.md`

## Global Constraints
- Worktree `C:\repos\airportmgr2-turnaround-contract`, branch `feature/fuel-litres` (stacked on #381). `-NoHotReloadFromIDE`; tests `-Project`.
- Airside never gets fuel semantics beyond the airframe bundle's capacity; vehicle fuel figures live in AirportOps (`UScenario::FuelVehicles`).
- New test .cpp needs two builds. No Co-Authored-By.

## Review Focus
- A demand mid-trips when the aircraft leaves (depart / delete): truck recalled, refill still released.
- Pause mid-pump: dwell does not end.
- Depot deleted while a truck refills: slot freed, no dangling refill.
- An aircraft type with capacity 0: no demand, no fee, turnaround unaffected.
- Save/load: `Demands` is transient today - unchanged behaviour (a load clears demands).

### Task 1: Airframe fuel capacity + content
Files: `Airside/Public/Entities/AircraftType.h`, `Airside/Public/Model/Airframe.h`, new `Tools/Python/build_fuel_capacities.py`, test in `AirsideTests/Private/AirframeAxlesTest.cpp` style: new `FuelCapacityTest.cpp` (`Airside.Content.FuelCapacitiesAuthored`: every DA_Aircraft_* with a mesh has capacity > 0 and equals the script's table for the probed types).
- [ ] test RED, add fields (`UAircraftType::FuelCapacityLitres`, `FAirframe::FuelCapacityLitres`, copy in `Airframe()`), build, run script headless, GREEN, commit.

### Task 2: Offer draws the load; seam to fuel service
Files: `Flight.h` (`FuelLitres`), `OfferGenerator.cpp`, `FlightBoard.cpp` (AcceptImmediate 0.7x), `FuelService.h` (`LitresOwedFor` TFunction + `DefaultLitres(const FAirframe&)`), `OpsRuntime.cpp` wiring, `OfferViewModels`/`OfferInboxWidget` chip text.
Tests: `AirportOps.Model.Offers.Generate.FuelLoadIsHalfToNineTenths`, `AirportOps.Fuel.RuntimeWiresLitresOwed` (runtime attach -> seam set; a board flight with FuelLitres 300 owning agent 5 -> seam returns 300), `AirportMgr.Offers.ViewModel.FuelChipShowsLitres`.
- [ ] RED, implement, GREEN, commit.

### Task 3: Litres, trips, refill, fee in the fuel service
Files: `FuelService.h/.cpp`, `OpsDefinition.h` (FuelVehicles, refill rate; remove FuelDwellSeconds), `Pricing.h/.cpp` (`FuelPricePerLitre`, `FuelFee(double)` replaces `FuelServiceFee`), `OpsRuntime.cpp`; tests: rewrite `PumpDwellTest.cpp` -> `FuelLitresTest.cpp` cases (`PumpTimeIsLitresOverFlow`, `PausedPumpDoesNotFinish`, `BigLoadTakesTrips`, `RefillHoldsTheDepot`, `FeeIsPerLitre`, `NoCapacityNoDemand`), adapt `FuelServiceTest.cpp` dwell assertions.
- [ ] RED, implement, GREEN, mutation (trips: deliver whole owed in one load -> BigLoadTakesTrips red), full suite, commit.

### Task 4: Review, PR
- [ ] Opus review, fix Critical/Important with TDD, PR against `feature/turnaround-contract`.
