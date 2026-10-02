# Space-time taxi planning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Aircraft taxi on space-time plans (SIPP over a reservation table) whose ORDER is enforced at run time, so two aircraft can no longer meet head-on on a two-way stretch and jam.

**Architecture:** Four stacked PRs. PR 1 (this plan's detailed part) adds two world-free model pieces - `FTaxiReservations` (per-resource ordered windows) and `FTaxiPlanner` (SIPP earliest arrival over the table's free intervals) - plus the seams they need to REUSE existing authorities rather than copy them: RouteSearch's edge admissibility (extracted into `FRouteEdgeFilter`), RouteSearch's polyline weld (`RouteSearch::PlanFromSteps`), the claim pass's box rule (`FTrafficRules::IsBox`), and `FSpeedProfile` timing (`BuildPiece`, `SecondsToDrive`). Nothing is wired into traffic in PR 1.

**Tech Stack:** UE 5.8 C++, `Airside` plugin `Model/`, UE automation tests (`Airside.Model.TaxiPlan.*`), `Tools/Check-Architecture.ps1`.

**Spec:** `docs/superpowers/specs/2026-10-02-taxi-planning-design.md`

## The four PRs

1. **Reservations + planner, model only, nothing wired** (this plan, detailed below).
2. **Order enforcement + clearances** - `FPassingOrder` from the table; one more refusal in `FClaimPass` ("not my turn: waiting for X"); `UTaxiPlanning` as a `UGroundTraffic` subobject; arrival clearance (`ClearanceFor` needs a taxi-in plan) and pushback gated on a departure plan; arrival priority (revoke an unstarted departure); release when the tail clears. THE behaviour change; carries the headline no-deadlock test on `M_ScaleGatwick` (40 and 80 mov/h, 2 h, seeded delays, mutation proof with order disabled). Adds the planning log category and the Check-Architecture rule "only `UTaxiPlanning` writes `FTaxiReservations`" (see Task 5 for why it is not in PR 1).
3. **Re-time, revoke, layout edit, unplanned fallback + alert** - lag > knob re-times (same order, later times); edit freezes planning, re-plans moving aircraft, drops failures to UNPLANNED with one alert.
4. **UI** - inspector clearance/"Now:" lines, selected-aircraft map route/holds/waiting-on through `IToolPreviewSink` meanings, arrivals-panel reasons.

PRs 2-4 are detailed by later agents against the code that then exists.

## Global Constraints

- Scope: AIRCRAFT ONLY (spec ruling 1). PR 1 queries `ETraversalClass::Aircraft`.
- A resource is "a guideline edge (a two-way edge is ONE resource, both directions) and a node" (spec §1). `FTaxiResource` carries no direction.
- "Waiting is allowed only at the start (stand / holding queue) and at plain nodes that are not junction boxes" (spec §1).
- "Edge traversal times come from `FSpeedProfile` (the drivability authority - bends and stops included), plus a safety margin on every window" (spec §1).
- "Edge admissibility (one-way `EGuidelineDir`, aircraft size, runway rules) is `RouteSearch`'s, reused - never a second copy" (spec §1).
- "Planning runs on clearance requests, re-times and edits only - never per frame" (spec §2): the planner is a pure call; no tick.
- `Model/` is world-free: tests `NewObject<URoadNetwork>` and no world.
- Rule 77 (model .cpp line budget): new .cpp files stay under 800 lines; `RouteSearch.cpp`'s row (1191) is lowered to its new count after the extraction (it must not sit > 50 under its figure).
- Rule 22: no `FRouteQuery X;` outside RouteSearch.cpp - build queries with `FRouteQuery::For`.
- Rule 15: `SpeedProfile.*` take `FChassis`, never `FAirframe`.
- Batched testing (owner rule): all PR 1 tests + compile-able stubs, ONE build + ONE filtered run (red, each on its own assertion), implement, ONE build + ONE filtered run (green), ONE full suite.
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-slot4\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE`
- Tests: `C:\repos\airportmgr2-slot4\Tools\Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-slot4\AirportMgr.uproject" -Filter Airside.Model.TaxiPlan`

## Review Focus

