# Facility upgrades and fleet purchase — design

2026-09-29. First consumer: the fuel depot (buy sheds, buy/sell vehicles). Built generic so
every later building with modules and a fleet (catering, baggage, stairs, GPU) reuses it.

## 0. Why

Play-testing 2026-09-29: landings and scheduling hold up; the fuel depot is the bottleneck.
It has one placeholder fleet seeded at placement and no way to grow. GDD §5 (:204-208, :241)
always said capacity grows by add-on modules and "fleet size is an add-on count".

## 1. Rulings (user, 2026-09-29)

| # | Ruling |
|---|---|
| R1 | Model is generic: buildings get upgrades and buy vehicles through one mechanism. |
| R2 | One shed = one vehicle bay (any type). |
| R3 | A new depot has its start kit (Shed + Tank + Pump) and **no vehicles**. |
| R4 | Buyable types this slice: the existing two rows, UTILITY and FUEL bowser. Tanker later = a data row. |
| R5 | Idle vehicles can be sold for a partial refund. Sheds cannot be sold. |
| R6 | Sheds and vehicles both carry daily upkeep. |
| R7 | Approach A: one purchase service, data-driven catalogues; `EDepotModule` stays an enum until a second building needs modules. |
| R8 | Purchases in play are not undoable; selling is the way back. A shed purchase is an undo checkpoint. |
| R9 | A shed can be bought only if the plot has space: a free reserved (ghost) shed slot. |
| R10 | The selected depot shows its ghost slots while its inspector card is open, not only in edit mode. |

Out of scope: buying pumps or tanks, fuel stock/tank inventory, selling sheds, the
articulated tanker, hydrant network, vehicle delivery time.

## 2. Data and capacity

- **Module offers** live on `UPlotModuleKit` (already the per-building data asset):
  `Price`, `UpkeepPerDay`, `Grants` (struct; this slice has one field, `VehicleSlots`).
  Only the shed kit grants anything (`VehicleSlots = 1`). Rules code never names "shed".
- **Vehicle offers**: `FFuelVehicleSpec` gains `Price`, `UpkeepPerDay`, `ResaleFraction`
  (default 0.5). Rows stay on `UScenario`.
- **Capacity is derived, never stored.** `VehicleSlots(entity)` = sum of `Grants.VehicleSlots`
  over `FEntityInstance::Modules`. Vehicles at a depot = `UJobBoard::Vehicles` with
  `Home == depot`.
- **Retired:** `FEntityInstance::Trucks`, `FEntityPlacement::Trucks`, `UJobBoard::SyncFleet`'s
  placeholder seeding and `SeededDepots`. Depot removal still withdraws its vehicles and
  reopens their jobs.
- **Placement start kit:** `{Shed, Tank, Pump}` — a depot is refillable from day one, so
  `HasWorkingPump` and `DepotKit::ReportIncomplete` are unchanged.
- **Space (R9):** buying a shed lights the next reserved shed slot. The reservation is the
  existing `RunCap`/`ReserveWeight` derivation over the drawn plot (module-kits spec §3.2-3.4).
  No free reserved slot → refused `NoSlotReserved`.
- **Starting figures** (data knobs; balance 500k, fuel 1.5/L, full bowser load ≈ 15k):

| Item | Price | Upkeep/day | Resale |
|---|---|---|---|
| Shed | 40,000 | 200 | — |
| UTILITY | 25,000 | 150 | 50% |
| FUEL bowser | 90,000 | 500 | 50% |

## 3. Commands and events

`UFacilityPurchases` — new UObject in AirportOps `Model/`, owned by `UOpsRuntime`; holds
`URoadNetwork`, `UJobBoard`, the purse/ledger, and the bus. World-free.

- `Quote(Entity) -> FFacilityQuote`: slots owned/reserved, vehicles/bays, and one entry per
  module offer and vehicle offer with price, enabled, and refusal reason. **The UI renders only
  the quote**, so a button cannot disagree with the rules.
- `BuyModule(Entity, Module)`, `BuyVehicle(Entity, TypeCode)`, `SellVehicle(VehicleId)`
  → `FPurchaseResult { EPurchaseRefusal Refusal; double Amount; }`.
- `EPurchaseRefusal`: `None, NotAFacility, CannotAfford, NoSlotReserved, NoFreeBay,
  UnknownType, VehicleBusy`.
- Rules:
  - BuyVehicle: needs a free bay and the money. Adds an Idle, full `FServiceVehicle` at Home;
    bumps `FleetRevision`.
  - SellVehicle: Idle only (parked, empty queue). Credits `Price × ResaleFraction`; removes it.
  - BuyModule: needs a free reserved slot and the money. Appends to `Modules` through a new
    `URoadNetwork` mutator; the surface presenter relights via the network-changed path.
