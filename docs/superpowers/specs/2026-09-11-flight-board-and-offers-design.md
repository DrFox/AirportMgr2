# The Flight Board and the Offer Inbox — Design

**Status:** DESIGNED 2026-09-11, not implemented. Slice A+B of M3.

Agreed in conversation 2026-09-11 after PR #68 (service connections) merged and was
PIE-verified. M3 as the systems map states it is nine systems; this spec is the first two
of five slices, each of which gets its own spec, plan and playable result:

| Slice | Deliverable |
|---|---|
| **A+B (this spec)** | A flight is a modelled thing; offers arrive, the player accepts one, an aeroplane turns up |
| C | `ULedger`, fees, late fines, HUD clock and balance |
| D | `UJobBoard` with depot bidding, absorbing `UFuelService`; goods depot and fleet |
| E | `URunwaySequencer`: queues, separation, abstract holding, divert |

**Parents:** `2026-09-05-game-systems-map-design.md` §3.1–3.4 and §4.1 (this slice is that
subset, with the deviations in §3 below); `2026-09-06-ground-traffic-design.md` (claims and
the occupancy table); `2026-09-07-service-connections-design.md` (the lane a truck reaches a
stand by).

## 0. Nothing here is law

Best understanding on the day it was written. Where it contradicts the systems map, this
spec wins for these two slices and the map should be amended; where it contradicts the code,
the code wins and this spec is wrong. Say so rather than implementing around it.

## 1. Why

As of 2026-09-11 the airside half of a flight already runs unaided, on the player's own
level: land, vacate, taxi in, park, shut down, a fuel truck routes from the depot and
services it, the aircraft turns round and departs. The log for a whole cycle is in PR #68.

What does not exist is the GAME around it. The aeroplane appears because the player pressed
`7`. Nothing chose it, nothing paid for it, nothing decided which stand, and nothing is
worse off if the player never presses the key again. Every M3 system keys off a flight being
a thing the model owns rather than an agent id, so that is slice A; and the first decision
the player can actually make about the airport's traffic is accepting an offer, so that is
slice B. They ship together because A alone changes nothing visible.

## 2. What ships

An offer appears in an inbox: airline, aircraft type, ICAO code letter, an ETA. Accepting it
reserves a stand and schedules the arrival on the sim clock; at the ETA the aeroplane flies
the cycle that already works. Offers the airport cannot handle are shown un-acceptable with
the reason in the player's words ("the runway is 1 100 m short", "no Code C stand free").
Declining drops the offer; ignoring it lets it expire.

## 3. Decisions taken

### D1. Capability is the EXISTING planner asked speculatively, not a new query

§3.1 describes `FAirportCapability` as a fresh pure function. Two thirds of it already
exist and are better than a rewrite:

- `AirsideCapability::Summarise(Network)` returns `FAirsideCapability` — every runway with
  length, direction and profile, every stand with its design wingspan and anchor roles. That
  is the cheap filter: an airline whose fleet needs 2 400 m is not asked about again.
- `ArrivalPlanner::Plan(Network, Near, Airframe, Occupancy)` already answers "can THIS
  airframe land here, and if not why not" as a pure function, with `EArrivalRefusal` naming
  all seven reasons — `NoRunway`, `RunwayTooShort`, `NotAdmitted`, `NoExit`,
  `NoRouteToStand`, `RunwayOccupied`, `NoFreeStand`. That vocabulary IS the inbox's un-acceptable reason, and
  `ArrivalPlanner::DescribeRefusal` already renders each as a player-facing sentence.

So capability in this slice is a WRAPPER, not a rival evaluator:

- **Offer generation** runs `Plan` with NO occupancy - "could this field ever take this
  aeroplane" - and drops candidates whose refusal is permanent (`RunwayTooShort`,
  `NotAdmitted`, `NoExit`, `NoRouteToStand`). Transient refusals (`RunwayOccupied`,
  `NoFreeStand`) are still offered: they clear on their own, and the offer is answered
  minutes before it lands.
- **Acceptability** runs the same `Plan` WITH the live occupancy table. If it refuses, the
  offer is un-acceptable and `DescribeRefusal` says why.