1. A goal someone else is due at LATER (finite free interval) - expected: refused, because "every plan ends where the aircraft can stay" (spec §2). Pinned in Task 4's `RefusesWhenNothingFree`.
2. A re-plan by the same holder whose own windows are still booked - expected: its own windows are ignored, same arrival. Pinned in Task 4's `PlanBooksCleanly`.
3. A plan with holds booked into the table - expected: every pass books without overlapping another (an edge held through a hold at its end must not collide with the node window). Pinned in Task 4's `PlanBooksCleanly`.
4. A start node held by someone else at `DepartAt` - expected: `NoFreeWindow`, no crash. Pinned in Task 4's `RefusesWhenNothingFree`.
5. A query with no errand - expected: `NoRoute`, RouteSearch's own error logged once (`IsQueryAnswerable` reused). Pinned in Task 4's `RespectsOneWayAndSize`.

## Facts located (verified against live headers, 2026-10-02)

- Guideline graph: `Model/RoadGuideline.h` - `FGuidelineNode` (`Incident`, `bCrossingConflict`, `HoldingPosition`), `FGuidelineEdge` (`A`, `B`, `Direction` = `EGuidelineDir`, `MaxWingspan` 0 = unlimited, `AtJunction` set on every turn-path piece, `Length` cached). Handles `FGuidelineEdgeId`/`FGuidelineNodeId` in `Model/RoadHandles.h` (Index+Generation, hashable).
- Admissibility today lives in `RouteSearch.cpp`'s anonymous `ExpandNode` (lines 190-388 at 5daf0ae7): traffic class + one-way via `URoadNetwork::ForEachOutgoingGuideline(Node, Class, Visit)`, then banned edge/node, tow exclusions, pavement (clamped to `TaxiwayPavementCeiling`), runway avoidance (`ERunwayAvoidance`, per-search runway memo), size (`ExceedsWingspan`, `VehicleFit::Fits` memo). Cost (`EdgeCost`) is separate. `IsQueryAnswerable` (Errand Unset / occupancy mismatch) is anonymous too.
- Policy: `FRouteQuery::For(ERouteErrand, Start, Goal, Wingspan, ETraversalClass)` is the only factory; `ArrivalTaxiIn` and `DepartureToEntry` resolve to `ERunwayAvoidance::All`, `EOccupancyUse::Never` (`RoutePolicy.cpp`). `.NeedsPavement(Airframe.MinimumPavement)`.
- `FSpeedProfile` (`Model/SpeedProfile.h`): `Build(Points, FChassis, ...)`, `LimitAt(Distance)`; last vertex limit forced to 0 (route ends at rest); logs one census line per Build (and a Warning per too-tight span). No time accessor exists. Ground figures: `FChassis::Ground` (`FGroundPerformance`) `.Taxi` (`FGroundRegime`: `Accel`, `Decel`, `SpeedCap`).
- Box: `TrafficClaims.cpp:1195` `const bool bBox = Length < F + G;` with F/G = `FTrafficRules::FootprintFor/GapFor(Class)` (`Model/TrafficRules.h`). "A box is an edge the agent cannot stand on without still blocking the node behind it, which is every junction turn path" (TrafficClaims.h:139-142).
- Airframe: `FAirframe` (`Model/Airframe.h`): `Wingspan`, `Chassis`, `MinimumPavement`. `VehicleCodes.h` names vehicle kinds only (not aircraft size) - not used.

## Design decisions (and the alternatives rejected)

- **Admissibility reuse = extraction, not a callback into A*.** `ExpandNode`'s filter half moves to `FRouteEdgeFilter::ForEachAdmitted`; `ExpandNode` keeps cost and relaxation and calls it. SIPP needs the SAME "may this edge be taken from here" but a different cost (time) and different state (node x interval), so neither `Find` nor `FindToGoals` can be reused whole. Memo semantics stay per search (the filter object is per search).
- **Timing = `FSpeedProfile` per PIECE (edge), four variants.** An edge's time depends on whether the aircraft enters/leaves it rolling or at rest. `BuildPiece(Points, Chassis, EPieceEnd::Rolls|Stops)` + `SecondsToDrive(EntrySpeed)` give RollRoll / RestRoll / RollRest / RestRest per edge. Rejected: one whole-route profile (SIPP does not know the route until it ends); per-edge `Build` (ends every edge at rest and logs a census line per edge per plan). Cross-edge braking is approximated (a piece enters at its own `LimitAt(0)`); `EtaAgreesWithWholeRouteProfile` measures the error against the whole-route profile.
- **Hold rule.** Hold allowed at the start, or at node N reached via edge E when: E is not a turn path (`AtJunction` unset), `!Rules.IsBox(E.Length, Aircraft)`, and N is not `bCrossingConflict`. While holding, the aircraft keeps holding E (its tail) and N.
- **Windows.** Edge: `[Leave - m, max(Reach, departure from To) + m]`. Node: `[Reach - m, departure + m]`; start node `[DepartAt, Leave + m]`; goal `[Reach - m, Forever)`. Half-open; touching windows do not conflict. One holder per resource at a time (in-trail on one edge is excluded - a known throughput cost PR 2 measures).
- **Goal must be free for ever** after arrival ("every plan ends where the aircraft can stay").
- **Margin** is `FTrafficRules::TaxiPlanMargin` (seconds, default 5) - one source, a level knob.
- **No log category in PR 1.** The planner is a pure function; the owner (`UTaxiPlanning`, PR 2) logs "once per event". `FTaxiPlan` carries the reason.

