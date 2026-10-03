# Airlines panel - design

2026-10-02. Status: rulings 1-3 by the user; section 2 decided autonomously while the user was away
(marked **[auto]** - revisable on review). Stacked on PR #532 (turnaround runs on-blocks -> off-blocks).

## Goal

One window that answers, per airline: how does it feel about my airport, what moved that over the
last week, which of its aircraft could come here and what would I have to build for the rest, what is
it offering me now, and which of its flights are with me now.

## Rulings

1. **Scope A** (user): show what the model tracks today. "Unlock more" = the per-type admission verdict
   with its figures ("Saab 340: runway too short - needs 1200 m, longest 800 m"). Building IS the unlock
   (progression ruling: capability gating). The airline ladder (contract / focus / base, spec
   2026-10-02-progression-and-fuel-supply §5) is its own later project; the panel gains a "next rung"
   section then. No placeholder ladder now.
2. **History = tally + trend** (user): per airline, the last 7 game days. Per day: satisfaction at the
   close, and per cause a count and summed delta. Panel shows a 7-point trend line and the tally summed
   over the 7 days.
3. **Layout = list + detail** (user): narrow airline list left (name, %, trend arrow), selected
   airline's detail right.

## 1. Model (AirportOps plugin, world-free tests)

### 1.1 Satisfaction causes become an enum

`EAirlineSatisfactionCause`: `OnTime, LateOffStand, OfferIgnored, OfferNeverAcceptable, LeftShortOfFuel,
CancelledAirportClosed, CancelledByPlayer, DailyDrift`. `UAirlineRoster::Apply` takes the enum plus its
free text (the text keeps its figures - "late off stand (25 min)"). `FAirlineSatisfactionChange` and
`FAirlineSatisfactionEvent` gain the enum. **Why:** a tally grouped by parsing free text is a second
source of truth that a reworded cause silently breaks. The `AIRSIDE_EXHAUSTIVE_SWITCH` pattern covers
the panel's cause -> label switch so a new cause is a build error there.

### 1.2 `UAirlineHistory` - daily tally per airline

- New `UAirlineHistory` (Model/), `IOpsPersistent`, blob "AirlineHistory".
- Per airline: a ring of `DaysKept = 7` `FAirlineDay { int32 Day; double CloseSatisfaction;
  TArray<FAirlineCauseTally{Cause, Count, SumDelta}> Tallies; }`. The last element is TODAY (open).
- `Record(AirlineId, Cause, Delta, NewSatisfaction)` adds to today's tally and updates today's
  running close value. `CloseDay(Day, Standings)` stamps each airline's close and opens the next day,
  dropping the oldest beyond 7.
- **[auto] FED BY THE ROSTER, NOT BY THE BUS.** Section 1 said "subscribes to the satisfaction event";
  that fails on the day boundary: the roster's daily drift is published while handling FDayEndedEvent, so
  a bus listener that also rolls its day on FDayEndedEvent would book the drift into the NEXT day
  (and the order of two Reaction-tier handlers is not something to lean on - multicast order memory).
  The roster is the only writer of satisfaction, so it calls `History->Record` from `Apply` and
  `History->CloseDay` at the end of `OnDayEnded`, after drift. Pattern: composition with a sole writer;
  the history stays its own class (its own save blob, its own reader), the roster just feeds it.
  `History` is a raw pointer set by UOpsRuntime like `Bus`; null in a bare NewObject (every call checks).
- Drift IS recorded in the tally (cause DailyDrift) even though `Recent` does not remember it - the
  panel's tally must sum to the real change, or the trend line and the tally disagree.
- A new game resets it beside `UAirlineRoster::ResetForNewGame`; a load with no blob starts empty.
- Readers: `GetDays(AirlineId)` (oldest first), `SummedTallies(AirlineId)`.

### 1.3 Admission verdict per fleet type

