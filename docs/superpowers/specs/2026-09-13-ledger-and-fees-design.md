# The Ledger, Fees and What Building Costs — Design

**Status:** DESIGNED 2026-09-13, not implemented. Slice C of M3.

Agreed in conversation 2026-09-13, after PR #140 (pushback and manoeuvring slice 1) merged.
Slice C of the five the flight-board spec named:

| Slice | Deliverable | State |
|---|---|---|
| A+B | A flight is a modelled thing; offers arrive, the player accepts one, an aeroplane turns up | shipped, PR #113 onward |
| **C (this spec)** | **Money: what a flight earns, what building costs, what owning it costs** | this spec |
| D | `UJobBoard` with depot bidding, absorbing `UFuelService`; goods depot and fleet | not started |
| E | `URunwaySequencer`: queues, separation, abstract holding, divert | not started |

**Parents:** `2026-09-05-game-systems-map-design.md` §4.2 and §3.6; `AirportManagerGDD.md` §11;
`2026-09-11-flight-board-and-offers-design.md` (the flight lifecycle these fees hang off);
`2026-09-12-game-ui-design.md` §9 (the reserved, empty ledger slot this fills).

## 0. Nothing here is law

Best understanding on the day it was written. Where it contradicts the systems map, this spec
wins for this slice and the map should be amended; where it contradicts the code, the code
wins and this spec is wrong. Say so rather than implementing around it.

Every figure in §7 is a FIRST PASS, chosen for internal coherence rather than from play. They
are meant to be tuned, and the spec says where each came from so a tune stays coherent.

## 1. Why

As of 2026-09-13 an offer arrives, the player accepts it, an aeroplane lands, taxis, parks, is
fuelled, manoeuvres off the stand and departs. None of it is worth anything. The player can lay
a hundred kilometres of taxiway for free, and can decline every offer forever and be no worse
off.

Three things are already sitting in the code waiting for this slice, each with a comment saying
so:

- `UFlight::LandingFee` and `ParkingFee` exist, are zero, and nothing reads them
  (`Flight.h:131`). `UOfferGenerator` deliberately leaves them zero and says why
  (`OfferGenerator.cpp:93`).
- `UScenario::StartingBalance` is authored and consumed by nobody.
- The HUD status strip holds a sized `USpacer` named `LedgerSlot`, reserved and deliberately
  empty (`BuildBarWidget.cpp:241`).

And one test named in the M1 plan was deferred to this slice for want of a ledger to compare:
"same seed, same inputs, same ledger".

## 2. What ships

A balance in the HUD. Offers in the inbox show what they pay, so accepting one is a decision
about money and not only about whether a stand is free. Landing, parking and fuelling credit
the ledger; building and owning the airport debit it. The build ghost shows the price before
the click, and refuses in place when the money is not there. The player can raise or lower
their landing fee, and airlines offer less when it goes up.

## 3. Decisions taken

### D1. One resolver, because prices are not constants

Costs and fees change during a game: research makes roads cheaper, a fuel contract makes fuel
cheaper, the player chooses what to charge for a landing. So no call site reads a base figure
off an asset and spends it. Every question goes through `UPricing`, which is the ONLY place a
base figure becomes a number anyone spends.

Today `UPricing` has exactly one live modifier — the player's landing-fee multiplier. Research
and contract modifiers plug into the same function in M4 without touching a call site. What
does NOT ship is a generic stacked-modifier engine with named sources and an ordering rule:
nothing but the fee lever could push one, and scaffolding with no publisher is the bug
CLAUDE.md names three times.

### D2. Base rates live on the thing, not in a table beside it

| Where | Field | Why there |
|---|---|---|
| `URoadProfile` | `CostPerMetre`, `UpkeepPerMetrePerDay` | A new taxiway width cannot be added without a price |
| `UEntityDefinition` | `PlacementCost`, `UpkeepPerDay` | Same, for stands and depots |
| `UAirsideSettings` | `ApronCostPerSquareMetre`, `ApronUpkeepPerSquareMetrePerDay` | See below |

A central cost table keyed by asset was the alternative and is rejected: adding a road profile
and forgetting its row would build free, with nothing anywhere to say so. That is the "lists
that must agree" failure this codebase has shipped three times.