**Amended 2026-09-11, after the first PIE session.** This section originally had generation
filter on `Summarise` alone - longest runway and stand wingspans - on the grounds that
generation is cheap and acceptance is dear. That shipped an inbox in which every Accept was
greyed out: `Summarise` knows nothing about `RunwayAdmission`, so it offered A320s to
`M_Starter`'s 15 m strip, which admits a 15 m wingspan. **A filter that disagrees with the
gate behind it is worse than no filter** - it fills the inbox with decisions the player is
not allowed to make. One evaluator, asked twice with different occupancy, is the rule.

A second "what can the airport handle" function would be a second source of truth, and the
two would drift the way the Piper's figures did at seven call sites. The building half of
§3.1 (service roles offered, Control queue depth) is not built here: buildings are M4. When
it lands it JOINS this, it does not replace it.

### D2. A stand is reserved by a CLAIM in the occupancy table, not by a new table

`ArrivalPlanner::ChooseStand` already skips a stand whose `PoseNode` is held:

```cpp
if (Occupancy != nullptr && Occupancy->IsHeld(FTrafficResource::OfNode(Stand.PoseNode), ExcludingAgent))
```

`FTrafficOccupancy::IsHeld` returns true for ANY other holder's claim, reservation
(`bOccupied == false`) or body. So the allocator reserves by making exactly that claim, and
the planner honours it with no new code path and no exclusion-set parameter threaded through
a pure function. This is what `FTrafficResource`'s own header asks for: one table, one
`TryClaim` rule, for a junction and a landing alike.

An accepted flight has no agent yet, and claims are keyed by `int32 AgentId`. Agent ids are
allocated `NextAgentId++` from 1, so **flight reservations use the NEGATIVE of the flight
id**. The table is mechanism, not policy — it never asks what an agent is — and the sign
keeps the two id spaces provably disjoint without a registry to keep in step. On dispatch,
the flight releases its reservation in the same call that dispatches the agent, so no other
flight can be offered the stand in between; single-threaded, same frame, no window.

### D3. A reservation is keyed on the stand ENTITY and re-applied after a rebuild

`GroundTrafficRebuild` calls `FTrafficOccupancy::ReleaseGuidelineClaims()`, which removes
every `Edge` and `Node` claim — "a set of resources ceasing to exist". The player editing a
taxiway would therefore silently drop every stand reservation, and the node ids would not
survive anyway.

So the flight's saved truth is the stand's `FEntityInstanceId`, never a `FGuidelineNodeId`,
and `UFlightBoard` re-claims each reserved stand's pose node after a rebuild. The test that
pins it edits the network with flights reserved and asserts the reservations are still
honoured. This is a case where the code is right and the runtime would be wrong; it is
written down because nothing about `ReleaseGuidelineClaims` announces it to a caller.

### D4. The player cannot over-commit

Accept reserves a stand, or the offer is not acceptable and says so. §3.2's `Diverted`
phase and the inbound patience timer therefore have nothing to do in this slice and are not
built: with reservation at Accept, an accepted flight always has somewhere to go. Divert
arrives with the sequencer (slice E), where holding and separation make it meaningful.

The pressure this leaves is legible and is the one we want early: more stands, more flights.

### D5. An offer carries an ETA; the clock schedules the arrival

`USimClock::At(GameTime, Callback)` already exists. Accepting schedules the dispatch. This
makes the clock matter, lets offers stack into a rush, and gives the inbox an answer to
"when does a stand free" — the reserving flight's phase says so. Arrival-on-accept was
rejected as leaving the clock doing nothing.

### D6. `UFlightBoard` is the ONLY caller of `DispatchArrival`

Key `7` (`aircraft.land` in `BuildActions`) stays, because a debug spawn that needs no inbox
is worth keeping, but it creates a FLIGHT with an immediate ETA rather than dispatching an
agent itself. Two doors onto arrival is how this codebase has shipped three
lists-that-must-agree bugs; there is one door.

### D7. Airlines are real definition assets

`UAirlineDefinition : UOpsDefinition`, loaded by `UOpsCatalog::All<T>()` like every other
definition — the base class's own comment already names airlines as an intended subclass.
Fields: display name, fleet (`TArray<UAircraftType*>`), offer weight, and the service
demands its flights bring. Two or three authored to start.

