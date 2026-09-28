# Offers and demand: a queued inbox, real-time windows, a daily demand curve

2026-09-28. Status: design approved in conversation 2026-09-28; spec awaiting review.

Chunk A of three. B is the arrival sequencer (runway queue, holding, diversion - the ATC
tower's groundwork). C is the per-flight service score (-100..+100) and reputation. A is
built first because it is the most visible and needs neither; it RECORDS what B and C will
read, so neither needs a migration.

## Problem

The offer system was built to get an aircraft landed for testing. As play it has no shape
(state measured from code and `Saved/Logs/AirportMgr.log`, 2026-09-28):

1. **One fixed cadence.** `UOpsRuntime::Attach` books `Clock->Every(Interval)` once, from
   `SecondsPerDay / sum(OffersPerDay)`. No busy periods, no lulls. Log: `Offers: 4.0 per game
   day across 1 airline(s)`.
2. **The fee lever does not move the cadence.** `Interval` reads `Pricing->DemandFactor()` at
   Attach only (`OpsRuntime.cpp:231`); `StepLandingFee` re-times nothing, and neither does a
   load. Raising the fee is pure profit mid-session - the elasticity trade
   (`UPricing::Elasticity`'s "am I full?") is not live.
3. **The window is too short to decide in.** `OfferLifeSeconds = 600` GAME seconds is ~8 real
   seconds at 1x and less at 3x. Log: Offer 2 posted 18:08:31, lapsed 18:08:39.
4. **`OfferWeight` is read by nothing.** The pick is uniform over every admissible type of
   every airline, so a bigger fleet is a louder airline.
5. **`OffersPerDay`'s comment is false.** It says the rate "scales down as capability falls";
   capability only filters, the rate is flat.
6. **A missing service blocks the offer** (`NoStandServiceable` is permanent in
   `UOfferGenerator::IsPermanentRefusal`), so the player cannot choose to take a flight they
   will serve badly.

## Decisions (user, 2026-09-28)

| # | Question | Ruling |
|---|---|---|
| 1 | What drives busy/lull | **Time of day.** Per-airline 24 h demand curve. |
| 2 | Day length | **Configurable; separate daylight and night lengths.** Daylight 40 real min at 1x, night 8 (default, unconfirmed - tune). |
| 3 | Offer window | **Real seconds, per airline.** Pause stops it; speed does not change it. |
| 4 | Stand availability | **A suitable stand free NOW.** Accept holds it at once, as today. Forecast-free-at-ETA belongs to B. |
| 5 | Which facilities are soft | **Services only.** Geometry and surface gates stay hard and are never offered. |
| 6 | Inbox cap | **Capped** (8). A full inbox drops the offer and counts it. |
| 7 | GA and the floor | **A "flying club" airline** flagged as the floor. |
| 8 | Penalty for not accepting | **Decline free; lapse costs a little** with that airline (C applies it; A records why). The club never penalises. |
| - | Generation mechanism | **Accumulator** over a per-minute rate (not Poisson, not a re-timed fixed timer). |
| - | Countdown mechanism | **Remaining seconds on the flight**, drained in `Tick` (not a second real-time clock). |
| - | Club at night | **Quiet.** The floor applies in daylight only; night is a real lull. |

### Deviations from the GDD, and why

- **GDD section 3, "Missing a required service means no offer."** Reversed for services: the offer
  is made, shows the service as unavailable, and C scores the flight down. The player's
  decision - take it now badly, or build first - is more interesting than a silent filter,
  and the filter hid WHY an airline was not offering. Infrastructure demands stay hard
  gates exactly as section 3 says.
- **GDD sections 2 and 6, "an airline stops coming."** Kept for ordinary airlines (C), but the airport as
  a whole is never shunned: the flying club is immune. "You should never be in a place where
  no one wants to use you."
- **GDD section 6, "Offers are generated over game time ... weights by reputation."** Generation is
  still over game time; reputation weighting arrives with C through the same rate function.

The GDD is updated to match when A merges, not before - it describes the game, and until A
lands the game is the old one.

## Section 1: Data model

### `UAirlineDefinition` (per airline, authored)

| Field | Replaces | Meaning |
|---|---|---|
| `DemandCurve` (24 x double, 0..1) | `OffersPerDay` | Hourly weight; interpolated linearly between hours. |
| `PeakOffersPerHour` | `OffersPerDay` | Rate at weight 1.0, before the fee's demand factor. |
| `FloorOffersPerHour` | - | Rate never falls below this in daylight. Non-zero only on the club. |
| `bIsFloor` | - | The club flag: immune to C's reputation cuts; lapses never penalise. |
| `OfferWindowSeconds` | `UOfferGenerator::OfferLifeSeconds` | REAL seconds an offer stands. Club 120, demanding carrier 45. |
| `LeadTimeSeconds` | `UOfferGenerator::LeadTimeSeconds` | GAME seconds from accept to the aircraft appearing on approach. |
| `TurnaroundSlack` | - | Multiplier on `FAirframe::TurnaroundSeconds` for the contract. 1.0 strict; club 2.0. |
| `CallsignPrefix` | - | Flight-number prefix ("CA"), or a tail pattern for the club ("G-????"). |

`OfferWeight` is deleted (issue 4 above): the curve and the peak rate are the weight.

### `UScenario` and `USimClock`

- `RealSecondsPerGameDay` is replaced by `RealSecondsDaylight = 2400`, `RealSecondsNight = 480`,
  `DawnHour = 6`, `DuskHour = 20`. `USimClock` advances game time at two rates, chosen by the
  hour. Every consumer of `TimeScale()` (sun, fuel dwell, the ledger's day) reads the clock,
  so none of them changes; `USimClock::SecondsPerDay` stays 86400 GAME seconds.
- `MaxPendingOffers = 8` (scenario). The ATC tower may raise it later.

### `UFlight`

| Field | Meaning |
|---|---|
| `OfferSecondsLeft` | REAL seconds, saved, drained only while unpaused. Replaces `ExpiresAt`. |
| `Callsign` | Generated at offer from the airline's prefix and the seeded stream. |
| `bWasEverAcceptable` | Set the first time the board evaluates the offer as acceptable. |
| `LapseReason` | `None`, `Ignored` (was acceptable at some point), `NeverAcceptable`. C reads it. |
| `AcceptedAt` | Game time of accept. |
| `AirborneBy` | The turnaround contract: `ArrivesAt + TaxiAllowance + TurnaroundSeconds x TurnaroundSlack`, fixed at accept. |
| `AirborneAt` | Game time the flight reached `Departing`. |

**Revised 2026-09-28, from play:** the contract is ONE per-airline figure,
`UAirlineDefinition::ContractSeconds` (club 3 h, Cumbria 2 h). The formula below gave an SR22
40 game minutes and it reached its stand with one to spare - aircraft move in real seconds while
the clock runs ~21x, so landing + taxi-in measured ~28 game min, a fuel loop ~30, taxi-out ~25.
`TurnaroundSlack` and `TaxiAllowance` are gone. Original text kept below.

`TaxiAllowance` is one scenario figure for A (a flat game-seconds allowance for landing and
taxi-in). B replaces it with the sequencer's own estimate.

### Refusals

`EArrivalRefusal::NoStandServiceable` leaves `IsPermanentRefusal`'s permanent set: it no
longer blocks an offer. It still blocks nothing at accept (it never did - the stand holds; the
service fails later on the fuel path).

### Saves

There are no player saves (2026-09-23 ruling). Old slots are not migrated; the PR notes the
break. `OpsSave`'s snapshot version is bumped so an old slot fails loudly, not strangely.

## Section 2: Generation

`UOfferGenerator` is ticked by `Clock.Every(60 game s)` - replacing the single fixed-interval
timer. Per airline, per tick:

1. **Rate** `= max(Peak x Curve(hour) x Pricing.DemandFactor(), daylight ? Floor : 0)`, in
   offers per game hour. `Curve(hour)` interpolates the 24 weights. This is ONE function,
   `UOfferGenerator::RateAt(Airline, GameTime, Pricing)`; the demand strip (section 4) calls it too.
2. **Admissibility.** If no fleet type passes `CouldEverAdmit`, the airline accrues nothing -
   unmet demand is not banked. Logged on the TRANSITION only (`Offers: Cumbria Air cannot use
   this airport (runway too short for ATR 72)` / `Offers: Cumbria Air can use this airport
   again`), never per tick.
3. **Accumulate** `Rate / 60`. When the accumulator reaches the airline's current threshold,
   emit one offer, subtract the threshold, and redraw it from the seeded stream, uniform in
   [0.6, 1.4]. The mean rate is exact; the spacing is not metronomic.
4. **Pick** uniformly among that airline's admissible types.
5. **Cap.** If `PendingOfferCount() >= MaxPendingOffers`, the offer is not made: logged
   (`Offers: inbox full, dropped Cumbria Air ATR 72`) and `DroppedOffers` incremented (saved;
   C reads it as unmet demand).

The fee's demand factor is read every tick, so `StepLandingFee` moves the cadence and the
price of NEW offers at once (fixes problem 2). The per-airline accumulator, its threshold and
the stream are saved; a reload continues the same sequence.

`OfferIntervalSeconds` and `UOpsRuntime`'s `OfferHandle` / `LastOfferIntervalSeconds` go.
The startup log line becomes the day's expected total from integrating `RateAt`:
`Offers: ~N expected today across M airline(s) (floor: Flying Club)`.

## Section 3: Inbox, countdown, accept

**Countdown.** `UOpsRuntime::Tick(RealDelta)` drains `OfferSecondsLeft` by `RealDelta` when the
clock is not `Paused` - never multiplied by the speed. At zero: `Expired`, `LapseReason` from
`bWasEverAcceptable`, moved to History, logged (`Offer 7 lapsed (Ignored)`). This deletes
`ExpiresAt`, `ScheduleExpiry`, `CancelExpiry`, `ExpiryHandles` and the Offered branch of
`RearmSchedules`.

**Acceptability is the board's.** The `WhyNotAcceptable` cache (keyed on board, guideline and
occupancy revisions - issue #169) moves from `UOfferViewModel` into `UFlightBoard`, per
offer. The board re-evaluates an offer when any key moves, sets `bWasEverAcceptable`, and the
row reads the board's answer. One evaluator; a lapse is classified correctly with the inbox
closed.

**Accept.** Unchanged gate: `Offered`, and `UStandAllocator::Reserve` holds a free admitting
stand now. On success the countdown stops, the arrival is scheduled at `Now +
Airline.LeadTimeSeconds` (game), `AcceptedAt` and `AirborneBy` are fixed. `AirborneAt` is
written at `Departing`. A flight that cannot dispatch at its ETA keeps today's warning; B
queues it.

**Soft services.** Fuel is the one service live in A. New world-free
`UFuelService::CouldServe(const URoadNetwork&, const FAirframe&)` answers "is there a depot
that could serve a stand this airframe would take". Accept is allowed when it is false; the
flight lands, waits out its turnaround unserved, and leaves - today's `Unserviceable` path,
unchanged. Pushback need is DISPLAYED only ("needs tug"); no tug service exists to query.

**Order.** `UFlightBoard::Offers()` returns time-left ascending.

## Section 4: UI

`OfferInboxWidget`, one row per offer:

- **Who** - callsign, airline, type, code-letter badge.
- **Worth** - landing fee, fixed at offer.
- **Contract** - "lands 15 min after accept - airborne within 1 h 10 m" (game time, clock format).
- **Demands** - chips: fuel with/without (live, `CouldServe`); pushback "tug" as information.
- **Countdown** - draining bar plus seconds; amber under 30 s, pulsing under 10 s.
- **Accept** - greyed with the reason as subtitle and tooltip (`ArrivalPlanner::DescribeRefusal`),
  or "Accept (no fuel)" when a service is missing. **Decline** - always live.

Header "Offers 3/8". The bottom bar's inbox button carries a count badge; a toast fires on a
new offer while the inbox is closed (existing `ToastStackWidget`).

**Demand strip.** A thin 24 h strip above the rows: the summed `RateAt` over all airlines,
a now-marker, night shaded. It is what makes time-of-day demand plannable. It samples the
generator's own function - it cannot disagree with it.

**Clock.** Sun/moon icon beside the time on the bar.

Colours, sizes and the pulse are tuned live (Live Coding, shot, look), not specced to the pixel.

## Testing

Model (world-free, `NewObject`):

- Flat curve at 6/h yields 6 +/- 1 offers in a game hour.
- A peak hour yields more than a trough hour, same airline.
- The club's floor holds at fee 100% and 200%; it is zero at night.
- An inadmissible airline yields 0; becoming admissible does not release a banked burst.
- A full inbox drops and counts; `DroppedOffers` survives a save.
- Same seed, same inputs, same sequence of offers (callsigns included).
- Stepping the fee mid-hour changes the next hour's count (the regression for problem 2 -
  delete the live read and watch it go red).
- `USimClock`: an hour at 07:00 and an hour at 23:00 take the configured real seconds each.

Composition (spawn runtime + board + row, tick it):

- Countdown drains equally at 1x and 3x; not at all paused.
- Save mid-countdown, load, seconds left unchanged.
- An offer never acceptable lapses `NeverAcceptable`; one that was acceptable once lapses
  `Ignored` - with no inbox widget alive.
- Accept with no fuel depot succeeds; the flight leaves after its turnaround; `AirborneAt` set.
- `AirborneBy` equals the formula.
- Row reason and button state match the board after a stand frees.
- Rows sorted by time left; badge equals `PendingOfferCount`.
- The strip's samples equal `RateAt` at the same hours.

## Content

- New `DA_Airline_FlyingClub`: `bIsFloor`, C172 (moved from Cumbria), daylight curve,
  floor ~1/h, window 120 s, slack 2.0, tail pattern `G-????`.
- `DA_Airline_Cumbria`: scheduled/charter shape - morning and evening peaks, window 60 s,
  slack 1.3. Its fleet loses the C172; which of the existing types (`DA_Aircraft_A320`,
  `B738`, `Plane1`-`Plane18`) it flies is a content choice made in the plan. Until the airport
  admits one, the log's transition line says so.
- Both need no new `PrimaryAssetTypesToScan` line (the type is already scanned).

## Out of scope

- Runway queue, holding, diversion, forecast stand availability - **B**.
- Score, reputation, what a lapse or an unserved service costs - **C**.
- Airline contracts and the schedule grid (GDD section 6) - after C.
- Per-type weights within an airline.
