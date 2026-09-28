# Offers and Demand (chunk A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed-cadence offer timer with a per-airline daily demand curve, real-time offer windows, a capped queued inbox, soft service demands and a turnaround contract, recorded so B (sequencer) and C (score) need no migration.

**Architecture:** `USimClock` gains a two-rate day. `UOfferGenerator` becomes persistent and ticks once a game minute, accumulating each airline's `RateAt` and emitting offers. `UFlightBoard` drains a real-seconds countdown per offer, owns the acceptability verdict (arrival refusal + fuel), and classifies lapses. The inbox viewmodel reads the board's verdict; the widget paints the new fields and a demand strip sampled from the generator's own `RateAt`.

**Tech Stack:** UE 5.8.2 C++, AirportOps plugin (Model/ world-free, Present/ composition), AirportMgr game module (UMG), UE automation tests, headless Python for content.

**Spec:** `docs/superpowers/specs/2026-09-28-offers-and-demand-design.md`

## Global Constraints

- Worktree `C:\repos\airportmgr2-offers-and-demand`, branch `feature/offers-and-demand`. The open editor is on `airportmgr2-ui-shape-spike`, so every build here adds `-NoHotReloadFromIDE`; tests pass `-Project`.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-offers-and-demand\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
- Test: `./Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-offers-and-demand\AirportMgr.uproject -Filter <prefix>`; read its `N test(s) run, N failed, N crashed` line.
- New test .cpp: build twice, check `Compile [x64] <File>.cpp` on the second.
- Model/ never includes Entities/, Content/, Present/ (Check-Architecture rule 1).
- Every `UE_LOG` removed must be replaced or named in the PR; WHY comments travel with code.
- A phase is an enum: `ELapseReason`, not bools.
- Numbers from the spec: daylight 2400 real s, night 480 real s, dawn 06, dusk 20, cap 8, threshold U[0.6,1.4], generator tick 60 game s, club window 120 s / slack 2.0 / floor ~1/h, Cumbria window 60 s / slack 1.3.
- Commit messages: concise, no Co-Authored-By trailer.
- No player saves exist: bump `FOpsSnapshot::Version` to 5 once; no migration of old offer fields.

## Review Focus