- **Money is synchronous inside the command**, never via an event — a refusal is never charged.
  Ledger: sheds post `Placement`; new category `Fleet` for vehicle buys and sale credits;
  daily upkeep adds module upkeep and fleet upkeep as two described lines.
- **Undo (R8):** undo is a whole-network Memento, so a shed purchase clears the build undo
  history (checkpoint). Otherwise undoing an earlier edit would drop the shed and keep the
  money. Vehicle trades never touch undo.
- **Logging** (`LogAirportOps`, one line per call):
  `Purchase: depot 3 bought FUEL for 90000 (2/2 bays)`,
  `Purchase refused: depot 3 FUEL - NoFreeBay`, `Purchase: depot 3 sold vehicle 7 for 45000`.
- **Bus** — two new variant events, published only on success:
  - `FFacilityUpgradedEvent { Entity, Module }`
  - `FFleetChangedEvent { Depot, VehicleId, Bought | Sold }`
  - Wired in `UOpsRuntime::WireBus` (rule 31):
    - Sim: `FFleetChanged` marks the JobBoard pass dirty — waiting jobs meet the new vehicle
      on the next drain; nothing polls.
    - Presentation: `UOpsEvents::OnFleetChanged` delegate; `Notification` toast
      ("Bought bowser — 90,000").
- **Save:** fleet already saved (#393); sheds saved in `Modules`. A load keeps the saved fleet
  even if it exceeds bays; excess can be sold, not replaced. No player saves exist yet — the
  break is noted in the PR.

## 4. UI

Depot card in `UInspectorWidget`, all rows rendered from `Quote(Entity)`:

```
Fuel depot 3                        Ready | No vehicles — buy one
<existing backlog line (#394)>
Sheds 2 / 3 space                   [Buy shed 40,000]   (disabled: "No space" / "Can't afford")
Vehicles 1 / 2 bays                 [Buy vehicle ▾]
                                      Utility tow 25,000 · 1,000 L
                                      Bowser      90,000 · 10,000 L
  Bowser #7 — idle at depot          [Sell 45,000]
  Utility #8 — fuelling stand 4      [Sell] (disabled: busy)
```

- Buttons go through `FBuildAction` (`selection.buy_module`, `selection.buy_vehicle`,
  `selection.sell_vehicle`) like `runway_use` / `unstick`; rows are generated from the quote's
  offer list, so a new kit or vehicle row needs no UI code. Menu reuses `UUiMenuButton`.
- C++ base builds it asset-free; BP restyles through `BindWidgetOptional` `ShedsRow`,
  `VehiclesRow`, `FleetList`.
- Nothing on the bottom bar: acting on a selection belongs to the inspector.
- **Ghost slots (R10):** presenter's ghost gate becomes `edit mode OR entity == revealed
  entity`; the inspector sets/clears the revealed entity from its selection. Hovering Buy shed
  highlights the next slot to light. The zoomed-out readability ruling (ghosts only in edit
  mode) holds for every other depot.

## 5. Testing

World-free, `AirportOps.Model.Facility.*`:
- slots derived from modules; new depot has full kit, zero vehicles, no seeding
- buy vehicle charges, adds Idle full vehicle, publishes `FFleetChanged`
- each refusal (`NoFreeBay`, `CannotAfford`, `NoSlotReserved`, `VehicleBusy`, `UnknownType`,
  `NotAFacility`) charges nothing and publishes nothing
- sell credits `Price × ResaleFraction`, removes vehicle
- buying a shed makes a second vehicle buyable
- upkeep sums modules + fleet
- over-capacity saved fleet survives a load
- quote reasons equal command outcomes for every offer (the UI and the rules cannot disagree)

Composition (one per seam):
- `FFleetChanged` wakes the JobBoard pass: a job waiting on an empty depot dispatches after
  buy + drain, with no tick polling
- shed purchase relights a slot in the presenter (lit count +1)
- shed purchase clears the undo stack
- revealed depot draws ghosts outside edit mode; others do not

UI: inspector action ids exist in the registry, checked by name.

Updated: fleet-seeding tests (`RestoredFleetIsNotReseeded`, `SyncFleet` tests) and every
`Trucks` reader.

PIE verification: place depot → "No vehicles — buy one"; buy bowser → `Purchase: depot N
bought FUEL` and the queued job dispatches; buy shed → ghost slot lights; sell idle vehicle →
refund line in the log and the ledger.