---

### Task 1: `FTaxiReservations`

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/TaxiReservations.h`
- Create: `Plugins/Airside/Source/Airside/Private/Model/TaxiReservations.cpp`
- Test: `Plugins/Airside/Source/AirsideTests/Private/TaxiPlanTest.cpp`

**Interfaces:**
- Produces:

```cpp
enum class ETaxiResourceKind : uint8 { Edge, Node };

struct AIRSIDE_API FTaxiResource
{
	ETaxiResourceKind Kind = ETaxiResourceKind::Node;
	FGuidelineEdgeId EdgeId;
	FGuidelineNodeId NodeId;
	static FTaxiResource Edge(FGuidelineEdgeId Id);
	static FTaxiResource Node(FGuidelineNodeId Id);
	bool operator==(const FTaxiResource& Other) const;
};
uint32 GetTypeHash(const FTaxiResource& Resource);

struct FTaxiWindow { int32 Holder = 0; double From = 0.0; double To = 0.0; };   // [From, To)
struct FTaxiInterval { double Start = 0.0; double End = 0.0; };                  // [Start, End)
struct FTaxiPass { FTaxiResource Resource; FTaxiWindow Window; };

class AIRSIDE_API FTaxiReservations
{
public:
	static constexpr double Forever = TNumericLimits<double>::Max();
	static constexpr double Always = TNumericLimits<double>::Lowest();
	bool BookWindow(const FTaxiResource& Resource, const FTaxiWindow& Window);   // false + unchanged on overlap or From >= To
	bool BookPasses(TConstArrayView<FTaxiPass> Passes);                          // all or nothing
	int32 ReleaseHolder(int32 Holder);
	int32 ReleaseHolderOn(const FTaxiResource& Resource, int32 Holder);
	void FreeIntervals(const FTaxiResource& Resource, int32 IgnoreHolder, TArray<FTaxiInterval>& Out) const;
	bool IsFree(const FTaxiResource& Resource, double From, double To, int32 IgnoreHolder) const;
	TConstArrayView<FTaxiWindow> WindowsOn(const FTaxiResource& Resource) const;   // sorted by From
private:
	TMap<FTaxiResource, TArray<FTaxiWindow>> Windows;
};
```

Mutator names are deliberately distinctive (`BookWindow`, `BookPasses`, `ReleaseHolder`, `ReleaseHolderOn`) so PR 2's lint rule can grep them; `Insert`/`Remove` collide with `TArray`.

- [ ] **Step 1: Write the failing tests** `Airside.Model.TaxiPlan.ReservationsBookAndRelease` and `Airside.Model.TaxiPlan.ReservationsFreeIntervals`:

```cpp
// BookAndRelease
FTaxiReservations Table;
const FTaxiResource R = FTaxiResource::Node(Node);   // a live node from a NewObject network
TestTrue(TEXT("a window books on an empty resource"), Table.BookWindow(R, {1, 10.0, 20.0}));
TestTrue(TEXT("a TOUCHING window books - [10,20) and [20,30) share no instant"), Table.BookWindow(R, {2, 20.0, 30.0}));
TestFalse(TEXT("an OVERLAPPING window is refused"), Table.BookWindow(R, {3, 15.0, 25.0}));
TestEqual(TEXT("and the refusal changed nothing"), Table.WindowsOn(R).Num(), 2);
TestFalse(TEXT("a window with no length is refused"), Table.BookWindow(R, {3, 40.0, 40.0}));
TestTrue(TEXT("out-of-order booking"), Table.BookWindow(R, {3, 0.0, 5.0}));
TestEqual(TEXT("windows read sorted by From"), Table.WindowsOn(R)[0].Holder, 3);
// BookPasses all-or-nothing: one pass overlapping holder 2 -> neither pass lands
// ReleaseHolderOn(R, 2) == 1 and the edge resource of holder 2 survives; ReleaseHolder(1) == count of 1's windows
// Edge(Id) and Node(Id) with the same Index are different resources