**Gotcha to honour:** `UOpsDefinition::GetPrimaryAssetId` derives the type from the class
name minus its prefix, so `DefaultGame.ini` needs a `PrimaryAssetTypesToScan` entry for
`AirlineDefinition` or the catalog loads nothing and reports no airlines at all.

### D8. Fees are modelled now, shown in slice C

Each offer computes and stores its landing and parking fee from the airline and the type.
Nothing displays them and nothing banks them until `ULedger` exists. They are carried on
`UFlight` rather than recomputed later so that slice C is a column in a widget and a ledger
post, not a redesign.

### D9. Phases deferred

`EFlightPhase` is `Offered, Accepted, Inbound, Landing, TaxiIn, Turnaround, TaxiOut,
Departing, Departed` plus `Declined` and `Expired`. `Pushback` waits for the job board
(slice D); `Diverted` and `Cancelled` wait for the sequencer (slice E). A phase nothing can
enter is a lie in an enum.

### D10. The UI stack is MVVM plus `UListView`, not HTML

Considered and rejected: `WebBrowserWidget`. It is alive in 5.8 (only
`UWebBrowserAssetManager` carries a 5.8 deprecation), but it is CEF in a separate process
rendering to a texture, every model read becomes a JS bridge, and the widget tests that
already run headlessly — `BuildBarWidgetTest`, `InspectorWidgetTest`, `BuildActionsTest` —
would have nothing to test. A UI the test harness cannot see is a UI that rots silently.

What ships instead:

