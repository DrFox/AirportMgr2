# One route policy — `ERouteErrand` and `FRoutePolicy`

*2026-09-21. Supersedes the routing policy scattered across ten call sites; amends
`2026-09-06-ground-traffic-design.md` §4.*

---

## 1. The problem

Aircraft taxi down the runway. Reported from play, 2026-09-21.

`RouteSearch::Find` is already the only pathfinder in the codebase — all ten callers go
through it. What is scattered is **policy**: what an actor, on a given errand, may drive on.
Each caller assembles that by hand on `FRouteQuery`, whose defaults are the *permissive*
ones: `AvoidRunways = None`, `Occupancy = nullptr`.

Six of ten sites set runway avoidance. Four do not, and get "runways are fine" without ever
saying so:

| Site | Says |
|---|---|
| `PushbackPlanner.cpp:202` — `TaxiOutRoute` | nothing |
| `PushbackPlanner.cpp:173` — push clearance | nothing |
| `GroundTrafficRebuild.cpp:609` — `ReResolvePlan` | nothing |
| `RoadEditFacadeSurfaces.cpp:541` — `FindRoute` | nothing |

`TaxiOutRoute` is the long taxi from push-end to the runway entry, and `FindRoute` backs
every player- and tool-issued route. Either reproduces the report.

Two failure modes follow, and the second is the one that matters:

1. **Forgetting is silent and permissive.** No call site declares "runways are fine for
   this"; four simply omit the field.
2. **The policy cannot be read in one place.** Nothing can check that ten sites agree,
   because there is no list to check them against. This is CLAUDE.md's own recurring bug —
   *"Lists that must agree are ONE list"*, *"Check where a list is CONSUMED"* — on its
   fourth outing.

The congestion knob has the same shape one step further along: §4 *states* the rule
("vehicles always, aircraft never") in a document and *implements* it four times.

### 1.1 What the code actually does, against what §4 says

§4 is wrong about vehicles, and the deviation is deliberate and load-bearing.
`FuelService.cpp:230` routes with **no** occupancy weight:

> NO OCCUPANCY WEIGHT, deliberately. Which depot is nearest is a fact about the airport's
> SHAPE, not about who happens to be on the road this instant; a congestion-weighted length
> would make the chosen depot flicker between ticks and the log unreadable.

Nor is "comparison versus drive" quite the axis, tempting as it looks.
`ArrivalPlanner::ChooseStand` produces the route an arrival actually *drives*, and its query
carries no occupancy at all — the table is consulted afterwards, to skip a stand somebody is
on, never to weight an edge.

The axis that survives all four sites is **whether the route may still change**:

- A route **fixed when it is issued** — a clearance, a pushback, a comparison between
  candidates — costs shape only. Weighting it by who is on the road this instant makes the
  answer flicker between ticks without ever changing what gets driven.
- A route **being re-chosen for an agent already under way** — a deadlock replan, a rebuild
  re-resolve — costs held length too. That is the whole point of replanning: the ban removes
  the one edge the caller knows is hopeless, the congestion term stops the new route being
  the next queue along.

§4 is amended accordingly in §9 below.

---

## 2. Decisions

1. **The errand is the axis.** A named `ERouteErrand` says what the route is *for*;
   everything policy-shaped is derived from it. Not vehicle type — see §2.1.
2. **One table, `FRoutePolicy::For(ERouteErrand)`**, a public named type with its own tests,
   not a `constexpr` buried in `RouteSearch.cpp`.
3. **Runway is filter AND penalty, layered.** Errands that must never use a strip keep the
   hard filter; everything else pays a large multiplier. A forgotten errand degrades to a
   detour, not a taxi down the runway.
4. **`Unset` is the default and the search refuses it**, loudly, in `LogAirside`.
5. **Bans and wingspan stay per-call.** A deadlock resolver's banned edge is per-incident,
   not per-errand; a table row for it would be a row of one.

### 2.1 Why an errand enum is not the mistake `ETraversalClass` warns about

`ETraversalClass`'s own comment forbids the obvious neighbouring design:

> What they do is a service role, on an entity anchor, and it is deliberately not this enum —
> otherwise this grows with every vehicle type in the game and gets consulted by pathfinding
> for no reason.

That warning is about **who the actor is**. `ERouteErrand` names **what the route is for**,
which is the one thing pathfinding genuinely must consult — it is the question the policy
answers. The test that keeps the two apart: adding a vehicle type must not add an errand. A
catering truck and a fuel truck are both `VehicleToJob`.

---

## 3. `ERouteErrand`