- `UOfferGenerator::FAdmissionCache` keeps, per fleet index, `FFleetTypeVerdict { EArrivalRefusal Why;
  FString Sentence; }` (None = admitted), filled by the SAME `CouldEverAdmit` call the cache already
  makes - `Admissible`, `FirstRefusal*` and `FleetShare` derive from it (FirstRefusal* may become a
  query over the array; keep `DescribeWhyNot`'s output identical).
- Public `GetFleetAdmission(FName AirlineId) -> TArray<FFleetAdmission{ const UAircraftType* Type;
  bool bAdmitted; EArrivalRefusal Why; FString Sentence; }>`, empty before the airline's first
  judgement. **Why one list:** the pick, the fleet share and the panel then cannot disagree.
- Sentences carry the figures (needed vs available): extend `CouldEverAdmit`'s sentence only where
  the planner already knows them; where it does not, the reason's wording stands - no second planner.
- Temporary refusals (RunwayOccupied, NoFreeStand) count as admitted, as they do for offers today.

### 1.4 Rate now

`UOfferGenerator` exposes `CurrentRate(const UAirlineDefinition&, const USimClock&) -> double` built
from `RateAt(... AirlineFactor(Airline))` - the generator's own expression, extracted, not copied - plus
the airline factor split for display (`AirlineFactorOf` = mood, `FleetShare` = share).

## 2. Presentation (AirportMgr game module) **[auto]**

- `UAirlinesPanelWidget : UAirportMgrPanelWidget`, `WantsWindow` (draggable, persisted like Ledger).
  `EHudWindow::Airlines` + name + `PanelFor` case + `CreateAll`/`WireWindows`, bar action
  `game.airlines` "Airlines" in the Game section, no key (Alerts' reason: free letters are scarce).
  The four lists that must agree (enum, name table, PanelFor, WireWindows) - follow the existing
  static_assert; add the window to whatever test enumerates windows.
- **View models** (plain UObjects, the OfferViewModels pattern; polled with a revision/memo key):
  - `UAirlineListViewModel` -> rows `{AirlineId, Name, SatisfactionPct, Arrow, bFloor}`; sorted floor
    first then by name; selection kept by AirlineId. (Final review ruling, 2026-10-03: the arrow is the
    INBOX's - the direction of the latest remembered change, `UOfferViewModel::MoodArrowOf` - not today
    vs yesterday's close, so the two windows never point opposite ways; the week's direction is the line.)
  - `UAirlineDetailViewModel` for the selected airline:
    - header: name, %, "~N offers/h now", "mood x0.9, N of M types can come".
    - trend: the oldest kept day's OPENING, then each kept day's close - up to 8 values for 7 days
      (missing days omitted, not zero-filled), so last - first equals the summed tally (final review I1).
    - tally rows: cause label, count, summed % - one row per cause seen in the window, largest
      |delta| first; DailyDrift last.
    - fleet rows: tick/cross, type display name, sentence for a cross.
    - offering now: that airline's Offered flights - callsign, type, seconds to answer (the inbox's own
      countdown text helper, not a second formatter). Read-only: accepting stays in the inbox, the one
      place that acts on offers.
    - flights with you: that airline's flights between Accepted and Departed - callsign, type, phase
      word, and the contract text the arrivals row uses (`ArrivalViewModels`' helper, not a copy).
- **Widgets:** UUiRow for list rows (selectable). New tiny `UUiSparkline` (UWidget/SLeafWidget,
  `OnPaint` -> `FSlateDrawElement::MakeLines`, colour from UUIStyle by meaning, 0..1 fixed scale with a
  faint 50% baseline). Everything else is text blocks in vertical boxes.
- Empty states: "not judged yet" for a fleet with no verdicts; "no history yet" before day 1 closes
  shows today's partial tally only.

## 3. Tests

- Model: cause enum carried on change and event; history - record, close day, 7-day cap, drift lands
  in the day it closes, save/load round trip, new-game reset; per-type verdicts - every index has one,
  pick draws only admitted, DescribeWhyNot unchanged, FleetShare unchanged; CurrentRate equals what
  TickMinute accrues.
- Composition (spawn/attach the runtime, per refactor contract "every seam gets a test"): the roster
  -> history feed is wired by UOpsRuntime (an off-blocks event through the bus lands in history).
- UI: list VM sort/selection/trend; detail VM rows from a fixture board/roster/history; window
  registered and toggled by `game.airlines`.

## Out of scope

Airline ladder/contracts/base; click-to-focus a flight; accepting offers from the panel; a per-flight
score breakdown; holding/taxi penalties.