- **A day straddle in one Advance.** A long frame (or a test's big step) crossing 20:00 must advance each side at its own rate - pinned in Task 1 (`StraddlesDusk`).
- **Fee at a clamp / zero demand factor.** Club floor must still produce offers at 200% fee - pinned in Task 3 (`FloorHoldsAtMaxFee`).
- **Offer accepted in the same frame its countdown hits zero.** Accept must refuse a flight already Expired, and the countdown must skip an Accepted one - pinned in Task 4 (`AcceptAfterLapseRefused`).
- **Save mid-countdown then change speed.** Seconds left identical after load and independent of speed - pinned in Task 4 (`CountdownSurvivesSave`).
- **Airline with an empty admissible fleet for hours, then admissible.** No burst of banked offers - pinned in Task 3 (`NoBankedBurst`).

## Deviations from the spec found while planning

- **Inbox badge and "new offer" toast dropped.** The inbox card is always on screen (top right, `BuildHudLayer`) and there is no bar button to badge; the header "Offers 3/8" carries the count. Measured: no inbox entry in `BuildBarWidget.cpp`/`BuildActions.cpp`.
- **The generator was never saved.** `UOfferGenerator::Stream`'s comment says "SAVED" but the generator is not in `UOpsRuntime::Persistents()`. Task 3 makes it `IOpsPersistent` ("Offers") - the claim becomes true.
- **Contract stored as a duration.** `UFlight::ContractSeconds` (fixed at offer) so the row can show "airborne within" before accept; `AirborneBy = AcceptedAt + ContractSeconds`. Same value as the spec's formula.
- **Clock icon is a word.** "night" suffix on the clock text rather than a glyph the fallback font may lack.

---

### Task 1: Two-rate day on `USimClock`

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/SimClock.h`, `Private/Model/SimClock.cpp`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OpsDefinition.h` (UScenario)
- Modify: `Plugins/AirportOps/Source/AirportOps/Private/Present/OpsRuntime.cpp` (Attach copy + log)
- Modify (mechanical `RealSecondsPerGameDay = X` -> `SetUniformDay(X)`): `SimClockTest.cpp`, `OpsSaveTest.cpp`, `LedgerDeterminismTest.cpp`, `FuelServiceTest.cpp` comment, `Source/AirportMgr/SunDriverTest.cpp`, `SunPath.h` comment
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/SimClockTest.cpp`

**Interfaces — Produces:**
```cpp
UPROPERTY() double RealSecondsDaylight = 2400.0;  // real s for DawnHour..DuskHour at x1
UPROPERTY() double RealSecondsNight = 480.0;      // real s for DuskHour..DawnHour at x1
UPROPERTY() double DawnHour = 6.0;
UPROPERTY() double DuskHour = 20.0;
void SetUniformDay(double RealSecondsPerDay);     // splits so the rate is the same all day
bool IsDaylight(double TimeOfDaySeconds) const;   // [Dawn, Dusk)
bool IsDaylight() const { return IsDaylight(TimeOfDay()); }
bool IsPaused() const { return Speed == ESimSpeed::Paused; }
double GameSecondsPerRealSecond(double TimeOfDaySeconds) const; // at x1
double TimeScale() const;                         // Multiplier * GameSecondsPerRealSecond(TimeOfDay())
```
UScenario: `RealSecondsDaylight`, `RealSecondsNight`, `DawnHour`, `DuskHour` replace `RealSecondsPerGameDay`.

- [ ] **Step 1: Write failing tests** in `SimClockTest.cpp`:
  - `AirportOps.Model.SimClock.DaylightHourTakesItsShare`: defaults (2400/480, 6/20); `StartAtHour(7)`; `Advance(2400.0/14.0)`; expect `TimeOfDay()` == 8h ±1e-6 (one daylight hour = 2400/14 real s).
  - `...NightHourTakesItsShare`: `StartAtHour(22)`; `Advance(480.0/10.0)`; expect 23h.
  - `...StraddlesDusk`: `StartAtHour(19.5)`; real = half a day-hour + half a night-hour = `2400/28 + 480/20`; expect 20.5h.
  - `...UniformDayIsUniform`: `SetUniformDay(1200)`; `StartAtHour(0)`; `Advance(600)`; expect 12h; `TimeScale()` == 72 at both 03:00 and 13:00.
- [ ] **Step 2:** Build, run `-Filter AirportOps.Model.SimClock` - expect compile failure (members missing).
- [ ] **Step 3: Implement.** `GameSecondsPerRealSecond(t)`: daylight hours = `DuskHour-DawnHour` (3600 each), night hours = `24 - that`; return `hours*3600/RealSeconds` of the band `t` is in; guard `RealSeconds<=0` -> 0 (frozen, as today). `SetUniformDay(R)`: `RealSecondsDaylight = R*(Dusk-Dawn)/24; RealSecondsNight = R - RealSecondsDaylight`. `Advance(RealDelta)`: loop - rate = `Multiplier*GameSecondsPerRealSecond(TimeOfDay())`; if rate<=0 break; game seconds to next band edge (next Dawn or Dusk, wrapping 24h) = `Edge`; real needed = `Edge/rate`; if `Remaining <= needed` add `Remaining*rate` and stop, else add `Edge`, subtract needed, continue. Then the existing drain loop, unchanged. Keep the day-compression paragraph of the class comment, rewritten for two bands; keep "Airside agents run on Multiplier() alone".
- [ ] **Step 4:** UScenario fields (defaults 2400/480/6/20, WHY comment: night is a lull the player should not wait through). `OpsRuntime::Attach`: copy the four; scenario log line prints `%.0f/%.0f real s day/night`.
- [ ] **Step 5:** Mechanical test edits (`SetUniformDay`). Build twice; run `-Filter AirportOps.Model.SimClock` and `-Filter AirportOps.Save` and `-Filter AirportMgr.Sun`: all pass.
- [ ] **Step 6: Commit** `feat(clock): day and night run at their own rates`

### Task 2: Airline definition and `RateAt`

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/AirlineDefinition.h`
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OfferGenerator.h`, `Private/Model/OfferGenerator.cpp`
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OfferGeneratorTest.cpp` (remove the `OfferIntervalSeconds` cases, add rate cases)

**Interfaces — Produces:**
```cpp
// UAirlineDefinition (OffersPerDay and OfferWeight deleted)
UPROPERTY(EditAnywhere, Category="Demand") TArray<double> DemandCurve;       // 24 weights 0..1; empty = flat 1.0
UPROPERTY(EditAnywhere, Category="Demand", meta=(ClampMin="0.0")) double PeakOffersPerHour = 1.0;
UPROPERTY(EditAnywhere, Category="Demand", meta=(ClampMin="0.0")) double FloorOffersPerHour = 0.0;
UPROPERTY(EditAnywhere, Category="Demand") bool bIsFloor = false;
UPROPERTY(EditAnywhere, Category="Offer", meta=(ClampMin="1.0")) double OfferWindowSeconds = 60.0;  // REAL
UPROPERTY(EditAnywhere, Category="Offer", meta=(ClampMin="0.0")) double LeadTimeSeconds = 900.0;   // GAME
UPROPERTY(EditAnywhere, Category="Offer", meta=(ClampMin="1.0")) double TurnaroundSlack = 1.5;
UPROPERTY(EditAnywhere, Category="Offer") FString CallsignPrefix = TEXT("XX");  // "G-????" = tail pattern
double CurveAt(double TimeOfDaySeconds) const;   // linear between hourly weights, wraps 23->0

// UOfferGenerator
static double RateAt(const UAirlineDefinition& Airline, double TimeOfDaySeconds, bool bDaylight, double DemandFactor);
// = max(Peak * CurveAt * max(DemandFactor,0), bDaylight ? Floor : 0), offers per GAME hour
```

- [ ] **Step 1: Failing tests** (`AirportOps.Model.Offers.Rate.*`): `CurveInterpolates` (curve 0 at 07h, 1 at 08h -> 0.5 at 07:30); `FlatWhenEmpty`; `FeeScales` (factor 0.5 halves); `FloorHoldsInDaylight` (peak 0, floor 1, daylight -> 1); `FloorOffAtNight` (-> 0); `NegativeFactorIsZero`.
- [ ] **Step 2:** Build - fails (no `RateAt`).
- [ ] **Step 3:** Implement `CurveAt`, `RateAt`; delete `OfferIntervalSeconds` and `OffersPerDay`/`OfferWeight`. Rewrite `Fleet`'s comment reference `AirportAdmits` -> `CouldEverAdmit` (it names a function that does not exist). Keep the Elasticity/"am I full?" paragraph from `OfferIntervalSeconds` on `RateAt`.
- [ ] **Step 4:** No build or commit here: deleting `OfferIntervalSeconds`/`OffersPerDay` breaks `OpsRuntime::Attach`, which Task 3 rewrites. Tasks 2 and 3 are one build cycle and one commit ("a commit has been built").
- [ ] **Step 5:** `AirlineDefinitionTest.cpp`: replace `OffersPerDay > 0` checks with `PeakOffersPerHour > 0 || FloorOffersPerHour > 0` ("an airline that can never offer is not in the game") and require `DemandCurve.Num()` is 0 or 24.

### Task 3: Accumulator generation, persistent generator, runtime wiring

**Files:**
- Modify: `OfferGenerator.h/.cpp`, `Flight.h`, `OpsRuntime.h/.cpp`, `OpsSave.h` (Version 5)
- Test: `OfferGeneratorTest.cpp`, `OpsRuntimeTest.cpp`, `LedgerDeterminismTest.cpp`

**Interfaces — Consumes:** `RateAt`, `USimClock::IsDaylight/TimeOfDay`.
**Produces:**
```cpp
USTRUCT() struct FAirlineOffers { GENERATED_BODY()
  UPROPERTY() TObjectPtr<const UAirlineDefinition> Airline; UPROPERTY() TArray<FOfferCandidate> Fleet; };

USTRUCT() struct FAirlineOfferState { GENERATED_BODY()
  UPROPERTY() double Accumulated = 0.0; UPROPERTY() double Threshold = 1.0;
  UPROPERTY() bool bCouldCome = true; };   // for the transition log only

class UOfferGenerator : public UObject, public IOpsPersistent   // blob "Offers"
  UPROPERTY() TMap<FName, FAirlineOfferState> States;   // key: airline asset FName
  UPROPERTY() int32 DroppedOffers = 0;
  UPROPERTY() FRandomStream Stream;                     // now actually saved
  UPROPERTY() int32 MaxPendingOffers = 8;               // copied from UScenario
  UPROPERTY() double TaxiAllowanceSeconds = 600.0;      // copied from UScenario
  static constexpr double TickSeconds = 60.0;
  // One game minute. Emits 0..n offers, never more than MaxPendingOffers - PendingNow.
  TArray<UFlight*> TickMinute(const URoadNetwork& Network, const FVector2D& Focus,
      TArrayView<const FAirlineOffers> Airlines, const USimClock& Clock, int32 PendingNow,
      TFunctionRef<int32()> NextId);
  UFlight* MakeOffer(const URoadNetwork&, const FVector2D& Focus, const UAirlineDefinition&,
      const TArray<FOfferCandidate>& Admissible, double Now, int32 Id);  // Admissible pre-filtered
  static double TotalRateAt(TArrayView<const FAirlineOffers>, double TimeOfDay, bool bDaylight, double DemandFactor);
  static FString MakeCallsign(const FString& Prefix, FRandomStream& Stream);
// UFlight additions
  UPROPERTY() double OfferSecondsLeft = 0.0;  UPROPERTY() FString Callsign;
  UPROPERTY() double LeadTimeSeconds = 0.0;   UPROPERTY() double ContractSeconds = 0.0;
  UPROPERTY() bool bFloorAirline = false;
// UScenario additions: MaxPendingOffers = 8, TaxiAllowanceSeconds = 600
// UOpsRuntime: CandidatesByAirline() replaces CandidatesFromCatalog(); OfferTick() replaces GenerateOffer();
//   OfferHandle armed with Clock->Every(UOfferGenerator::TickSeconds, ...); LastOfferIntervalSeconds and
//   OfferIntervalSecondsForTest deleted; HasOfferScheduledForTest kept; GetAirlineOffers() for the strip.
```
`TickMinute` per airline, in order: key `Airline->GetFName()`; state = `States.FindOrAdd`; admissible = fleet filtered by `CouldEverAdmit`; if empty: `Accumulated = 0` (no banking), log on `bCouldCome` true->false naming first refusal, continue; log on false->true. `Accumulated += RateAt(...)/60`. While `Accumulated >= Threshold`: `Accumulated -= Threshold; Threshold = Stream.FRandRange(0.6, 1.4)`; if pending (PendingNow + emitted) >= Max: `++DroppedOffers`, log `Offers: inbox full, dropped %s %s`, continue; else `MakeOffer` with `Admissible[Stream.RandHelper(n)]`. First-seen state's `Threshold` drawn from the stream, not 1.0.

`MakeOffer` sets: Id, Airframe, AirlineName, TypeName, Callsign, Phase Offered, `OfferSecondsLeft = Airline.OfferWindowSeconds`, `LeadTimeSeconds = Airline.LeadTimeSeconds`, `ContractSeconds = LeadTime + TaxiAllowance + Airframe.TurnaroundSeconds * Airline.TurnaroundSlack`, `bFloorAirline`, `ApproachFocus`, `LandingFee` from Pricing. `ArrivesAt` = 0 until accept (Task 4 sets it).

`MakeCallsign`: prefix contains `?` -> each `?` becomes `'A' + Stream.RandHelper(26)`; else `Prefix + " " + FString::FromInt(100 + Stream.RandHelper(900))`.

Runtime `OfferTick`: guard target/network; focus from `DefaultApproachFocus`; `TickMinute(..., FlightBoard->PendingOfferCount(), [&]{ return FlightBoard->TakeNextId(); })`; each result `FlightBoard->AddOffer(*Clock, Offer)` then the existing `Offer %d: ...` log extended with callsign. Attach log: `Offers: ~%.0f expected today across %d airline(s)` (integrate `TotalRateAt` at 24 hour midpoints x 1 h) plus `(floor: %s)` naming any `bIsFloor` airline, or a Warning `Offers: no airline is the floor - the airport can go silent` if none. `Persistents()` adds `OfferGenerator`.

- [ ] **Step 1: Failing tests** (`AirportOps.Model.Offers.Generate.*`, world-free with `AirsideTestWorld`/test graph fixtures already used by `OfferGeneratorTest.cpp`):
  - `FlatSixPerHour`: flat curve, peak 6; 60 `TickMinute` calls; count in [5,7].
  - `PeakBeatsTrough`: curve 1.0 at 08h, 0.1 at 14h, peak 6: hour 08 count > hour 14 count.
  - `FloorHoldsAtMaxFee`: club peak 0 floor 1, Pricing multiplier 2.0 (`StepLandingFee(+1)` x10); 3 game hours daylight -> >= 2 offers.
  - `ClubQuietAtNight`: same club, 3 hours from 21:00 -> 0.
  - `NoBankedBurst`: airline inadmissible for 6 hours (no runway), then add runway; the next single minute emits <= 1.
  - `FullInboxDrops`: PendingNow = 8: 0 offers, `DroppedOffers` > 0.
  - `SameSeedSameSequence`: two generators, `Stream.Initialize(7)`, same inputs over 2 hours: identical (Callsign, TypeName) lists.
  - `FeeStepMovesCadence`: peak 6 flat; hour 1 count at fee 100%, `StepLandingFee(+1)` x10 (200%), hour 2 count < hour 1 count. (Regression for spec problem 2; verify it goes red by reading `DemandFactor` once outside the loop.)
  - `ContractFormula`: `ContractSeconds == Lead + Taxi + Turnaround*Slack`.
  - `CallsignPattern`: `G-????` yields `G-` + 4 upper letters; `CA` yields `CA ` + 3 digits.
- [ ] **Step 2:** Run `-Filter AirportOps.Model.Offers` - fail/compile fail.
- [ ] **Step 3:** Implement generator, flight fields, scenario fields, runtime. Delete old `MakeOffer(Fleet...)` overload, `LeadTimeSeconds`/`OfferLifeSeconds` on the generator. Move the "SAYS WHY, and names the aeroplane" comment + log to the transition log. Update `OpsRuntimeTest` (the case that ticked `OfferIntervalSecondsForTest`) to tick `UOfferGenerator::TickSeconds * 120` game seconds' worth of real time with a flat-curve test airline and assert >= 1 offer.
- [ ] **Step 4:** Bump `FOpsSnapshot::Version` to 5 with a line in its version history comment: "5: offers queue (generator blob, real-time countdown); older offer fields dropped - no player saves exist".
- [ ] **Step 5:** Build twice, run `-Filter AirportOps`: all pass.
- [ ] **Step 6: Commit** `feat(offers): daily demand curve per airline, flying-club floor, capped inbox`

### Task 4: Real-time countdown, lapse reasons, accept schedules from now

**Files:**
- Modify: `FlightBoard.h/.cpp`, `Flight.h`, `OpsRuntime.cpp` (Tick)
- Test: `FlightBoardTest.cpp`, `FlightSaveTest.cpp`, new `Plugins/AirportOps/Source/AirportOpsTests/Private/OfferCountdownTest.cpp`

**Produces:**
```cpp
UENUM() enum class ELapseReason : uint8 { None, Ignored, NeverAcceptable };
// UFlight: UPROPERTY() bool bWasEverAcceptable = false; UPROPERTY() ELapseReason LapseReason = None;
//          UPROPERTY() double AcceptedAt = 0.0; AirborneBy() const { return AcceptedAt + ContractSeconds; }
//          UPROPERTY() double AirborneAt = 0.0;   ExpiresAt DELETED
struct FOfferVerdict { EArrivalRefusal Why = EArrivalRefusal::None; bool bFuelServable = true;
                       uint32 BoardAt=0, GuidelineAt=0, OccupancyAt=0; bool bValid=false; };
// UFlightBoard
void TickOffers(const UGroundTraffic&, const URoadNetwork&, const USimClock&, double RealDeltaSeconds);
const FOfferVerdict& VerdictFor(const UGroundTraffic&, const URoadNetwork&, const UFlight&) const; // cached
// Offers() returns ascending OfferSecondsLeft (stable).
```
`TickOffers`: no-op if `Clock.IsPaused()`; snapshot `Flights`; for each Offered: `VerdictFor` (sets `bWasEverAcceptable` when `Why==None` - the verdict cache is `mutable TMap<int32,FOfferVerdict>`, the flag write goes through a non-const path inside TickOffers); `OfferSecondsLeft -= RealDelta`; `<=0`: `Phase=Expired`, `LapseReason = bWasEverAcceptable ? Ignored : NeverAcceptable`, `--OfferedCount`, log `Offer %d (%s) lapsed (%s)`, `MoveToHistory(*F, Clock.Now())`, `++RevisionCount`.

`Accept`: after `Reserve`: `AcceptedAt = Clock.Now(); ArrivesAt = AcceptedAt + LeadTimeSeconds;` then `Schedule`. `AcceptImmediate`: `LeadTimeSeconds = 0`, `OfferSecondsLeft = 1` (never ticks - accepted in the same call). `OnAgentPhase`: when phase becomes `Departing` and `AirborneAt <= 0`: `AirborneAt = Clock.Now()`. Delete `ScheduleExpiry`, `CancelExpiry`, `ExpiryHandles`, the Offered branch of `RearmSchedules` (its log line "lapsed on load" is removed: a countdown in real seconds cannot pass while the game is shut - say so in the PR), and the `ExpiresAt` fallback in `OnAfterRestore` (use `ArrivesAt`). `OpsRuntime::Tick`: after `Clock->Advance`, `FlightBoard->TickOffers(*Model, *Target->Network, *Clock, RealDeltaSeconds)` inside the existing model guard; rewrite the trailing comment.

- [ ] **Step 1: Failing tests** in `OfferCountdownTest.cpp` (`AirportOps.Model.Offers.Countdown.*`):
  - `DrainsInRealSeconds`: offer window 60; `TickOffers(...,10)` at X1 and at X4 -> 50 left both times (fresh boards).
  - `PausedDoesNotDrain`: `TogglePause()`; `TickOffers(10)` -> 60.
  - `LapseIgnored`: stand available; tick 61 -> Expired, `Ignored`.
  - `LapseNeverAcceptable`: no stand; tick 61 -> `NeverAcceptable`.
  - `AcceptAfterLapseRefused`: lapse, then `Accept` returns false and phase stays Expired.
  - `AcceptSchedulesFromNow`: accept at Now=T -> `ArrivesAt == T + LeadTimeSeconds`, `AirborneBy() == T + ContractSeconds`.
  - `OffersSortedByTimeLeft`: windows 120, 45, 60 -> Offers() order 45, 60, 120.
  - `CountdownSurvivesSave` (in `FlightSaveTest.cpp`): tick 20 of 60, save/restore board via `OpsSave::SerializeObject/DeserializeObject` + `RearmSchedules`, then `SetSpeed(X4)`, tick 10 -> 30 left.
- [ ] **Step 2:** Build twice (new file); run `-Filter AirportOps.Model.Offers.Countdown` - fail.
- [ ] **Step 3:** Implement. Fix `FlightBoardTest.cpp` cases that set/assert `ExpiresAt` or rely on `Clock.Advance` lapsing an offer: rewrite them to `TickOffers`.
- [ ] **Step 4:** Run `-Filter AirportOps`: all pass.
- [ ] **Step 5: Commit** `feat(offers): real-time offer countdown; lapses say why`

### Task 5: Fuel is a soft demand

**Files:**
- Modify: `FuelService.h/.cpp` (`CouldServe`), `OfferGenerator.cpp` (`IsPermanentRefusal`), `FlightBoard.h/.cpp` (`Fuel` pointer, verdict), `OpsRuntime.cpp` (wire `FlightBoard->Fuel = FuelService`)
- Test: `FuelServiceTest.cpp`, `OfferGeneratorTest.cpp`, `OfferCountdownTest.cpp`

**Produces:**
```cpp
// UFuelService: true if some stand this airframe's letter admits (StandAdmission::Judge) has a
// depot ChooseDepot would send from (Why == None, busy counts as servable).
bool CouldServe(const URoadNetwork& Network, const FAirframe& Airframe) const;
// UFlightBoard
UPROPERTY() TObjectPtr<UFuelService> Fuel = nullptr;   // null = fuel treated as servable (tests)
```
Verdict computes `bFuelServable = Fuel == nullptr || Fuel->CouldServe(Network, Flight.Airframe)` on the same revision keys. `IsPermanentRefusal`: move `NoStandServiceable` into the `return false` group with the comment "a service the airport cannot give is the player's to accept badly - spec 2026-09-28 section 5 - and C scores it".

- [ ] **Step 1: Failing tests:** `AirportOps.Fuel.CouldServe.NoDepot` (false), `.DepotJoined` (true, using the fixture `FuelServiceTest.cpp` already builds for a served stand); `AirportOps.Model.Offers.ServiceIsSoft` (`IsPermanentRefusal(NoStandServiceable) == false`); `AirportOps.Model.Offers.Countdown.VerdictReportsFuel` (board with `Fuel` set, no depot -> `bFuelServable == false`, `Why == None`, Accept succeeds).
- [ ] **Step 2:** Run - fail.
- [ ] **Step 3:** Implement. `CouldServe` loops `Network.GetEntities()` filtered by `IsStandCandidate()` and `StandAdmission::Judge(Stand, Airframe).IsAdmitted()`, calls `ChooseDepot(Network, FuelAnchorOf(Network, id), VehicleFor(Stand), DesignVehicleFor(Stand))`, returns true on `Why == EFuelRefusal::None`.
- [ ] **Step 4:** Run `-Filter AirportOps`: pass.
- [ ] **Step 5: Commit** `feat(offers): accept an aircraft you cannot fuel; the row says so`

### Task 6: Inbox viewmodel and widget

**Files:**
- Modify: `Source/AirportMgr/OfferViewModels.h/.cpp`, `OfferInboxWidget.h/.cpp`, `BuildBarWidget.cpp` (clock "night")
- Create: `Source/AirportMgr/DemandStripWidget.h/.cpp` (a `UWidget`-free painter: `UUserWidget` with `NativePaint` drawing 24 bars from samples)
- Test: `OfferViewModelsTest.cpp`, `OfferInboxWidgetTest.cpp`

**Produces (UOfferViewModel getters):** `GetCallsign()`, `GetFee()` (FText via `Pricing->Format`), `GetContract()` ("lands in 15 min · airborne within 1 h 10 min" - game time), `IsFuelServable()`, `GetSecondsLeft()` (int), `GetTimeLeftFraction()` (0..1 of the airline window - stored on flight as `OfferWindowSeconds` copy: add `UPROPERTY() double OfferWindowSeconds` to UFlight in this task), `GetAcceptLabel()` ("Accept" / "Accept (no fuel)"). Row reads `Board.VerdictFor` - the row's own three revision fields and `bWhyComputed` are DELETED (the cache moved to the board; keep the #169 paragraph on `FOfferVerdict`). `UOfferInboxViewModel`: `GetCapacity()` (generator `MaxPendingOffers`), `SampleDemand(int32 Count) -> TArray<double>` = `UOfferGenerator::TotalRateAt(Runtime->GetAirlineOffers(), hour midpoint, Clock.IsDaylight(t), Pricing->DemandFactor())`, `GetNowFraction()`.

Widget: head line = callsign + airline (Title) | seconds left (Label, amber < 30, Warning < 10 with alpha pulse `0.6+0.4*sin`); line 2 = type + code letter + fee; line 3 = contract (Body, muted); line 4 = chips "Fuel ✓/✗" (✗ in Warning) and "Tug" when `PushbackNeed == VehicleTug`; a 3 uu countdown bar (`UProgressBar`, fill = fraction, colour follows the seconds text); refusal line unchanged; Accept label from `GetAcceptLabel()`. Header "OFFERS" + "3/8". Demand strip under the header, 24 bars, night hours in `PanelDark`, now-marker in `Accent`. Clock: `BuildBarWidget::RefreshClock` appends " night" when `!Clock->IsDaylight()`.

- [ ] **Step 1: Failing tests:** `AirportMgr.Offers.ViewModel.AcceptLabelNamesMissingFuel`; `.ContractText` (lead 900, contract 4200 -> "lands in 15 min · airborne within 1 h 10 min"); `.RowsSortedByTimeLeft`; `.StripMatchesGenerator` (each sample == `TotalRateAt` at that hour - identity with the generator's function); `AirportMgr.Offers.Widget.HeaderShowsCapacity` ("1/8").
- [ ] **Step 2:** Build twice (new widget file), run `-Filter AirportMgr.Offers` - fail.
- [ ] **Step 3:** Implement; update the existing `OfferViewModelsTest`/`OfferInboxWidgetTest` cases that used `ExpiresAt`/ETA.
- [ ] **Step 4:** Run `-Filter AirportMgr.Offers` then full suite: pass.
- [ ] **Step 5: Commit** `feat(ui): offer rows show callsign, fee, contract, fuel and a countdown; demand strip`

### Task 7: Content - flying club and Cumbria

**Files:**
- Modify: `Tools/Python/build_airlines.py`
- Create (by script): `Content/Entities/DA_Airline_FlyingClub.uasset`; modify `DA_Airline_Cumbria.uasset`

- [ ] **Step 1:** Headless probe: print each `DA_Aircraft_Plane*` `display_name`, wingspan and `minimum_pavement`/surface so the fleet split is by fact. Club = single-engine grass singles (172, Cherokee, SR22, Meridian); Cumbria = twins/turboprops (Twin Otter, King Air, Caravan, Baron, Seneca, Saab 340).
- [ ] **Step 2:** Rewrite the dict to `(display, fleet, curve, peak, floor, is_floor, window, lead, slack, prefix)`. Club: curve 0 before 07h and from 20h, 0.6 at 07, 1.0 10-16, 0.7 17-19; peak 3, floor 1, is_floor, window 120, lead 600, slack 2.0, "G-????". Cumbria: 0 at 00-05, 0.8 at 06, 1.0 07-09, 0.4 10-15, 1.0 16-19, 0.5 20-21, 0.1 22-23; peak 2, floor 0, window 60, lead 900, slack 1.3, "CU". Keep every dated comment in the dict; add one dated 2026-09-28 for the split. Set properties by their Python names (`demand_curve`, `peak_offers_per_hour`, ...); remove `offers_per_day`/`offer_weight`.
- [ ] **Step 3:** Run `UnrealEditor-Cmd.exe <worktree>\AirportMgr.uproject -run=pythonscript -script=<script> -unattended -nosplash -nopause`; grep `MARKER:`; confirm both `.uasset` on disk with fresh mtimes.
- [ ] **Step 4:** Run `-Filter AirportOps.Content` (AirlineDefinition tests) then the full suite.
- [ ] **Step 5: Commit** `content(airlines): flying club is the floor; Cumbria flies the twins`

### Task 8: GDD, Check-Architecture, PR

- [ ] **Step 1:** GDD section 3 (services soft), section 6 (curve, floor, real-time window, cap) - dated lines pointing at the spec.
- [ ] **Step 2:** Check-Architecture rule for the shape removed: no `Clock->Every(` whose callback names `Offer` with an interval other than `UOfferGenerator::TickSeconds` - or state in the PR why no regex sees "a cadence computed once". (Recommend the latter plus the `FeeStepMovesCadence` test, which is the real guard.)
- [ ] **Step 3:** Count `UE_LOG(` and comment lines before/after in touched files; list removed logs (expiry-on-load, interval banner) and their replacements.
- [ ] **Step 4:** Full `Run-AirsideTests.ps1`; push; `gh pr create` filling the template (build line, test line, log deltas).