`Model/RoutePolicy.h`, a new header in the plugin. Its own file rather than an addition to
`RouteSearch.h`, so `FuelService` in AirportOps includes the policy without dragging the
search in, and so the table's test has one obvious subject.

```cpp
UENUM()
enum class ERouteErrand : uint8
{
    /** Never valid. The search refuses it - see §6. */
    Unset = 0,

    /** An arrival from a runway exit to a stand. Never touches a strip. */
    ArrivalTaxiIn,

    /** A departure from a stand to its runway entry. Never touches a strip. */
    DepartureToEntry,

    /** A departure backtracking to a threshold. The ONE errand that must use the strip. */
    DepartureBacktrack,

    /** Pushback's clearance route, off the stand and clear of the departure's own arm. */
    PushbackClear,

    /** The taxi from where a push ends to the runway entry. */
    PushbackTaxiOut,

    /** A deadlock or failed-edge replan, mid-taxi. May use a runway end that is FREE. */
    Replan,

    /** A graph rebuild re-resolving a surviving plan onto new handles. */
    RebuildReResolve,

    /** A vehicle driving to or from a job. Re-chosen on dispatch, so it takes the table. */
    VehicleToJob,

    /** Comparing candidates - which depot, which stand. Shape only; see 1.1. */
    CandidateComparison,

    /** A route the player or a tool asked for directly. */
    PlayerIssued,

    /**
     * No policy: plain shortest path, runways free, no congestion. TESTS AND TOOLS ONLY.
     *
     * It exists so a graph-shape test ("is B reachable from A") does not have to pick a
     * production errand whose policy it does not care about and would silently inherit.
     * Check-Architecture.ps1 fails the build if this appears outside a *Tests/ module or
     * Tool/ - the permissive default is allowed to survive only where it is NAMED.
     */
    GraphProbe,
};
```

---

## 4. `FRoutePolicy` — the one table

```cpp
/** Whether this errand's cost reads the occupancy table. See §1.1 for why this is not
 *  simply "vehicles yes, aircraft no". */
UENUM()
enum class EOccupancyUse : uint8
{
    /** Cost is shape only. A clearance, or a comparison that must not flicker.
     *  The search REFUSES a query that supplied a table anyway - see §6. */
    Never,
    /** Cost includes held length. The search REFUSES if no table was supplied. */
    Required,
};

USTRUCT()
struct AIRSIDE_API FRoutePolicy
{
    GENERATED_BODY()

    UPROPERTY() ERunwayAvoidance Avoidance = ERunwayAvoidance::All;
    UPROPERTY() EOccupancyUse    Occupancy = EOccupancyUse::Never;

    /**
     * Multiplier on a runway-derived edge's length, applied only when Avoidance leaves the
     * edge usable. 1.0 means no penalty.
     */
    UPROPERTY() bool bPenaliseRunways = false;

    /** The table. Every errand has exactly one row; see the completeness test in §8. */
    static FRoutePolicy For(ERouteErrand Errand);
};
```

`bPenaliseRunways` is a flag, not a number: the number itself is
`FTrafficRules::RunwayPenalty`, so it sits with `CongestionWeight` in the Details panel and
can be tuned on a placed actor without a rebuild. The policy says *whether* the penalty
applies; the rules say *how much*.

**The table:**

| Errand | Avoidance | Penalty | Occupancy |
|---|---|---|---|
| `ArrivalTaxiIn` | `All` | — | `Never` |
| `DepartureToEntry` | `All` | — | `Never` |
| `DepartureBacktrack` | `None` | no | `Never` |
| `PushbackClear` | `All` | — | `Never` |
| `PushbackTaxiOut` | `All` | — | `Never` |
| `Replan` | `Held` | yes | `Required` |
| `RebuildReResolve` | `Held` | yes | `Required` |
| `VehicleToJob` | `All` | — | `Required` |
| `CandidateComparison` | `All` | — | `Never` |
| `PlayerIssued` | `None` | yes | `Never` |
| `GraphProbe` | `None` | no | `Never` |

Penalty reads "—" where `Avoidance` already removes every runway edge: the multiplier can
never be reached, and writing `yes` there would be a value no code path can observe.

`DepartureBacktrack` takes no penalty because the backtrack's *whole purpose* is the strip —
charging it would make a legal backtrack lose to nothing at all, since there is no
alternative to lose to.

`ArrivalTaxiIn` is `Never`, which is what `ChooseStand` ships today: its query carries no
occupancy, and the table is read separately to skip held stands. That is not an oversight
being preserved — a stand chosen by congestion would be re-chosen every tick until the
aircraft committed, which is §1.1's flicker wearing a different hat. If an arrival's taxi is
ever to be weighted, it is `Replan` that should do it, once the aircraft is under way.