// FreeIntervals
Table.BookWindow(R, {1, 10.0, 20.0}); Table.BookWindow(R, {2, 30.0, 40.0});
Table.FreeIntervals(R, 0, Free);   // [Always,10) [20,30) [40,Forever)
Table.BookWindow(R, {3, 20.0, 30.0});
Table.FreeIntervals(R, 0, Free);   // touching windows leave no zero-length gap: [Always,10) [40,Forever)
Table.FreeIntervals(R, 3, Free);   // holder 3's own window ignored: [Always,10) [20,30) [40,Forever)
TestTrue(TEXT("IsFree in a gap"), Table.IsFree(R, 20.0, 30.0, 0) == false && Table.IsFree(R, 20.0, 30.0, 3));
```

- [ ] **Step 2:** stub every function (return false / 0 / empty) so it compiles; run with Task 3's and Task 4's tests (batched). Expected: FAIL on the assertions above.
- [ ] **Step 3: Implement.** `BookWindow`: refuse `From >= To`; refuse when any existing window satisfies `W.From < Other.To && Other.From < W.To` (half-open); insert at the sorted position (`Algo::LowerBound` by From). `BookPasses`: check every pass against the table AND against the passes before it (same resource), then book them all. `FreeIntervals`: walk sorted windows skipping `IgnoreHolder`; cursor from `Always`; emit `[cursor, W.From)` when `W.From > cursor`; `cursor = max(cursor, W.To)`; finally `[cursor, Forever)` unless `cursor == Forever`. `IsFree`: no non-ignored window overlaps `[From, To)`. Release: `RemoveAll` by holder, drop empty map entries.
- [ ] **Step 4:** green with the batched run.
- [ ] **Step 5: Commit** `taxiplan: FTaxiReservations - per-resource windows, free intervals`.

### Task 2: Seams for reuse (no behaviour change)

**Files:**
- Create: `Plugins/Airside/Source/Airside/Public/Model/RouteEdgeFilter.h`, `Private/Model/RouteEdgeFilter.cpp`
- Modify: `Private/Model/RouteSearch.cpp` (`ExpandNode`, `BuildPlanFromArrival`, `IsQueryAnswerable`, `TaxiwayPavementCeiling`, `ExceedsWingspan`, `GRunwaySeedResolveCountForTest` + its two accessors move), `Public/Model/RouteSearch.h` (declare `RouteSearch::IsQueryAnswerable`, `RouteSearch::PlanFromSteps`; fix the `TaxiwayPavementCeiling` file reference)
- Modify: `Public/Model/TrafficRules.h`, `Private/Model/TrafficRules.cpp` (`IsBox`, `TaxiPlanMargin`), `Private/Model/TrafficClaims.cpp:1195` (use `IsBox`)
- Modify: `Tools/Check-Architecture.ps1` rule 77 row for RouteSearch.cpp (lower to the new count, dated reason)

**Interfaces:**
- Produces:

```cpp
// RouteEdgeFilter.h
struct FAdmittedEdge
{
	FGuidelineEdgeId Id;
	const FGuidelineEdge* Edge = nullptr;
	FGuidelineNodeId Next;
	bool bReversed = false;
	bool bRunwayEdge = false;   // the one answer EdgeCost's runway penalty reads
};

class AIRSIDE_API FRouteEdgeFilter
{
public:
	FRouteEdgeFilter(const URoadNetwork& InNetwork, const FRouteQuery& InQuery, bool bInIgnoreSize = false,
		const TSet<FGuidelineEdgeId>* InExcluded = nullptr, TMap<FGuidelineEdgeId, bool>* InFitMemo = nullptr);
	void ForEachAdmitted(FGuidelineNodeId At, TFunctionRef<void(const FAdmittedEdge&)> Visit);
};

// RouteSearch.h, namespace RouteSearch
AIRSIDE_API bool IsQueryAnswerable(const FRouteQuery& Query);
AIRSIDE_API FRoutePlan PlanFromSteps(const URoadNetwork& Network, FGuidelineNodeId Start, TArray<FRouteStep> Steps);

