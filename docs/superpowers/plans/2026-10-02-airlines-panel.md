# Airlines Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Airlines window (list + detail) showing each airline's satisfaction, a 7-day trend and cause tally, per-fleet-type admission with the reason, its live offers and its flights at the airport.

**Architecture:** Model additions in the AirportOps plugin (cause enum, `UAirlineHistory` fed by `UAirlineRoster`, per-type verdicts in `UOfferGenerator`'s admission cache, a current-rate getter), then plain-UObject view models and a C++ panel in the AirportMgr game module following `LedgerPanelWidget`/`OfferViewModels`.

**Tech Stack:** UE 5.8 C++, UMG/Slate built in C++, UE automation tests.

**Spec:** `docs/superpowers/specs/2026-10-02-airlines-panel-design.md` (read it first).

## Global Constraints

- Read `CLAUDE.md` at the repo root first and obey it: WHY comments, `// ENFORCED BY:` for claims about other code, one log category per name, doc comment touches its declaration, Check-Architecture is the lint, never trust the runner's exit code.
- Worktree: `C:\repos\airportmgr2-slot6`, branch `feature/airlines-panel`, stacked on `feature/turnaround-on-blocks` (PR #532). Never checkout/pull in `C:\repos\AirportMgr2`.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-slot6\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE` (the flag is a user ruling: safe in a worktree only).
- Tests: `pwsh -NoProfile -File Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-slot6\AirportMgr.uproject -Filter <prefix>`; quote the `N test(s) run, N failed, N crashed` line. A NEW test .cpp needs two builds (the first says Succeeded without compiling it) - check the run count went up.
- Plan code is a sketch: VERIFY every signature against the live headers before writing it (memory: plans ship real defects).
- Commits: concise, NO `Co-Authored-By` trailer (user rule); end with `Claude-Session: https://claude.ai/code/session_01UYZQFAQcQPM5CDTcizdQmF`.
- Scratch files only under `C:\Users\daren\AppData\Local\Temp\claude\C--repos-AirportMgr2\f5207036-5836-4062-bb24-44060eaddb6b\scratchpad\airlines\`. Never kill processes by name.
- Window names, enum entries, PanelFor and WireWindows are lists that must agree - follow the existing static_assert.

## Review Focus

1. An airline with NO standing/history yet (fresh game, first minute): panel must show "no history yet" / "not judged yet", not crash or show 0%. Test in Task 5.
2. Day rollover while the panel is open: the trend gains a point, the oldest drops at 8 days, today's tally restarts. Test in Task 2.
3. Satisfaction clamped at 0 or 1: a change that does not move is not published; history must still match the real change (Delta after clamp). Test in Task 2.
4. Airline whose fleet is entirely refused: every row crossed, each with its own sentence; FleetShare 0; DescribeWhyNot unchanged. Test in Task 3.
5. Save/load mid-day: today's partial tally and the 7 days survive; a save with no "AirlineHistory" blob loads empty. Test in Task 2.

---

### Task 1: Satisfaction cause enum

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/AirlineRoster.h`, `Private/Model/AirlineRoster.cpp`, `Public/Model/OpsEventBus.h` (`FAirlineSatisfactionEvent`), `Private/Model/OpsEventBus.cpp` (Describe)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/AirlineRosterTest.cpp`

**Interfaces:**
- Produces: `UENUM() enum class EAirlineSatisfactionCause : uint8 { OnTime, LateOffStand, OfferIgnored, OfferNeverAcceptable, LeftShortOfFuel, CancelledAirportClosed, CancelledByPlayer, DailyDrift };` in `AirlineRoster.h` (UENUM so UHT sees it - memory: UHT cannot see a plain enum). `FAirlineSatisfactionChange::Kind` and `FAirlineSatisfactionEvent::Kind` of that type. `Apply(FAirlineStanding&, double Delta, EAirlineSatisfactionCause Kind, const FString& Cause, bool bRemember = true)`.

- [ ] Step 1: Add a test `AirportOps.Model.Airlines.CauseKindCarried`: for each handler (off-blocks on time, off-blocks late, offer ignored, never acceptable, short of fuel, closure cancel, player cancel, day end drift) drive the roster with the existing fixture and assert `Recent.Last().Kind` (drift: assert the published event's Kind via a bus recorder, since drift is not remembered).
- [ ] Step 2: Build, run `-Filter AirportOps.Model.Airlines`; expect compile failure/red.
- [ ] Step 3: Add the enum and fields; pass the Kind at every `Apply` call site (AirlineRoster.cpp lines ~50, 60, 86, 90, 125, 164, 178 - the cancel site picks ClosedAirport vs ByPlayer where it picks the text today).
- [ ] Step 4: Build, run filter; green. Run `-Filter AirportOps` to catch event-describe tests.
- [ ] Step 5: Commit `airlines: satisfaction cause is an enum, text keeps its figures`.

### Task 2: `UAirlineHistory`, fed by the roster

**Files:**
- Create: `Plugins/AirportOps/Source/AirportOps/Public/Model/AirlineHistory.h`, `Private/Model/AirlineHistory.cpp`
- Modify: `AirlineRoster.h/.cpp` (a `UAirlineHistory* History = nullptr;` raw pointer like `Bus`; `Apply` calls `History->Record` after the clamp, only when it moved - same condition as the publish; `OnDayEnded` calls `History->CloseDay(Event.Day ...)` AFTER the drift loop), `Public/Present/OpsRuntime.h/.cpp` (`CreateDefaultSubobject<UAirlineHistory>(TEXT("AirlineHistory"))`, set `Airlines->History` where `Airlines->Bus` is set and clear it where Bus is cleared, register as `IOpsPersistent` where the roster is, `ResetForNewGame` beside the roster's at ~:1036, getter `GetAirlineHistory()`)
- Test: Create `Plugins/AirportOps/Source/AirportOpsTests/Private/AirlineHistoryTest.cpp`; composition test in the existing OpsRuntime bus test file (find where `AirportOps.Present.Bus.*` tests live).

**Interfaces:**
- Consumes: `EAirlineSatisfactionCause` (Task 1).
- Produces:
```cpp
USTRUCT() struct FAirlineCauseTally { GENERATED_BODY()
    UPROPERTY() EAirlineSatisfactionCause Kind = EAirlineSatisfactionCause::OnTime;
    UPROPERTY() int32 Count = 0;
    UPROPERTY() double SumDelta = 0.0; };
USTRUCT() struct FAirlineDay { GENERATED_BODY()
    UPROPERTY() int32 Day = 0;                 // the clock's day number (check FDayEndedEvent's field name)
    UPROPERTY() double CloseSatisfaction = 0.0; // running value while open, the close once closed
    UPROPERTY() TArray<FAirlineCauseTally> Tallies; };
USTRUCT() struct FAirlineDays { GENERATED_BODY()
    UPROPERTY() FName AirlineId;
    UPROPERTY() TArray<FAirlineDay> Days; };   // oldest first; last = today (open)
UCLASS() class AIRPORTOPS_API UAirlineHistory : public UObject, public IOpsPersistent {
public:
    static constexpr int32 DaysKept = 7;
    void ResetForNewGame();
    void Record(FName AirlineId, EAirlineSatisfactionCause Kind, double Delta, double NewSatisfaction, int32 Today);
    void CloseDay(int32 ClosedDay, TArrayView<const FAirlineStanding> Standings); // stamps close, opens ClosedDay+1, trims to DaysKept
    const FAirlineDays* Find(FName AirlineId) const;
    TArray<FAirlineCauseTally> SummedTallies(FName AirlineId) const; // over every kept day
    // IOpsPersistent: SaveBlobName "AirlineHistory"; OnBeforeRestore resets
private:
    UPROPERTY() TArray<FAirlineDays> Airlines; };
```
  The roster needs today's day number for `Record`: take it from the clock the runtime already gives other model objects, or keep `CurrentDay` inside the history (set by `CloseDay`, starting at the clock's day at attach) - prefer the latter so `Record` is `Record(AirlineId, Kind, Delta, NewSatisfaction)`; pick one and document WHY.

- [ ] Step 1: Tests in `AirlineHistoryTest.cpp` (world-free, `NewObject`):
  - `AirportOps.Model.AirlineHistory.RecordTallies` - two OnTime +0.03 and one Late -0.04 -> today has OnTime{2,+0.06}, LateOffStand{1,-0.04}; close value = last NewSatisfaction.
  - `...CloseDayKeepsSeven` - close 9 days -> 7 kept, oldest is day 3, last is open with no tallies and CloseSatisfaction carried from the previous close.
  - `...DriftBelongsToTheDayItCloses` - roster with history: one late event, then `OnDayEnded`; the drift's DailyDrift tally is in the CLOSED day, not the new one (reason: the roster records before it closes).
  - `...ClampedChangeRecordsRealDelta` - satisfaction 0.99, OnTime bonus 0.03 -> recorded Delta 0.01; at 1.0 another OnTime records nothing.
  - `...SaveLoadRoundTrip` - use the existing IOpsPersistent round-trip helper (see AirlineRosterTest's save test) mid-day; tallies and days equal after.
  - `...NewGameResets` and `...UnknownAirlineGrowsARowOnlyFromTheRoster` (Record for an id never seeded is ignored? Decide: the roster only calls Record for seeded rows, so history may add rows on first Record - test whichever you choose, with the reason).
- [ ] Step 2: Composition test `AirportOps.Present.Bus.HistoryIsWired`: attach a runtime as the existing bus tests do, publish an `FFlightOffBlocksEvent` for a seeded airline, drain, assert `GetAirlineHistory()->Find(id)` has an OnTime or LateOffStand tally. Must fail if `Airlines->History` is not set.
- [ ] Step 3: Build twice (new .cpp), run `-Filter AirportOps.Model.AirlineHistory` and the composition test; red.
- [ ] Step 4: Implement. Header comment on UAirlineHistory states the pattern and the deviation from the spec's first draft (fed by the roster, not the bus - the day-boundary reason, copy it from the spec §1.2). `// ENFORCED BY: AirportOps.Model.AirlineHistory.DriftBelongsToTheDayItCloses` beside the claim.
- [ ] Step 5: Green; `-Filter AirportOps` green.
- [ ] Step 6: Commit `airlines: seven-day history of satisfaction causes, fed by the roster`.

### Task 3: Per-type admission verdicts and current rate

**Files:**
- Modify: `Plugins/AirportOps/Source/AirportOps/Public/Model/OfferGenerator.h` (FAdmissionCache ~:303; new public getters), `Private/Model/OfferGenerator.cpp` (~:105-205 cache fill and use; :305 DescribeWhyNot; FleetShare ~:63)
- Test: `Plugins/AirportOps/Source/AirportOpsTests/Private/OfferGeneratorTest.cpp`

**Interfaces:**
- Produces:
```cpp
USTRUCT() struct FFleetAdmission { GENERATED_BODY()
    FName TypeName;              // FOfferCandidate::TypeName (display name)
    bool bAdmitted = false;
    EArrivalRefusal Why = EArrivalRefusal::None;
    FString Sentence; };         // empty when admitted; the plan's sentence, else DescribeRefusal(Why)
TArray<FFleetAdmission> GetFleetAdmission(FName AirlineId) const;   // empty before first judgement; fleet order
double CurrentRate(const UAirlineDefinition& Airline, const USimClock& Clock) const; // offers per game hour now
double MoodFactor(const UAirlineDefinition& Airline) const;          // AirlineFactorOf or 1.0
```
  The cache gets `TArray<FFleetAdmission> Verdicts;` filled in the same loop as `Admissible` (one `CouldEverAdmit` per type, no extra searches - assert via `AdmissionChecksForTest()`). Replace `FirstRefusal/FirstRefusalSentence/FirstRefused` with a query over `Verdicts` (first not-admitted) so there is one list; `DescribeWhyNot` output must be byte-identical. `CurrentRate` is the EXACT expression TickMinute uses for accrual - extract it into one private helper both call; do not write it twice.

- [ ] Step 1: Tests:
  - `AirportOps.Model.Offers.FleetAdmission.EveryTypeHasAVerdict` - mixed fleet (one fits, one too long for the runway, one too wide for any stand, using the existing OfferGenerator fixtures) -> 3 rows in fleet order, correct bAdmitted/Why, non-empty Sentence on the crosses; `AdmissionChecksForTest()` equals fleet size (no extra searches).
  - `...AllRefusedEachHasItsOwnReason` (Review Focus 4) - FleetShare 0, DescribeWhyNot equals the first row's sentence.
  - `...EmptyBeforeJudgement`.
  - `...CacheRebuildRefreshesVerdicts` - lengthen the runway (bump guideline revision as existing tests do) -> the too-long type becomes admitted.
  - `AirportOps.Model.Offers.Rate.CurrentRateIsWhatAccrues` - CurrentRate equals RateAt(..., DemandFactor(), AirlineFactor(Airline)) at the clock's time; and equals the accumulator's per-minute increment x 60 over one TickMinute.
- [ ] Step 2: Build, run `-Filter AirportOps.Model.Offers`; red.
- [ ] Step 3: Implement. Keep every UE_LOG; keep comment-line count from falling.
- [ ] Step 4: Green; `-Filter AirportOps` green.
- [ ] Step 5: Commit `offers: a verdict for every fleet type, one list for pick, share and panel`.

### Task 4: `UUiSparkline` widget

**Files:**
- Create: `Source/AirportMgr/UI/UiSparkline.h/.cpp` (UWidget wrapping a private `SLeafWidget` subclass `SUiSparkline`), test in `Source/AirportMgr/UI/UiControlsTest.cpp` (or a new `UiSparklineTest.cpp` - two builds)

**Interfaces:**
- Produces: `UUiSparkline::SetValues(TArrayView<const double> Values01)`, `SetStyle(const UUIStyle*)`; draws a polyline over a 0..1 fixed scale with a faint 50% baseline; colour by meaning from UUIStyle (read the style for the right tokens - e.g. Ink for the line, InkMuted for the baseline). `ComputeDesiredSize` 120x28. Pure helper `static void UUiSparkline::LayOut(TArrayView<const double>, FVector2D Size, TArray<FVector2D>& OutPoints)` (clamps to 0..1, spreads x evenly, y=0 is top so value 1 -> y 0) so the geometry is testable without painting.

- [ ] Step 1: Test `AirportMgr.UI.Sparkline.LayOut`: 3 values {0, 0.5, 1}, size 100x20 -> points (0,20) (50,10) (100,0); 1 value -> a single point at mid x; values outside 0..1 clamp; 0 values -> no points.
- [ ] Step 2: Build, run; red. Step 3: implement (`OnPaint` -> `FSlateDrawElement::MakeLines` with LayOut's points; nothing drawn under 2 points except a dot). Step 4: green. Step 5: commit `ui: sparkline widget`.

### Task 5: View models

**Files:**
- Create: `Source/AirportMgr/AirlineViewModels.h/.cpp`, test `Source/AirportMgr/AirlineViewModelsTest.cpp` (two builds)
- Read for the pattern: `Source/AirportMgr/OfferViewModels.h/.cpp`, `ArrivalViewModels.h/.cpp` (reuse their contract/countdown text helpers - do not write a second formatter; if a helper is private, make it a public static and say why)

**Interfaces:**
- Consumes: roster `GetStandings()`, `UAirlineHistory` (Task 2), `GetFleetAdmission/CurrentRate/MoodFactor/FleetShare` (Task 3), `UFlightBoard` flights by phase, the catalog's `UAirlineDefinition`s (as `OfferViewModels` finds them).
- Produces:
```cpp
enum class EAirlineTrend : uint8 { Up, Down, Flat };
struct FAirlineListRow { FName AirlineId; FText Name; int32 SatisfactionPct = 0; EAirlineTrend Trend = EAirlineTrend::Flat; bool bFloor = false; };
struct FAirlineTallyRow { FText Label; int32 Count = 0; double SumDelta = 0.0; FText DeltaText; };   // "+18%"
struct FAirlineFleetRow { FText TypeName; bool bAdmitted = false; FText Reason; };
struct FAirlineOfferRow { FText Callsign; FText TypeName; FText Countdown; };
struct FAirlineFlightRow { FText Callsign; FText TypeName; FText Phase; FText Contract; bool bLate = false; };
struct FAirlineDetail { FText Name; int32 SatisfactionPct = 0; FText RateLine; FText FactorLine;
    TArray<double> Trend; bool bHasHistory = false; TArray<FAirlineTallyRow> Tallies;
    bool bJudged = false; TArray<FAirlineFleetRow> Fleet; TArray<FAirlineOfferRow> Offers; TArray<FAirlineFlightRow> Flights; };
UCLASS() class UAirlineListViewModel : public UObject { ... TArray<FAirlineListRow> BuildRows(const UOpsRuntime&) const; };
UCLASS() class UAirlineDetailViewModel : public UObject { ... FAirlineDetail Build(const UOpsRuntime&, FName AirlineId, double Now) const; };
```
  Pure static builders taking the model objects (not the runtime) so tests can feed fixtures; the runtime overloads forward. Trend: today's running value vs yesterday's close, Flat within 0.5 pp. Tally order: by |SumDelta| descending, DailyDrift last. Cause labels via an exhaustive switch (`AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN/END` - see Flight.cpp) so a new cause is a build error. Rate line "~3.1 offers/h now"; factor line "mood x0.9 · 3 of 5 types can come".

- [ ] Step 1: Tests (fixtures: NewObject roster/history/board/generator, as AirportOps tests do; reuse their helpers if exported, else small local ones):
  - `AirportMgr.Airlines.List.SortAndTrend` - floor first, then by name; trend Up/Down/Flat cases.
  - `AirportMgr.Airlines.Detail.NoHistoryYet` (Review Focus 1) - seeded airline, nothing happened: bHasHistory false, Tallies empty, bJudged false, % = start.
  - `...TallyOrderAndText` - drift last, "+18%" formatting, signs.
  - `...FleetRows` - one admitted, one refused with its sentence.
  - `...OffersAndFlightsFilteredByAirline` - two airlines' flights on one board; only the selected airline's Offered rows and in-progress rows appear; departed/cancelled excluded; contract text equals the arrivals row helper's output for the same flight.
- [ ] Step 2: Build twice, run `-Filter AirportMgr.Airlines`; red. Step 3: implement. Step 4: green. Step 5: commit `airlines: list and detail view models`.

### Task 6: The window, bar button, registration

**Files:**
- Create: `Source/AirportMgr/AirlinesPanelWidget.h/.cpp` (model on `LedgerPanelWidget` - read it and `AirportMgrPanelWidget.h` first)
- Modify: `Source/AirportMgr/BuildHudLayer.h` (`EHudWindow::Airlines` before `Count`), `BuildHudLayer.cpp` (`HudWindowNames`, `PanelFor`, `CreateAll`, `WireWindows`), `BuildActions.cpp` (`game.airlines` in EActionSection::Game after `game.alerts`, `EKeys::Invalid`, IsActive = `WindowShowing(Ctx, EHudWindow::Airlines)`, `HasRuntime`), and any test that enumerates windows/actions (grep `EHudWindow::Settings` and `game.alerts` in *Test.cpp).
- Test: a widget test beside `AlertsPanelTest.cpp`'s pattern, e.g. `Source/AirportMgr/AirlinesPanelTest.cpp` (two builds).

**Interfaces:**
- Consumes: Task 4 `UUiSparkline`, Task 5 view models, `UUiRow` for the list, `UUIStyle` tokens.
- Layout (user-chosen): horizontal box - left: vertical list of `UUiRow`s ("Flying Club  71% ▲"), selected row highlighted; right: scroll box with header (name, %), rate + factor lines, "Last 7 days" + sparkline, tally rows (label · count · delta coloured by sign: Positive/Warning tokens), "Fleet" rows (✔/✘ + name; reason under a ✘ in InkMuted), "Offering now" rows, "Flights with you" rows (late in Warning). Empty states: "No history yet", "Not judged yet", "No offers", "None at the airport". Use ASCII fallbacks if the font lacks ✔✘▲▼ - check the glyphs render in the existing UI first (grep the source for "▲" - the inbox satisfaction line already uses an arrow).
- Refresh: in the panel's tick, rebuild only when a memo key changes (roster revision/history day/board revision/selected id/minute of `Now`) - follow how ArrivalsPanel/OfferInbox avoid per-frame rebuilds.

- [ ] Step 1: Tests: `AirportMgr.Airlines.Panel.Registered` - `game.airlines` exists in the Game section and toggling it shows/hides `EHudWindow::Airlines`; `...Panel.SelectingRowShowsDetail` - with a runtime fixture of two airlines, selecting the second puts its name in the detail header; `...Panel.EmptyStates`.
- [ ] Step 2: Build twice, run `-Filter AirportMgr.Airlines`; red. Step 3: implement. Step 4: green.
- [ ] Step 5: Full suite at the tip: `pwsh -NoProfile -File Tools/Run-AirsideTests.ps1 -Project C:\repos\airportmgr2-slot6\AirportMgr.uproject`; quote the line. Check-Architecture PASS. UE_LOG count did not fall.
- [ ] Step 6: Commit `airlines: Airlines window - list, trend, tally, fleet, offers, flights`.

### Task 7: PR

- [ ] `git fetch; git rebase origin/feature/turnaround-on-blocks` (or origin/main if #532 merged - then rebase onto main), full suite again if anything moved, push, `gh pr create --base feature/turnaround-on-blocks` (or main), body from the PR template: build line, test line, "Stacked on #532", the [auto] decisions from the spec listed for the user to review, PIE verification steps (open Airlines from the bar's Game section; accept and fly a Cumbria flight; the tally gains "on time" or "late off stand"; Fleet shows ✘ rows with reasons). End body with `🤖 Generated with [Claude Code](https://claude.com/claude-code)`.