- **`ModelViewViewModel`** (Epic's "UMG Viewmodel", **Beta** in 5.8) enabled in
  `AirportMgr.uproject`, with `ModelViewViewModel` in the game module's Build.cs. A
  viewmodel derives `UMVVMViewModelBase` and pushes changes with
  `UE_MVVM_SET_PROPERTY_VALUE`, which broadcasts the field to whatever is bound.
- **`UListView`** for the offer rows. `SetListItems` takes `TArray<UObject*>` and `UFlight`
  is already a `UObject`, so the board's offers go in with no adapter. Rows are virtualized,
  which matters when the inbox is busy.
- **`UMVVMViewListViewBaseClassExtension`** hands each row's entry widget its own entry
  viewmodel — the engine's intended path for a list of viewmodels, not a thing to hand-roll.

**Viewmodels live in the game module beside the widgets, never in `Model/`.** `UFlight` and
`UFlightBoard` must not depend on the MVVM runtime, for the same reason Airside `Model/`
includes nothing above it: a model that knows about its view cannot be tested without one.
The viewmodel observes `UOpsEvents` and the board, and exposes flat display fields.

**Two hazards to write down now**, because both have cost this project a session before:

- MVVM bindings are authored in the Widget Blueprint's Viewmodels panel, so **a renamed or
  retyped C++ viewmodel field needs the BP recompiled and resaved** or the old binding runs
  against the new class. This is the stale-Blueprint failure the project notes already warn
  about, in a new place.
- The plugin is **Beta**. If a binding misbehaves, the fallback is a plain C++ widget
  reading the board directly, as `InspectorWidget` does today — not a redesign.

A sortable multi-column table is NOT built here. UMG has no column table; the engine's real
one is Slate's `SHeaderRow` with `SMultiColumnTableRow`, which the editor's own panels use.
The inbox is a handful of rows with two buttons and does not need it. It is the right answer
for the **flight board proper** — every live flight, sortable by ETA, stand and airline —
which arrives with the HUD in slice C.

## 4. The model

`Plugins/AirportOps/Source/AirportOps/`, `Model/`, world-free and testable with `NewObject`:

- **`UFlight`** — airline, aircraft type, `EFlightPhase`, ETA (game seconds), off-block time,
  reserved stand `FEntityInstanceId`, live agent id (`INDEX_NONE` until dispatch), fees.
- **`UFlightBoard`** — owns live flights; subscribes to `UOpsEvents::OnAgentPhaseChanged` and
  maps Airside's `EAgentPhase` onto `EFlightPhase`; the only caller of `DispatchArrival`.
- **`UOfferGenerator`** — runs on `USimClock::Every`; per airline, filters the fleet against
  `FAirsideCapability`, emits `Offered` flights with an ETA and an expiry.
- **`UStandAllocator`** — first-fit smallest stand whose design wingspan admits the airframe,
  nearest-exit as tiebreak; reserves at `Accepted`, releases at `TaxiOut`.

`UOpsRuntime` gains `UFlightBoard` and `UOfferGenerator` as subobjects and GROWS BY
FORWARDING, exactly as it did for `UFuelService`. The allocator is the board's, not the
runtime's: nothing else allocates.

Phase mapping, which is the seam a test must fail on if unwired:

| `EAgentPhase` | `EFlightPhase` |
|---|---|
| `Arriving` | `Landing` |
| `Taxiing` (inbound) | `TaxiIn` |
| `Parked` | `Turnaround` |
| `Taxiing` (outbound) | `TaxiOut` |
| `Departing` | `Departing` |
| `Gone` | `Departed` |

Inbound and outbound taxi are the same agent phase; the flight's own phase disambiguates,
because the board knows whether it has parked yet. That asymmetry is why the mapping lives
on the board and not in a free function.

## 5. Save and load

`FOpsSnapshot` gains a `Flights` byte array and `Version` goes to 2. The save rule already
established holds: a model object's non-Transient `UPROPERTY`s ARE its saved state.

**`USimClock` deliberately does not save its callback queue** (see the class comment). An
accepted flight's scheduled arrival is therefore NOT restored by restoring the clock: the
flight stores its ETA as a game time, and `UFlightBoard` re-arms `Clock->At()` for every
`Accepted` flight on load. Test: save with a flight inbound, reload, advance the clock, and
watch it arrive. Without that test this fails silently and only in a saved game.

## 6. Presentation

Game module, following `BuildBarWidget` and `InspectorWidget` — UMG C++ base, Blueprint
widget for layout — with the MVVM stack of D10:

- A bottom-bar section (horizontal, sectioned, the established Cities-Skylines shape) with
  an offers button and a count badge, the badge bound to the viewmodel's pending count.
- `UOfferInboxViewModel` — the list of offers, the pending count, and the Accept/Decline
  commands. `UOfferViewModel` per row — airline, type, ICAO code letter, ETA as a string,
  whether it is acceptable, and the refusal sentence when it is not. No fee column until
  slice C, though `UFlight` already carries the figure (D8).
- A `UListView` of offers, each row's entry widget given its `UOfferViewModel` by the
  ListView extension.
- An un-acceptable offer is disabled and carries `ArrivalPlanner::DescribeRefusal`'s
  sentence.
- No widget and no viewmodel mutates the model. Accept and Decline are viewmodel commands
  that call the board, which is the one door for undo, save and tests.

## 7. Tests

World-free `Model/` tests: phase transitions; the ETA schedule; first-fit allocation; an
offer filtered out by a short runway; expiry.

Seam tests at the composition, because that is the level the refactor contract asks for:

- Accept an offer, advance the clock, and assert `DispatchArrival` was genuinely called on a
  real `ARoadNetworkActor` — the test that fails if the board is never wired to the clock.
- A real agent phase change drives the flight's phase (fails if the `UOpsEvents`
  subscription is dropped).
- Two flights are never allocated the same stand, and the second offer reports `NoFreeStand`
  rather than silently double-booking.
- Editing the network while flights hold reservations leaves the reservations honoured (D3).
- Save with an inbound flight, reload, and it still arrives (§5).
- The offer viewmodel's pending count follows the board when an offer is accepted — the
  test that fails if the MVVM field is set without `UE_MVVM_SET_PROPERTY_VALUE` and so
  never broadcasts.

## 8. Out of scope, recorded so it is not forgotten

Divert and inbound patience (slice E). Pushback (slice D). Fees banked, late fines and
reputation (slice C). The Control building and queue depth, and the building half of
capability (M4). Contracted flights and the schedule grid, which §3.4 says supersede
first-fit allocation (M5). Go-around, wind and runway direction: out of v1 per the map.