**The apron is the one exception, and it is forced.** `FApronSurface` is an outline and a
material slot name; there is deliberately no per-apron asset, because bands and lanes are
meaningless for a polygon (`RoadApron.h:10`). So the apron rate goes to `UAirsideSettings`,
already this project's single door for a content default with no better home — with that reason
stated at the field, not left to be rediscovered.

**Two Airside assets now carry a number only AirportOps ever reads.** Deliberate, and it gets a
comment at each site: Airside owns the geometry and the profile, so Airside is the only layer
that can answer "how much of it is there", and putting the rate anywhere else means something
outside Airside has to know what a profile is made of.

### D3. `BuildCost` and `UPricing` are symmetric halves

- `BuildCost` (Airside, `Build/BuildCost.h`) answers **how much of it is there, at the authored
  rate**: `ForSegment`, `ForEntity`, `ForApron`, `DailyUpkeep(Network)`. It reads the rates in
  D2, and nothing else does.
- `UPricing` (AirportOps, `Model/Pricing.h`) answers **what we charge for that**: modifiers, the
  fee table, the refund fraction, the formatting.

Neither reads the other's figures. Airside never learns what a currency is; AirportOps never
learns what a profile band is.

### D4. `IBuildPurse`, a plain abstract class, because the seam carries five operations

```cpp
// Airside/Tool/BuildPurse.h - sibling to IRoadEditTarget, and a plain abstract class for the
// same reason that one is: nothing in Blueprint needs to see this seam.
struct FBuildQuote
{
    double BaseAmount = 0.0;

    /**
     * The URoadProfile or UEntityDefinition being placed - NOT a parallel enum of build kinds.
     * A research discount aimed at taxiways keys on the asset itself, so there is no second
     * list to keep in agreement with EPlaceableEntity.
     */
    const UObject* Source = nullptr;

    FText What;
};

class AIRSIDE_API IBuildPurse
{
public:
    virtual ~IBuildPurse() = default;

    virtual bool  CanAfford(const FBuildQuote& Quote) const = 0;
    virtual int32 Charge(const FBuildQuote& Quote) = 0;          // entry id, INDEX_NONE if refused
    virtual void  Reverse(int32 ChargeId) = 0;                   // undo
    virtual void  Credit(const FBuildQuote& Quote) = 0;          // demolish; the purse's own fraction
    virtual FText Describe(const FBuildQuote& Quote) const = 0;  // for the ghost's label
};
```

`UFlightBoard::Dispatcher` is a `TFunction`, and its comment argues against an interface for one
call site. This is not that shape: five operations that must all be bound together. Five
independently-bindable `TFunction` members would let a build charge while undo silently stopped
refunding, with nothing to say so.

`ULedger` implements it directly — no adapter class — exactly as `USimClock` and `UFlightBoard`
implement `IOpsPersistent`.

**A null purse means free.** `URoadBuildEdMode`, every existing tool test and a bare PIE session
keep building at no cost and need no change. One test asserts precisely that, because a default
that silently began charging would break the editor mode.

### D5. Undo reverses a transaction; demolish is a new one

They look symmetrical and are not, and separating them removes a whole field from the model:

- **Undo** reverses *the transaction that just happened*. It has the exact id, because
  `FRoadEditSnapshot` carries it, and puts back exactly what was taken. That is what undo means
  — the player is where they were.
- **Demolish** is a *new* transaction valuing the geometry at TODAY's price: quote it fresh,
  credit a fraction. Nothing has to remember what each segment cost, no `BuiltFor` field
  pollutes `FRoadSegment`, nothing extra enters the save, and scrap value tracks current prices
  rather than what was paid an hour ago.

`Credit` therefore takes no fraction. Airside says what was torn out; AirportOps decides what
that is worth back. The fraction is `UPricing`'s, and is a scenario figure.

### D6. The negative-balance lock falls out; it is not a second rule

The GDD says a negative balance locks placement and research, operations continue. `CanAfford`
refuses anything the balance cannot cover, so **building can never put the player under**. A
negative balance can arrive only from upkeep — and while negative, `CanAfford` is false for
everything. The GDD's rule is then the behaviour rather than a rule enforced somewhere else that
could disagree with it. Research does not exist yet and is not mentioned in code.