`PlayerIssued` keeps `None` rather than `All`: a player who clicks two points across a runway
is stating an intent, and refusing it outright reads as a broken tool. The penalty makes the
route go round when going round exists, and takes the strip when it does not. This is the one
row where the graceful-degradation argument does the work.

---

## 5. The cost function

`RouteSearch.cpp`'s `EdgeCost` becomes:

```
cost = Length * (bRunwayEdge && Policy.bPenaliseRunways ? Rules.RunwayPenalty : 1.0)
     + CongestionWeight * HeldLengthOn(EdgeId, QueryingAgent)
```

Still additive and non-negative over the straight-line heuristic, so the heuristic stays
admissible and the first pop stays optimal — the property §4 established and this must not
weaken. The multiplier is `>= 1.0`, enforced by `ClampMin` on the property, for the same
reason: a multiplier below 1 would make an edge cheaper than its own chord and break
admissibility silently.

### 5.1 "Is this edge on a runway" must not be asked per relaxation

`URoadNetwork::IsRunwaySegment` is a slot lookup plus a profile resolve. Asking it on every
relaxation of every edge is exactly the complaint `FGuidelineEdge::Length` was cached to fix
(#171) — a node is relaxed several times before `Closed` catches it.

**Answer: a per-search memo, not a cached UPROPERTY.**

`ExpandNode` already threads `TMap<int32, bool> RunwayInUse` from `RunSearch`/`FindToGoals`
for exactly this reason. A sibling `TMap<int32, bool> IsRunwaySeed`, threaded the same way,
memoises `IsRunwaySegment` per seed within one search.

Rejected: a `bool bOnRunway` cached on `FGuidelineEdge` beside `Length`. `Length` is
invalidated by the three writers that can move an edge's endpoints or `Control`, and that set
is closed. `bOnRunway` depends on the *profile*, which changes when a road is re-profiled —
a different and open invalidation trigger, on serialised data, with no writer positioned to
catch it. A stale `true` there would refuse taxiways for the rest of the session. The memo
has no invalidation problem because it does not outlive the search.

---

## 6. Enforcement

`FRouteQuery` gains `UPROPERTY() ERouteErrand Errand = ERouteErrand::Unset;` and
`UPROPERTY() FRoutePolicy Policy;`, with `For()` taking the errand:

```cpp
static FRouteQuery For(ERouteErrand Errand, FGuidelineNodeId Start, FGuidelineNodeId Goal,
                       const FAirframe& Airframe, ETraversalClass Class);
```

`RunSearch`, `Find` and `FindToGoals` each refuse two things, before any expansion:

```cpp
if (Query.Errand == ERouteErrand::Unset)
{
    UE_LOG(LogAirside, Error, TEXT("Route query with no errand (%s -> %s); refusing"), ...);
    return FRoutePlan();                       // Result stays NoStart
}
if ((Query.Policy.Occupancy == EOccupancyUse::Required) != (Query.Occupancy != nullptr))
{
    UE_LOG(LogAirside, Error, TEXT("Errand %s and the occupancy table disagree; refusing"), ...);
    return FRoutePlan();
}
```

**The occupancy check runs both ways, deliberately.** `Required` without a table is the
obvious half. `Never` *with* a table is the other, and it is the more useful of the two: a
caller that went to the trouble of supplying occupancy believes it is being weighted by it,
and silently ignoring the pointer would leave that caller reasoning about a cost term the
search never applied. Two of today's ten sites hold an occupancy table and deliberately do
not pass it (`FuelService`'s depot choice, `ChooseStand`); under this rule that stays a
statement rather than an omission.

Both refusals are `Error`, not `Warning`: warnings do not fail automation tests here
(memory: `unreal-warnings-do-not-fail-automation-tests`), and a silent permissive route is
precisely what this design exists to stop.

Runtime refusal rather than private fields: a `USTRUCT` needs a default constructor for
reflection, so `FRouteQuery{}` stays constructible whatever the access modifiers say.
Making the fields private would hide them from the four hand-built sites without stopping
a fifth from appearing — and would cost the reflection the Details panel reads.

`Check-Architecture.ps1` gains one rule, for `GraphProbe` only: it fails if the token appears
in a file outside a `*Tests/` module or a `Tool/` directory. The lint is narrow deliberately —
it guards the one named escape hatch, rather than trying to text-match every way a query can
be built.

---

## 7. Migration

Ten production sites. Every one keeps the policy it has today except the four that never
declared one, and `FuelService`'s two, which split by §1.1.

| Site | Today | Errand | Changes behaviour? |
|---|---|---|---|
| `ArrivalPlanner.cpp:36` | `All`, no congestion | `ArrivalTaxiIn` | no |
| `DeparturePlanner.cpp:88` | `All` | `DepartureToEntry` | no |
| `DeparturePlanner.cpp:106` | `None` | `DepartureBacktrack` | no |
| `PushbackPlanner.cpp:173` | *unset* | `PushbackClear` | **yes** — gains `All` |
| `PushbackPlanner.cpp:202` | *unset* | `PushbackTaxiOut` | **yes** — gains `All` |
| `GroundTrafficRebuild.cpp:95` | `Held` + congestion | `Replan` | penalty only |
| `GroundTrafficRebuild.cpp:609` | *unset* + congestion | `RebuildReResolve` | **yes** — gains `Held` |
| `FuelService.cpp:234` | `All`, no congestion | `CandidateComparison` | no |
| `FuelService.cpp:332` | `All`, no congestion | `VehicleToJob` | **yes** — gains congestion |
| `RoadEditFacadeSurfaces.cpp:541` | *unset*, congestion if not aircraft | `PlayerIssued` (param) | **yes** — gains penalty; see §7.1 |

`FuelService.cpp:332` is the truck's route home. It is being chosen now, for a truck about to
drive it, so it takes the table — the flicker argument covers the depot *choice* above it,
not a route already committed to.

### 7.1 `FindRoute` has no production caller

`URoadEditFacade::FindRoute` is reached only from `ARoadNetworkActor::FindRoute`, and the
only callers of that are four tests. It is a tool- and Blueprint-facing seam with nothing on
the C++ side of it today.

That matters for the aircraft/vehicle branch inside it. The branch is §4's rule implemented
locally, and `TrafficForwardersTest.cpp:167-168` exists to pin it — a van and an aeroplane
routed between the same two nodes, asserting they are costed differently. Deleting the branch
outright would delete that test's subject.

So: **the branch is replaced, not removed.** `FindRoute` grows an errand parameter defaulting
to `PlayerIssued`, and supplies occupancy from `TrafficModelProvider` whenever the resolved
policy says `Required` — the class test disappears, the errand decides. The forwarders test
then passes `VehicleToJob` for the van and `PlayerIssued` for the aeroplane, and goes on
asserting the same difference it always did, now against a rule that is written down once.

`IRoadEditTarget` implementors and `ARoadNetworkActor::FindRoute` forward the new parameter
unchanged; the default keeps every existing caller compiling.

### 7.2 Tests

~40 hand-built `FRouteQuery` in test files migrate to `GraphProbe` unless the test is about
policy, in which case it names the errand it means. `RouteSearchTest.cpp` (11 queries) is
graph-shape throughout and takes `GraphProbe` wholesale; `RouteStepDistanceTest.cpp:204-207`
is explicitly about avoidance and names errands instead.

---

## 8. Tests

Each fails if the thing it names is unwired — CLAUDE.md's seam rule.

- `RoutePolicy.EveryErrandHasARow` — iterates `ERouteErrand` by reflection and asserts
  `For()` returns a row distinguishable from a default-constructed one. Catches an errand
  added without a policy, which would otherwise inherit `All`/`Never` by accident.
- `RoutePolicy.MatchesCallSitesAsShipped` — asserts the table's value for each of the six
  errands whose sites already declared a policy equals what that site set on 2026-09-21.
  This is the refactor contract: it fails if the migration quietly changed a rule it
  promised not to.
- `RouteSearch.RefusesUnsetErrand` — a query with no errand returns an invalid plan and
  logs. Uses an unbuffered log spy (memory: `unreal-log-spy-must-be-unbuffered`).
- `RouteSearch.RefusesOccupancyMismatch` — both directions: a `Required` errand with no
  table, and a `Never` errand handed one. The second half is the one that would rot if only
  the first were written.
- `RouteSearch.RunwayPenaltyPrefersTheDetour` — a graph where the runway is shorter than a
  parallel taxiway. Asserts the route takes the taxiway with the penalty on, and the runway
  with it at 1.0. **Measures the rule, does not name it** (memory:
  `a-green-test-may-measure-nothing`): with the penalty at 1.0 the test must go red.
- `RouteSearch.RunwayPenaltyStillRoutesWhenItIsTheOnlyWay` — the degradation `PlayerIssued`
  exists for.
- `RouteSearch.RunwaySeedIsResolvedOncePerSearch` — a counter beside
  `NodeVisitCountForTest`, asserting §5.1's memo is actually consulted. Without it the memo
  is an optimisation nobody can prove is wired.
- `PushbackPlanner.TaxiOutAvoidsTheStrip` — the reported bug, pinned. Written first, red
  before the change.
- `Traffic.HeadOnReplansRoundBarHolder` and the existing `RouteSearchTest` suite must stay
  green unchanged; they are the evidence that `Replan`'s row is right.

---

## 9. Amendment to `2026-09-06-ground-traffic-design.md` §4

§4's "Who routes with it" paragraph reads *"Vehicles always. Aircraft never at dispatch."*
That was true of the two callers that existed on 2026-09-06 and has not been true since
`FuelService` landed on 2026-09-07 — see §1.1. It is replaced by:

> **Who routes with it.** `FRoutePolicy::For(Errand).Occupancy`, and the answer is checked
> both ways — a `Required` errand with no table is refused, and so is a `Never` errand that
> was handed one. `Required` for a route being re-chosen for an agent already under way;
> `Never` for a route fixed when it is issued, which includes every clearance and every
> comparison between candidates. Vehicle-versus-aircraft is not the axis: a fuel truck
> choosing a depot is `Never`, an arrival taxiing in is `Never`, and both of an aircraft's
> replan paths are `Required`.

---

## 10. Risks and unjudged figures

- **`RunwayPenalty` is a number nobody has judged, AND IT IS STILL UNJUDGED AS SHIPPED.**
  It went in at `10.0` on the grounds that a taxiway detour is rarely ten times the strip it
  parallels, which is an argument, not a measurement. The suite proves the MECHANISM (a
  detour of ~34000 uu beats a strip of ~22000 at ten, and loses at one) and proves nothing
  about a real airport. The map check was NOT done - see section 12.
- **Five sites change behaviour.** Four gain a restriction they never declared; the fifth
  gains congestion. Tests asserting a route across a strip through those paths will move.
  Each one that moves is reported with which errand changed it and why — none is quietly
  relaxed. `TrafficForwardersTest.cpp:167-168` moves by construction (§7.1) and must go on
  asserting the van and the aeroplane are costed differently; if that assertion cannot be
  kept, the errand list is wrong and not the test.
- **A full rebuild is required.** New `UENUM`s, a new `USTRUCT` and new `UPROPERTY`s on
  `FRouteQuery` and `FTrafficRules`; Live Coding does not cover any of them. The editor
  must be closed, or the work done in a worktree with `-NoHotReloadFromIDE`.
- **`FRouteQuery::For`'s signature changes**, so every caller including tests must be
  touched in the same commit. A commit that splits this is a commit that does not build.

## 11. Out of scope

- Runway *clearance* proper. `ERunwayAvoidance::Held` reads the occupancy table because
  there is no sequencer yet; `URunwaySequencer` (M3) is where "cleared onto the strip"
  eventually lives, and this design does not anticipate it.
- Weighting by any other surface class — apron speed limits, service-road preference. The
  penalty mechanism generalises, but nothing has asked for it.
- Turn cost. `AirsideSettings.cpp:74` claims "a route search costs a turn by Wingspan";
  wingspan is a *filter* (`ExceedsWingspan`), not a cost. The comment is corrected in
  passing; no turn cost is added.

---

## 12. Still outstanding at merge

Everything in sections 1-11 is implemented and covered by the suite (716 tests, 0 failed).
Two things are deliberately NOT done, and neither can be done from a headless run:

1. **The in-editor repro of the original report.** A full departure cycle on the starter map,
   confirming a pushback's taxi-out no longer runs along the strip, with
   `python Tools/Mcp.py shot after-fix.png` as the evidence. The suite pins the POLICY ROW
   (`Airside.Model.RouteSearch.ErrandsThatGainedAFilter`), not the route a real pushback
   produces: `PushbackPlanner` is private to the plugin and cannot be called from a test
   module, and every pre-existing fixture's edges are hand-authored with no `DerivedFrom`,
   so none of them could route along a strip even before this change. That is also why no
   existing test moved when five call sites changed behaviour.

2. **Judging `RunwayPenalty` against a real layout.** Issue a `PlayerIssued` route between
   two points whose shortest path runs lengthways along a runway with a parallel taxiway
   available. If it takes the taxiway, 10.0 holds for this airport; if not, raise
   `FTrafficRules::RunwayPenalty` **on the placed actor instance**, not in the constructor -
   an editor-set UPROPERTY overrides the constructor default, so read the instance when
   reporting what worked.
