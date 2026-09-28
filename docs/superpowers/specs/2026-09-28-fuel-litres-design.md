# Fuel by the litre: tank sizes, flow rates, trips

2026-09-28. Status: design approved in conversation 2026-09-28. Stacked on #381.

## Problem

Measured from `feature/turnaround-contract` (5a1c... / #381):

1. **Every fuelling takes the same 40 s**, on the MOVEMENT clock (`UFuelService::DwellSeconds`,
   divided by the depot's pump count, floored at `MinDwellSeconds`). A 172 and an A320 are
   fuelled in the same time.
2. **A vehicle's size means nothing.** The 1,000 L trailer "fuels" a Saab 340 (3,220 L tank) in
   one visit.
3. **The fee ignores the load**: `UPricing::FuelServiceFee` is half the base landing fee.

## Decisions (user, 2026-09-28)

| # | Question | Ruling |
|---|---|---|
| 1 | Fuel load | **Drawn at the offer**: 50-90% of the airframe's tank capacity. |
| 2 | Pumping time | **Litres / flow rate, in GAME time.** Trailer 75 L/min. |
| 3 | Vehicle size | **Tank capacity per vehicle; a load bigger than the tank takes several trips**, refilling at the depot between them. |
| 4 | Which vehicle serves a flight | **Unchanged for now** (stand letter: A/B utility tow + trailer, C-F bowser). A depot fleet the player buys (trailer, bowser, tanker) is the next piece. |
| 5 | Figures | Trailer 1,000 L @ 75 L/min; bowser 10,000 L @ 200 L/min; articulated tanker (later) 30,000 L @ 500 L/min; depot refill 500 L/min per pump module. First guesses, authored. |
| 6 | Fee | **Per litre delivered**, ¤1.50/L first guess. |

## Section 1: Data

- `UAircraftType::FuelCapacityLitres` (authored), carried into `FAirframe::FuelCapacityLitres`
  by `Airframe()` - the bundle already carries `TurnaroundSeconds` and `PushbackNeed` for the
  same reason (Model/ may not read the type). 0 = wants no fuel. Values from published figures:
  C172 212, Cherokee 189, SR22 348, Seneca 466, Meridian 454, Baron 734, Caravan 1,268, Twin
  Otter 1,466, King Air 350i 2,040, Saab 340B 3,220, A320 24,210, 737-800 26,020.
- Vehicle fuel figures are **AirportOps's**, not Airside's: Airside knows how a vehicle MOVES and
  must never learn what it is FOR (`UFuelService` header). `FFuelVehicleSpec { CapacityLitres,
  FlowLitresPerMinute }` in a `TMap<FName, FFuelVehicleSpec>` keyed by `FVehicle::TypeCode` on
  `UScenario::FuelVehicles` (defaults `UTILITY` 1000/75, `FUEL` 10000/200), copied into
  `UFuelService` at attach exactly as `FuelDwellSeconds` was. An unknown code falls back to the
  trailer's figures with a Warning once.
- `UScenario::DepotRefillLitresPerMinutePerPump = 500`; `UPricing::FuelPricePerLitre = 1.5`.
- `UScenario::FuelDwellSeconds` and `UFuelService::DwellSeconds`/`MinDwellSeconds`/
  `DwellSecondsFor` are removed.

## Section 2: The offer

`UOfferGenerator::MakeOffer` sets `UFlight::FuelLitres = round(capacity x Stream.FRandRange(0.5,
0.9))` - the seeded stream, so a seed repeats. `AcceptImmediate` (key 7) uses 0.7 x capacity.

The offer row's chip reads `Fuel 240 L` with the tick/cross, so the size of the job is visible
before the accept. A capacity of 0 shows no fuel chip.

**How the litres reach the fuel service.** `UFuelService::LitresOwedFor` is a
`TFunction<double(int32 AgentId, const FAirframe&)>` seam set by `UOpsRuntime::Attach` to ask the
board for the flight that owns the agent. Unset, or an agent no flight owns, it is 0.7 x the
agent's airframe capacity - one fallback, stated in one place.

## Section 3: The fuel loop

`FFuelDemand` gains `LitresOwed`, `LitresDelivered`, `Trips`. On park, `LitresOwed` comes from
`LitresOwedFor`; 0 litres makes no demand.

- **Pumping, GAME time.** At the hydrant: `Load = min(vehicle capacity, owed)`,
  `DwellEndsAt = Clock.Now() + Load / Flow x 60`. The dwell is timed on `USimClock` (pause
  stops it), no longer on `UGroundTraffic::GetSimSeconds` - the rule the old dwell's comment
  argued against is reversed, and the comment says why: a realistic load at a realistic rate is
  minutes of GAME time, a watchable 5-90 real seconds.
- **After a load**: `LitresDelivered += Load`, `LitresOwed -= Load`, `++Trips`. Owed > 0: the
  truck goes home and the demand returns to `Needed` (it will be served again - the deadline
  does not cut it off, as for Needed today). Owed = 0: `Done`, fee posted.
- **Refill at the depot.** A truck home with an empty tank is not free at once: the depot slot
  stays taken until `Clock.Now() + Delivered / (pumps x refill rate) x 60`. Counted in
  `TrucksOutFor`, and `FleetRevision` bumps when it frees, which is what re-offers a waiting
  demand. A plotless depot counts as one pump.
- Logs: `Fuel: truck 2 at stand 5 for aircraft 1: 1000 L of 2900 L (trip 1), 13 min`,
  `Fuel: aircraft 1 fuelled: 2900 L in 3 trips`, `Fuel: depot 6 refilling truck 2 (1000 L, 2 min)`.

**Money.** `UPricing::FuelFee(Litres) = Litres x FuelPricePerLitre`, posted once at `Done` for
the litres delivered.

## Testing

- Offer: load within [0.5, 0.9] x capacity; same seed same litres; capacity 0 gives 0.
- Pumping: SR22 300 L on the trailer = 4 game min; paused, the dwell does not end.
- Trips: 2,900 L on a 1,000 L trailer takes 3 trips; `Done` only after the third.
- Refill: after a trip the depot's slot is held for Delivered / (pumps x 500) game minutes.
- Fee: litres x rate, once.
- Content: every modelled aircraft type has a fuel capacity > 0, and the table's figures landed.
- Seam: the runtime wires `LitresOwedFor` to the board (a flight's `FuelLitres` reaches the demand).

## Out of scope

- The depot fleet (buying and assigning trailers, bowsers, tankers) - next.
- Hydrant pipe network.
- Partial service scoring (C).