// TrafficRules.h
UPROPERTY(EditAnywhere, meta = (ClampMin = "0")) double TaxiPlanMargin = 5.0;
bool IsBox(double StepLength, ETraversalClass Class) const;   // StepLength < FootprintFor + GapFor
```

- [ ] **Step 1:** Move the filter half of `ExpandNode` (null/self-loop edge, `BannedEdge`, `Excluded`, `BannedNode`, pavement, runway avoidance, size with fit memo) into `ForEachAdmitted`, IN THE SAME ORDER (the runway seed memo is consulted before avoidance, which is what `RunwaySeedResolveCountForTest` measures). Every WHY comment moves with its line. `ExpandNode` becomes:

```cpp
FRouteEdgeFilter& Filter, ... 
Filter.ForEachAdmitted(At, [&](const FAdmittedEdge& Admitted)
{
	const double Cost = EdgeCost(*Admitted.Edge, Admitted.Id, Query, Admitted.bRunwayEdge);
	if (Cost < 0.0) { return; }
	if (Closed.Contains(Admitted.Next)) { return; }
	... // relaxation unchanged
});
```

`RunSearch` and `FindToGoals` construct one `FRouteEdgeFilter` per search (where `RunwayInUse`/`RunwaySeeds` were declared).
- [ ] **Step 2:** split `BuildPlanFromArrival` into the backtrace and `RouteSearch::PlanFromSteps` (the weld); make `IsQueryAnswerable` a public `RouteSearch::` function.
- [ ] **Step 3:** `FTrafficRules::IsBox`; `TrafficClaims.cpp`'s `const bool bBox = Rules.IsBox(Length, Agent.Class);` (F/G there ARE `FootprintFor/GapFor(Agent.Class)`, TrafficClaims.cpp:393-394).
- [ ] **Step 4:** Count `UE_LOG(` and comment lines in RouteSearch.cpp + RouteEdgeFilter.cpp before/after (must not fall). Existing `Airside.Model.RouteSearch.*`, `RoutePolicy.*`, `Traffic.BoxEntryFirstOnly` cover the refactor in the full suite.
- [ ] **Step 5: Commit** `route: edge filter, weld and box rule shared - the planner's seams`.

### Task 3: `FSpeedProfile` pieces and time

**Files:**
- Modify: `Plugins/Airside/Source/Airside/Public/Model/SpeedProfile.h`, `Private/Model/SpeedProfile.cpp`
- Test: `TaxiPlanTest.cpp` (`Airside.Model.TaxiPlan.PieceSeconds`)

**Interfaces:**
- Produces:

```cpp
UENUM() enum class EPieceEnd : uint8 { Rolls, Stops };
void BuildPiece(const TArray<FVector2D>& Points, const FChassis& Chassis, EPieceEnd End);   // quiet: no census, no warning
double SecondsToDrive(double EntrySpeed) const;   // forward pass at Taxi.Accel under LimitAt
```

- [ ] **Step 1: Failing test** `PieceSeconds`: a straight 10000 uu line, Piper chassis. `Stops` from rest > `Rolls` from rest > `Rolls` entered at `LimitAt(0)`; `Rolls` entered at the cap equals `Length / SpeedCap` within 1%; a whole-route `Build` and `BuildPiece(Stops)` give the same `SecondsToDrive(0)` (one rule, two entry points).
- [ ] **Step 2:** red with the batch.
- [ ] **Step 3:** private `BuildLimits(Points, Chassis, SpanDirections, EPieceEnd End, bool bWholeRoute)`; `Build` = `(…, Stops, true)`; `BuildPiece` = `(…, End, false)`; the two `UE_LOG`s gated on `bWholeRoute`; `VertexLimits.Last() = 0` only for `Stops`. New `UPROPERTY() double Accel` set from `Ground.Taxi.Accel`. `SecondsToDrive`: step 100 uu; `Next = min(LimitAt(s + ds), sqrt(v^2 + 2 Accel ds))`; `dt = ds / max(mean, FRouteFollower::ProgressEpsilon)`.
- [ ] **Step 4:** green. **Step 5: Commit** `speed: pieces and seconds - the profile answers how long`.

### Task 4: `FTaxiPlanner` (SIPP)

**Files:**
- Create: `Public/Model/TaxiPlanner.h`, `Private/Model/TaxiPlanner.cpp`
- Test: `TaxiPlanTest.cpp`

**Interfaces:**
- Consumes: Tasks 1-3.
- Produces:

```cpp
enum class ETaxiPlanResult : uint8 { Planned, NoRoute, NoFreeWindow };

struct FTaxiRequest { FGuidelineNodeId Start; FGuidelineNodeId Goal; ERouteErrand Errand = ERouteErrand::Unset; int32 Holder = 0; double DepartAt = 0.0; };
struct FTaxiLeg { FGuidelineEdgeId Edge; FGuidelineNodeId To; double Leave = 0.0; double Reach = 0.0; };
struct FTaxiHold { FGuidelineNodeId At; double From = 0.0; double To = 0.0; };
struct FTaxiEdgeSeconds { double RollRoll = 0.0; double RestRoll = 0.0; double RollRest = 0.0; double RestRest = 0.0; bool bUsable = false; };

struct AIRSIDE_API FTaxiPlan
{
	ETaxiPlanResult Result = ETaxiPlanResult::NoRoute;
	ERouteResult RouteResult = ERouteResult::NoStart;   // RouteSearch::Find's answer on NoRoute
	FRoutePlan Route;          // welded by RouteSearch::PlanFromSteps - drivable
	TArray<FTaxiLeg> Legs;     // Legs[i] times Route.Steps[i]
	TArray<FTaxiHold> Holds;
	TArray<FTaxiPass> Passes;  // every window, Holder's, margin included - BookPasses-ready
	double Arrival = 0.0;      // at rest on Goal
	bool IsPlanned() const;
};

class AIRSIDE_API FTaxiPlanner
{
public:
	FTaxiPlanner(const URoadNetwork& InNetwork, const FTaxiReservations& InTable, const FAirframe& InAirframe, const FTrafficRules& InRules);
	FTaxiPlan Plan(const FTaxiRequest& Request);
	double RouteSeconds(const FRoutePlan& Route);   // rest at both ends, rolling between
	const FTaxiEdgeSeconds& SecondsFor(FGuidelineEdgeId Edge, bool bReversed);
	bool IsSharpJoint(FGuidelineEdgeId In, bool bInReversed, FGuidelineEdgeId Out, bool bOutReversed);
	static bool CanHoldAt(const URoadNetwork& Network, const FTrafficRules& Rules, FGuidelineEdgeId Arrived, FGuidelineNodeId At);
};
```

- [ ] **Step 1: Failing tests** (all on hand-built `NewObject<URoadNetwork>` graphs, `TestAirframes::Piper()`, default `FTrafficRules`):
  - `EmptyNetworkIsShortestRouteTime` - diamond (RouteSearchTest's shape): planned; same steps as `RouteSearch::Find`; `Arrival == DepartAt + RouteSeconds(Find)` (1e-6); no holds.
  - `EtaAgreesWithWholeRouteProfile` - a route with two instant corners: planner not optimistic by > 5%, not pessimistic by > 15% against a whole-route `FSpeedProfile::Build` + `SecondsToDrive(0)`; a smooth (straight) route within 1%.
  - `WaitsAtPlainNodeNotInJunction` - S -lane- P -turn path- J1 -turn path- J2 -lane- G (turn paths: `AtJunction` set, 400 uu); other holders: S from 30 s on, P->J1 [60, 200), J2->G [0, 100). Expect: planned; a hold at P; no hold at J1/J2; `Arrival > 200`. (Mutation: with holds allowed in the junction the plan holds at J2 and arrives ~110 s.) Plus `CanHoldAt` false on J1/J2 and on a short lane (`IsBox`), true at P.
  - `TakesOtherParallel` - X->Y twice (straight and curved, two edges = two resources); straight booked [0, 100): planned via the curved one, `Arrival < 100`.
  - `RefusesWhenNothingFree` - goal booked [0, Forever) -> `NoFreeWindow`, `RouteResult == Found`; goal booked [500, Forever) (someone due later) -> `NoFreeWindow`; start node held by another at `DepartAt` -> `NoFreeWindow`.
  - `RespectsOneWayAndSize` - only edge one-way against -> `NoRoute`/`Unreachable`; too narrow -> `NoRoute`/`TooWide`; short way one-way against + long way two-way -> planned the long way; `Errand::Unset` -> `NoRoute` with the expected "no errand" error.
  - `PlanBooksCleanly` - plan with a hold books via `BookPasses`; replanning the same holder with its windows booked gives the same arrival; a second holder's plan avoids the first's windows and books too.
- [ ] **Step 2:** red with the batch.
- [ ] **Step 3: Implement SIPP.** *(Revised during execution, 2026-10-02 - see "Execution notes" below.)* States exist ONLY at nodes the aircraft may stop at (start, `CanHoldAt`, goal), keyed (node, node interval, arriving edge, arriving-edge interval). A MOVE runs from a state through every no-stop node (depth-first, `FRouteEdgeFilter::ForEachAdmitted`, at most 8 edges) to the next stoppable node or the goal, with ONE departure `Tau` that fits every resource on the chain: per edge `[Tau+Enter-m, Tau+Reach+m]`, per passed node `[Tau+Reach-m, Tau+Reach+m]`, end node at least as reached (for ever at the goal); `Tau` pushed by the least shift any need asks until a pass asks none, never past `LatestLeave` (= min(node interval end, arriving-edge interval end) - m). Two variants per move: rolling on (`Tau = Arrive`, not from the start, not round a sharp joint) or from rest (`Tau >= Arrive + StopLoss`). Step durations from `SecondsFor` (four timings), at rest after/into a sharp joint (`IsSharpJoint` = `FSpeedProfile::HasSharpVertex` on the two spans either side) or into the goal. Priority `Arrive + dist/Taxi.SpeedCap`. On failure, `RouteSearch::Find` with the same query decides `NoRoute` (its result) vs `NoFreeWindow`.
- [ ] **Step 4:** green; then the full suite.
- [ ] **Step 5: Commit** `taxiplan: FTaxiPlanner - SIPP earliest arrival over the table`.

### Task 5: Lint rule decision, PR

- [ ] The rule "only `UTaxiPlanning` writes `FTaxiReservations`" LANDS IN PR 2. Why: its subject does not exist yet, and this codebase's rules fail when a file they name is missing ("do not let the rule check nothing"); in PR 1 no production code writes the table at all and the planner takes it `const&`, which the compiler enforces. PR 2 adds the row: callers of `BookWindow|BookPasses|ReleaseHolder|ReleaseHolderOn` only in `TaxiPlanning*.cpp`, `TaxiReservations.cpp` and tests.
- [ ] Full suite, push, `gh pr create --base feature/taxiway-names`.

## Execution notes (PR 1, 2026-10-02)

- **Textbook SIPP keyed on (node, interval) was wrong here, measured.** `WaitsAtPlainNodeNotInJunction` failed: the early arrival at J1 (a junction node, no waiting) was a dead end and, keyed alike, discarded the later arrival that fitted. Earliest-arrival dominance needs waiting; inside a junction there is none. Fix: states only at stoppable nodes; chains through the rest as one move with one departure (Step 3 above).
- **Instant corners between edges.** Per-piece timing first ignored a sharp vertex AT an edge boundary - 29.3 s against the whole-route profile's 39.8 s (26% optimistic). `IsSharpJoint` now asks the authority (`HasSharpVertex` on the joint's two spans) and times it as a stop: 44.2 s (+11%, pessimistic - the follower crawls at its steering floor, the planner stops). The test now bounds optimism (5%) and pessimism (15%) separately, and a smooth route to 1%.
- `FTrafficRules::IsBox` and `TaxiPlanMargin` landed in the red commit (the tests read them).
- Lint rule for table writers deferred to PR 2 (Task 5). Mutator names chosen for it: `BookWindow`, `BookPasses`, `ReleaseHolder`, `ReleaseHolderOn`.
- **Review of #527 (2026-10-02), fixed on PR 1:** one SIPP successor per reachable free interval of the move's end (was: earliest only - `LaterIntervalSuccessor`); no reversing onto the arriving edge (`NoUTurnOnArrivingEdge`, red with the check removed); one arithmetic for window bounds in search and booking, `WindowLo/WindowHi` (`TouchingBoundaryFractionalReach`: 194/200 booked before); the goal's arriving edge held for ever with the goal; `TaxiPlanMaxChainEdges` a rules knob, caps warned once per session (`LogAirsideTaxiPlan`, added here for it); chains memoised per (node, arriving edge). Noted, not fixed: dominance ignores momentum (a later rolling arrival pruned by an earlier one) - a pessimism, never a conflict; see `FSippState`.
