# Ops batch 3 - PR C plan: #405 (fallback park) and #404 (restored mid-flight)

Spec: `docs/superpowers/specs/2026-09-29-ops-batch3-design.md` §4, §7. Branch `feature/ops-restore-fixes`
(worktree `C:\repos\airportmgr2-ops-batch3`), stacked on `feature/ops-airport-status` (PR B, #416). Baseline full
suite on this tree: `1512 test(s) run, 0 failed, 0 crashed`. Baseline counts (UE_LOG / comment lines) at `e498d559`:
FlightBoard.cpp 19/205, FlightBoard.h 0/364, OpsRuntime.cpp 24/304.

## Premises checked against the code (2026-09-30)

- **Agents are not saved.** `UOpsRuntime::Persistents()` is Clock, JobBoard, FlightBoard, Ledger, Pricing,
  OfferGenerator, Airlines, Airport; the network blob is `URoadNetwork` (no agents); `LoadFromSlot` calls
  `ClearAgents` before `OpsSave::Restore`. The spec's premise holds.
- **No turnaround opens at the junction already.** `UJobBoard::OnAgentPhase` opens one only when the agent is still
  Parked AND its `GoalNode` is the pose of an entity with `PoseRole == Aircraft`. `DropAircraft` returns at once
  with no turnaround, so no `TurnaroundEnded` on the redirect. Pinned by the #405 test, not changed.
- **The landing fee flag is already saved.** `UFlight::bLandingFeePaid` is a UPROPERTY, set with the ledger post in
  `PostLandingFee`, which is a no-op when it is set. A restored Landing/TaxiIn flight carries it, so its second
  landing charges nothing. Forcing it true in `OnAfterRestore` would LOSE the fee of a flight saved before its
  `Arriving` was handled (the flag false, never charged) - so the flag is left as saved, and the test pins "charged
  once across the load".
- **`OnAfterRestore` is handed no clock.** Both `HoldingSince = now` and the retired flight's `TerminatedAt` need
  the loaded Now. `Persistents()` restores the Clock first (index 0; `OpsSave::Restore` is RestoreBlob +
  OnAfterRestore per object, in order), so a clock pointer on the board reads the LOADED time.
- **Load ordering.** `LoadFromSlot`: ClearAgents, Discard, Restore (-> `OnAfterRestore`), RebuildMesh, Reseat,
  `OnGraphRebuilt` (Reapply holds Accepted+Inbound stands), `RearmSchedules` (touches Accepted only), MarkAllDirty.
  Demoting to Inbound IN `OnAfterRestore` is therefore before the Reapply (stand re-held) and untouched by
  RearmSchedules (which would reset HoldingSince to ArrivesAt via Enqueue). `TickQueue` runs every frame and
  dispatches it.

## Files

| File | Change |
|---|---|
| `AirportOps/Private/Model/FlightBoard.cpp` | `OnAgentPhase`: a pre-Turnaround flight enters Turnaround only when its agent is still Parked at a STAND; Parked->Taxiing of a flight that never reached a stand reads TaxiOut when the goal is not a stand (a Depart from the junction). `OnAfterRestore`: re-queue / retire. |
| `AirportOps/Public/Model/FlightBoard.h` | `UPROPERTY(Transient) TObjectPtr<USimClock> RestoreClock` (named apart from the many `Clock` parameters: C4458); `OnAfterRestore` doc |
| `AirportOps/Private/Present/OpsRuntime.cpp` | `Attach`: `FlightBoard->RestoreClock = Clock` beside `Ledger->Clock`; `Persistents()` order comment: the clock must precede the board |
| tests | `OpsRuntimeBusTest.cpp` (#405), `FlightSaveTest.cpp` (#404 model), `OpsRuntimeTest.cpp` (#404 through LoadFromSlot) - existing files, one build each |

## Task 1 - #405

Test `AirportOps.Model.Bus.FallbackParkStaysTaxiIn` (StandRetarget's scenario on `FTestAirport` 2 stands, default
airframe; FlightBoard + JobBoard on a local bus fed by `OnAgentPhaseChanged`, drained after each Advance, as
production does): dispatch through the board, delete both stands mid-arrival, run until Parked at the junction:
flight `TaxiIn`, no turnaround. Place a stand, rebuild, re-offer: `Taxiing`, flight still `TaxiIn`, no
`TurnaroundEnded` published. Run until Parked at the stand: `Turnaround`, `Stand` is the new one, turnaround open.
Red expected: "parked on the fallback junction it is still taxiing in" (Turnaround).

Test `AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut`: same to the junction park, then `DepartAgent`; the
flight never reads TaxiIn after the depart and reaches Departing. Red (without the Taxiing rule) only if the depart
drives straight out; if it pushes back, Manoeuvring already maps absolutely and the test is a guard.

Fix in `OnAgentPhase` (live agent read: the event is a step late):

```cpp
const FEntityInstanceId GoalStand = StandAtGoal(Traffic, Network, AgentId); // pose of an IsStand() entity, or unset
EFlightPhase Next = FlightPhaseFromAgent(To, WasPhase);
if (WasPhase < EFlightPhase::Turnaround)
{
    if (To == EAgentPhase::Parked && !(StillParked && GoalStand.IsSet())) { Next = WasPhase; /* log */ }
    else if (To == EAgentPhase::Taxiing && From == EAgentPhase::Parked && !GoalStand.IsSet()) { Next = EFlightPhase::TaxiOut; }
}
```

Mutations: drop the Parked gate -> FallbackParkStaysTaxiIn red; drop the Taxiing rule -> DepartFromFallback red (if
straight out).

## Task 2 - #404

Model test `AirportOps.Model.FlightSave.MidFlightGoesRoundOrRetires` (FTestAirport, real dispatcher, bus-fed board
as `LandingFeeIsCharged`): land a flight with `LandingFee > 0` (charged once), run to TaxiIn; plant a Turnaround
flight with an agent id and a genuinely Inbound one (HoldingSince earlier); advance; Capture; advance the clock
again; ClearAgents; Restore into a fresh board with `RestoreClock` = the restored clock. Assert: TaxiIn ->
`Inbound`, `AgentId` none, `FlightForAgent(old)` null, `HoldingSince` == loaded Now, behind the genuine one in
`Queue()`; Turnaround -> `Departed`, in History (not `Live()`), `TerminatedAt` == loaded Now; nothing published.
Then `OnGraphRebuilt` -> the stand is held; `RearmSchedules`; `TickQueue` -> `Landing` with a new agent; drain ->
landing fee rows still 1.

Runtime test `AirportOps.Present.RuntimeLoad.MidFlightRequeuesOrRetires` (AirportTest's runtime shape: road,
runway, a stand): plant TaxiIn (stand held, fake agent) and Turnaround flights, advance, save, advance, load.
Assert Inbound with `HoldingSince == Clock->Now()` after the load (proves wiring + clock order), stand held under
the flight's holder id (proves Inbound before `OnGraphRebuilt`), Departed in history, still Inbound after ticks
(RearmSchedules left it), airline satisfaction unchanged after ticks (nothing scored).

Fix in `OnAfterRestore`, inside the existing snapshot loop, before the terminal sweep:

```cpp
const double Now = RestoreClock != nullptr ? RestoreClock->Now() : 0.0;
if (Phase == Landing || Phase == TaxiIn)  { Phase = Inbound; HoldingSince = Now; AgentId = INDEX_NONE; log "re-queued"; }
else if (Phase >= Turnaround && Phase <= Departing) { Phase = Departed; AgentId = INDEX_NONE; MoveToHistory(*Each, Now); log "retired as departed"; }
```

Mutations: drop the re-queue branch; drop the retire branch; drop `RestoreClock` wiring in Attach; move the Clock
after the FlightBoard in `Persistents()` -> each red in the named test.

## Finish

Full suite, no filter; UE_LOG / comment-line counts of touched files against the baseline; Check-Architecture
verdict and rule-12 warning count. Unverified in PIE: both fixes (spec §8 lists no PIE check for C).

## Execution notes (2026-09-30)

- Full suite: `1516 test(s) run, 0 failed, 0 crashed` (baseline 1512; +4). Check-Architecture PASS, rule-12 warnings 111 (unchanged).
- UE_LOG / comment lines, base `e498d559` -> now: FlightBoard.cpp 19/205 -> 23/235; FlightBoard.h 0/364 -> 0/379;
  OpsRuntime.cpp 24/304 -> 24/311; Flight.cpp 0/10 -> 0/13. No file fell.
- Red first: FallbackParkStaysTaxiIn ("parked on the fallback junction it is still taxiing in", "the redirect reads
  TaxiIn", ParkedAt 75 not 0); MidFlightGoesRoundOrRetires and MidFlightRequeuesOrRetires ("Inbound" not equal,
  AgentId 1 / 4101 not -1, HoldingSince 0, "Departed" not equal).
- Mutations (each red, restored with cp + touch, rebuilt green): Parked gate -> FallbackParkStaysTaxiIn; Taxiing
  rule -> DepartFromFallbackReadsTaxiOut ("the taxi away from the junction reads TaxiOut"); re-queue branch -> both
  #404 tests; retire branch -> both; `FlightBoard->RestoreClock = Clock` -> MidFlightRequeuesOrRetires (0 not
  32421); Clock after FlightBoard in `Persistents()` -> MidFlightRequeuesOrRetires (32526, the pre-load clock).
- DepartFromFallbackReadsTaxiOut stages DepartAgent's straight-out branch (RedirectAgent onto PlanAny's route):
  on FTestAirport the junction-parked heading points away, DepartAgent takes the pushback branch and is refused (no
  arm). Its first draft read only Seen (phase CHANGES) and passed under the mutation; it now reads the phase right
  after the move.
- Existing test changed by the spec: `AirportOps.Model.FlightBoard.FollowsTheAgentPhases` - its agent 5 is no agent
  of its traffic model, so Parked no longer reads Turnaround there; it asserts TaxiIn, and the at-a-stand case is
  FallbackParkStaysTaxiIn's last step.
- The `Persistents()` comment claimed OnBeforeRestore ran for every object before any blob was deserialised; OpsSave.cpp's
  loop does RestoreBlob + OnAfterRestore per object. Rewritten there. `IOpsPersistent::OnBeforeRestore`'s header
  (OpsSave.h) makes the same false claim; left, not in this PR's files.