### D7. Late fines do not ship; the fuel service fee does instead

The slice list said "late fines". **Nothing in this build can be late.**
`FFuelDemand::TurnaroundEndsAt` is computed from the ACTUAL park time, and its comment records
that `UFlight::OffBlockAt` was deleted for exactly this reason: a promised time and an actual one
disagreed the moment an arrival ran late (issue #97). Re-adding the promise to have something to
fine against would re-open a question already settled, and would fine the player for a late
arrival they did not cause.

So the pressure comes from the per-service fee the GDD already lists instead: a fuelled aircraft
credits a fuel service fee; one that times out `Unserviceable` — no depot in range, grace expired
— credits nothing and departs anyway. **The forfeit is an entry that does not happen, never a
negative one.** Same pressure, on a failure the player genuinely causes, using state
`UFuelService` already tracks. It also makes a depot pay for itself, which is the decision the
fee should be attached to.

Real late fines arrive with contracts (M4) or the schedule grid (M5) — the systems that promise
a time.

### D8. The fee lever is neutral by design, and that is the point

`UPricing::LandingFeeMultiplier` is saved, defaults to 1.0, and `UOfferGenerator`'s rate scales
by `pow(Multiplier, -Elasticity)` — constant-elasticity demand, the textbook form, named as such
at the site.

**Elasticity defaults to 1.0.** At 1.0, fee times demand is flat: raising the fee earns more per
flight and loses exactly enough flights to cancel it. So the lever pays NOTHING while the airport
has spare stands, and pays real money once it is capacity-bound and turning away offers it could
not have served anyway. The decision becomes "am I full?", which is a question about the airport,
rather than a slider with one correct setting. Below 1.0, raising fees would always be right;
above 1.0, always wrong. Both are traps, and both would be discovered by the player as "the
slider has a best position".

It is a guess until played, so it is a scenario field and tunable without a rebuild.

### D9. The multiplier applies at OFFER time, not at landing

`UOfferGenerator` computes the landing fee and writes it to `UFlight::LandingFee` — the field
left sitting there for it — so the inbox row shows what the flight pays. Changing the lever
therefore affects NEW offers only, and what an accepted flight will pay never changes under the
player. A fee computed at touchdown would let the player accept cheaply and raise the price
afterwards, and the number in the inbox would have been a lie.

### D10. Upkeep posts one entry a day, and the ledger rolls up

Upkeep is one `USimClock::Every(SecondsPerDay)` callback posting a SINGLE entry for the day's
total, not one per object: a hundred-stand airport would otherwise write a hundred rows a day
into a saved array.

`ULedger` rolls entries older than `MaxDays` into one brought-forward entry, balance preserved
exactly. Append-only does not mean unbounded: a long game would otherwise carry tens of thousands
of rows through every save.

### D11. `OpsSave::Capture/Restore` stop growing positional parameters

They take four model objects by name today. `ULedger` and `UPricing` would make six. That growth
is exactly what `IOpsPersistent` and `FOpsSnapshot::Blobs` were introduced to stop (#105 item 8)
— the comment there already says so. This slice converts both to take a
`TArrayView<IOpsPersistent*>` plus the network, and the runtime owns the list.

No version bump. A missing blob already means "that system starts fresh", so a pre-ledger save
opens at the scenario's starting balance, which is the correct answer.

### D12. The ghost price needs no new sink method

`IToolPreviewSink::Label(At, Text, Style)` already exists, and `EPreviewStyle::Refused` is
already defined as "something the gesture cannot do, with the reason". So the price is a `Label`:
`Pending` style when affordable, `Refused` when not.

`IBuildPurse::Describe` returns the formatted `FText`, so **the currency symbol never enters
Airside**. `IRoadEditTarget` gains `GetPurse()` so a tool reaches it through
`FToolContext::Target`, the way it reaches everything else.

This keeps the sink's contract intact: the tool describes intent in road-plane coordinates naming
a MEANING, and "what this costs" is a meaning, not a presentation choice.

### D13. The currency is `¤`

U+00A4, the Unicode generic currency sign — the glyph whose entire purpose is to stand in for an
unspecified currency. Not a real one, so no country is implied, and universally present in fonts,
unlike an invented glyph. A field on `UPricing`, so it can change without a recompile.

## 4. The objects

**New, AirportOps `Model/`:**

- `ULedger : UObject, IOpsPersistent, IBuildPurse`. `TArray<FLedgerEntry>`, append-only. Balance
  is a fold; a running total is cached for the HUD, and a test re-folds the array and asserts the
  two agree.
- `FLedgerEntry`: `double At` (game seconds), `ELedgerCategory Category`, `double Amount`
  (signed), `FText What`, `int32 Id` so a reversing entry can name what it reverses.
- `ELedgerCategory`: `LandingFee`, `ParkingFee`, `ServiceFee`, `Placement`, `Refund`, `Upkeep`.
  **Six, each with a publisher in this slice.** Research, contract and fine categories arrive with
  the systems that raise them.
- `UPricing : UObject, IOpsPersistent`. The resolver of D1, and the saved home of
  `LandingFeeMultiplier`.

**New, Airside:**

- `Tool/BuildPurse.h`: `FBuildQuote`, `IBuildPurse`.
- `Build/BuildCost.h`: `ForSegment`, `ForEntity`, `ForApron`, `DailyUpkeep(Network)`.

**Changed:**

- `URoadEditFacade::CommitAndNotify(FRoadEditScope&, const FBuildQuote&)` — the single door every
  scope-committing mutator already passes through (`RoadEditFacade.h:210`).
- `FRoadEditSnapshot` gains `int32 ChargeId`.
- `IRoadEditTarget` gains `GetPurse()`.
- `UFlight` gains `ParkedAt`.
- `UOpsRuntime` gains a `ULedger` and a `UPricing` pointer, three lines in `Attach`, and no logic
  — as its own header promises ("it GROWS BY FORWARDING").

## 5. The flows

**Build.** `CommitAndNotify(Edit, Quote)`: `CanAfford` false abandons the scope through the path
that already exists, the edit rolls back, nothing notifies, and the tool had already said so in
the ghost. Otherwise `Charge`, and the returned id lands on the pending snapshot before
`CommitEdit` pushes it.

**Undo.** `Travel` (`RoadEditFacade.h:218`) reads the id off the snapshot it steps past and calls
`Reverse`. Redo re-charges as a fresh entry, and is refused if the money has been spent meanwhile
— consistent with every other refusal.

**Demolish.** `DeleteSlot` and its siblings quote the doomed geometry and call `Credit`.

**Drag.** `MoveNode` bypasses `CommitAndNotify` by design and is scope-free across frames. Without
a hook here the cost model has a hole big enough to drive through: build ten metres of taxiway,
drag it to two kilometres, free. So `BeginInteractiveEdit` captures the base quote of the incident
segments and `EndInteractiveEdit(true)` quotes again and charges the difference — or, when the
player cannot afford the extension, takes the `bKeep = false` path that already exists and
discards the drag.

**Landing fee.** Computed at offer time (D9), posted at `EFlightPhase::Landing`.

**Parking fee.** `UFlight::ParkedAt` set at `Parked`; posted at `TaxiOut` for `Now - ParkedAt`,
and recorded into the existing `UFlight::ParkingFee`. **At `TaxiOut` and not at `Manoeuvring`**,
even though the aeroplane has physically left the stand by then: a stand is occupied for the
whole of the push, nothing else can be allocated it, and only `TaxiOut` is reached by every
departure. `Manoeuvring` is not: `DepartAgent` measures the ground ahead, and an aeroplane parked
within `StraightOutDegrees` of its exit heading simply drives out and never enters the manoeuvre
at all (commit 021cc2e). Charging at a phase some flights never enter is a fee that silently goes
uncollected on exactly the layouts the player built best.

**Fuel service fee.** `UFuelService` credits it when a demand reaches `Served`. `Unserviceable`
credits nothing (D7).

**Upkeep.** One daily callback armed in `UOpsRuntime::Attach`, posting `BuildCost::DailyUpkeep`
priced through `UPricing`.

## 6. The HUD

- The `USpacer` at `BuildBarWidget.cpp:241` becomes the balance readout, right of the clock, red
  below zero through a `UUIStyle` role rather than a literal colour.
- The fee lever is **two entries in `BuildActions()`**, up and down, showing the multiplier as a
  percentage. Not a hand-added pair of buttons beside the generated ones: `BuildActions()` is
  already the one list the bar, the key bindings and the inspector all read, and a second list is
  this codebase's named recurring bug.
- The ghost carries its price (D12).

**Out of scope, named:** a ledger panel listing entries. It is worth having, and it is a new
panel; the balance and the ghost price are what make the money legible enough to play with.

## 7. First-pass figures

All chosen for internal coherence against `StartingBalance = ¤500,000`, and all meant to be tuned.
The derivation is given so a tune stays coherent.

**Pavement, priced per square metre and authored per profile as a per-metre figure:**

| Surface | ¤/m² | Width | ¤/m | Sanity |
|---|---|---|---|---|
| Service road | 5 | 6 m | 30 | |
| Taxiway | 13 | 15 m | 195 | |
| Taxiway | 13 | 23 m | 300 | 500 m of it is ¤150,000 |
| Taxiway | 13 | 30 m | 390 | |
| Apron | 15 | — | — | 100 m × 100 m is ¤150,000 |
| Runway | 25 | 45 m | 1,125 | 1,500 m of it is ¤1,687,500 — a mid-game goal, deliberately |

Runway pavement is the thickest and dearest per square metre; apron sits between taxiway and
runway; service road is cheapest. That ORDERING is the part to preserve when tuning.

**Upkeep per day: 0.1% of placement cost.** Authored as its own field on each asset, filled at
that ratio first time. 500 m of 23 m taxiway is then ¤150/day.

**Entities:** Code C stand ¤40,000 (¤40/day). Fuel depot ¤120,000 (¤120/day).

**The ICAO letter table is THE fee table.** Landing fee by
`IcaoCode::LetterForWingspan(Airframe.Wingspan)` — already the codebase's one ICAO table
(`Solve/IcaoCode.h`), so a new aircraft type is priced the moment it has a wingspan and there is
no per-type fee to author:

| Letter | Landing | Parking /h (÷10) | Fuel service (×0.5) |
|---|---|---|---|
| A | ¤150 | ¤15 | ¤75 |
| B | ¤400 | ¤40 | ¤200 |
| C | ¤1,200 | ¤120 | ¤600 |
| D | ¤2,600 | ¤260 | ¤1,300 |
| E | ¤4,500 | ¤450 | ¤2,250 |
| F | ¤7,000 | ¤700 | ¤3,500 |

Parking and fuel are FRACTIONS OF THE SAME ROW rather than two more tables to keep in agreement.
The letter-to-fee table lives in AirportOps: it is money, not aerodrome geometry, and `Solve/`
stays dependency-free.

**Other:** demolition refund 0.5. Elasticity 1.0 (D8). Ledger roll-up after 30 game days.

## 8. Testing

**World-free `Model/`:** the cached balance agrees with a re-fold; roll-up preserves the balance
exactly; fee by code letter; the elasticity curve at 0.5, 1.0 and 2.0; the refund fraction; a
reversing entry cancels its charge to the penny.

**Seams, one test each, per CLAUDE.md's refactor contract:** a fake `IBuildPurse` proving
`CommitAndNotify` charges, undo reverses, redo re-charges, demolish credits, the drag charges its
delta, the drag is discarded when unaffordable — and **a null purse builds free**.

**Composition, at the level of the composition:** spawn the actor with a runtime attached, build a
taxiway, assert the balance fell by the quote; run a whole accepted flight and assert landing,
parking and fuel entries landed in the right categories at the right phases.

**Determinism.** The test M1 deferred for want of a ledger: same seed, same inputs, same ledger.
This is the test that keeps wall time out of the sim, and it can finally exist.

## 9. Open questions

- Every figure in §7 is unplayed. The ORDERING claims are the ones to defend; the magnitudes are
  guesses.
- Elasticity 1.0 makes the lever neutral below capacity by design (D8). Whether that reads as
  "interesting" or as "the slider does nothing" is a play question.
- A ledger panel is deferred (§6). If the balance moves in ways the player cannot account for, it
  stops being deferrable.
