# Pushback and manoeuvring out of the stand

2026-09-13. Slice 1 of two: the airframe capability and the motion phase. Slice 2 — tug
vehicles, the Pushback depot, the job and the fee — plugs into the seam this one creates
and is specified separately.

## 1. Why

Not realism. Two reasons, one of which is already a defect on screen.

**The spin exists today.** A parked aeroplane's taxi-out plan starts at its stand's nose-stop
node and runs *outward* along the stand's single lead-in, so `FRouteFollower`'s first tangent
is roughly 180° from the parked heading. Before nose-gear steering that was an unremarkable
yaw; since `50a4623`/`d7355cf` it is a Meridian pirouetting on its nose wheel at the stand.
Nothing in the model expresses "back out first", so there is nowhere to fix it.

**It is the one lever that makes a Pushback depot a decision.** The GDD (§7, §9, §11) already
commits to a depot, tugs and a per-pushback fee. Without a capability split every airport
needs one from its first flight, and the building is a tax rather than a choice. With it, an
airport flying Twin Otters genuinely needs no tug, and the first A320 offer is what forces
the build. That is the same shape as stand size gating which flights can be accepted.

## 2. Scope

**In.** `EPushbackNeed` on the aircraft type; `EAgentPhase::Manoeuvring` driven by a new
`FPushbackRun`; the no-push-required case (a stand with a way out forward), including the
content half that makes such a stand authorable; push-and-start engine behaviour; pushback
clearance; the `EFlightPhase` and flight-board edits; tests and logs.

**Out.** Tug agents, the Pushback depot, the tug job and its ordering against fuelling,
the fee, towbar coupling poses, de-icing. Nothing in slice 1 branches on *which* tug: all
three `EPushbackNeed` values produce the same manoeuvre, differing only in push speed. A
value that changed behaviour before any tug existed would be a lie about who pushed.

## 3. `EPushbackNeed`

```cpp
UENUM()
enum class EPushbackNeed : uint8
{
    /** Reverses under its own power. A Twin Otter beta-ranges out of a stand. */
    SelfManoeuvre,
    /** Light enough for a towbar on a manual tug. */
    HandTug,
    /** Needs a tug vehicle. */
    VehicleTug
};
```

Authored `EditAnywhere` on `UAircraftType`, beside `TurnaroundSeconds`, and flattened into
`FAirframe` exactly as that figure is — for the reason `UFlight::Airframe` already records:
`Model/` may not see `Entities/`, and the agent carries no pointer to its type by design.

**AN ENUM AND NOT A BOOL** even though slice 1 only ever asks "is it `SelfManoeuvre`". A
two-value flag would have to be widened the moment the depot's first upgrade rung appears,
and the hand-tug tier is that rung: a cheap depot that can push a Dash 8 but not an A320.

**AUTHORED AND NOT DERIVED FROM THE CODE LETTER.** The rejected alternative was "Code A and
B self-manoeuvre, C and above need a tug", which needs no new field. It is wrong for real
types — a Dash 8 is Code C and reverses perfectly well — and it buries a gameplay lever in a
letter that exists to dimension pavement. `FAirframe::TypeCode`'s own comment records what
happened last time a code letter was asked to identify a type.

## 4. `EAgentPhase::Manoeuvring`, driven by `FPushbackRun`

A fourth sibling beside `FLandingRun`, `FRouteFollower` and `FTakeoffRun`, with
`FRoadAgent::Advance` owning the switch — the State-pattern-over-an-enum that struct's header
already describes and justifies. `Parked → Manoeuvring → Taxiing`. `UGroundTraffic::DepartAgent`
enters it instead of calling `RedirectAgent` straight from `Parked`.

### CORRECTED 2026-09-13: it walks a route of its OWN

**This section originally said the push walks a prefix of the DEPARTURE route, and that was
the defect.** It was reported from PIE with photographs (`samples/reversePath1-8.png` against
`samples/reversePathWanted1-5.png`): the aeroplane reversed its lead-in correctly and then
kept reversing the way it meant to taxi, finishing *past* the junction on the wrong side of
its own turn.

A real pushback reverses onto the arm of the junction the departure does **not** take, so
that driving forward afterwards carries the aeroplane *through* the junction and away.
`PushbackPlanner::Plan` builds that route, and plans the taxi out **from where the push ends**
— the aeroplane finishes somewhere the departure route never visits, so the route planned from
the stand no longer begins where it is standing. `FRoadAgent::TaxiOutPlan` carries it, exactly
as `TaxiInPlan` carries an arrival's.

**The heading law collapses to one line as a result**: facing is the tangent turned through
180°, the whole way. At the stand the lead-in runs away from the terminal, so tangent + π *is*
the parked heading; at the far end the route runs away from where the aeroplane is going, so
tangent + π *is* the taxi-out heading. The `Back`/`Swing` phases, `TargetHeading`,
`PushSwingLength` and the corner-finding below were all needed only because the run was
walking the wrong line. They are gone.

**When a stand has no second arm the departure is REFUSED** (`EDepartureRefusal::NoPushbackRoute`)
rather than falling back on the old behaviour — a stand nothing can leave is a layout problem
the player can see and fix, and a fallback would make the defect appear on some layouts and
not others.

The rest of this section is kept as written, because what it says about `Travelled` and the
reversed body is still exactly true — only *which line* was wrong.

### Superseded: it walks the departure plan; it does not invent geometry

`DeparturePlanner::PlanAny` already returns an `FRoutePlan` whose `Steps[0]` *is* the stand
lead-in — `FAnchorLink::Gather` casts that lead-in along `Heading + PI` from the pose node
(`AnchorLink.cpp:213-222`) and adds it only when the node has no incident edge, so there is
exactly one and it is always the route's first step. Its `EndDistance` is already computed.
So:

- **`Travelled` keeps its meaning**: the distance of the **steered axle** along
  `Plan.Polyline`, sampled by `GuidelineGeom::PointAtDistance`, exactly as
  `FRouteFollower::Advance` uses it. The tug is coupled at the nose gear and the tug driver
  follows the painted line, so the nose gear stays constrained to the guideline through the
  whole manoeuvre. Nothing about the sampling changes — which is what keeps the guideline
  invariant ("the graph samples ONCE") intact.
- **the body is reversed, and that is a fact about the OFFSETS, not the line.** A parked
  aeroplane's nose gear is at `s = 0` and its tail at `s > 0` — the lead-in runs away from
  the terminal the aeroplane faces. So as `Travelled` increases the **mains lead and the nose
  gear trails**, which is what a towbar push is. This is the free function
  `2026-09-13-nose-gear-steering` §6 anticipated by name.

### Two sub-phases, because the swing has a different law from the push

```cpp
UENUM()
enum class EPushPhase : uint8 { Back, Swing };
```

An enum and not a bool pair, per the house rule — and there really are two laws:

| Phase | `Travelled` runs | Heading |
|---|---|---|
| `Back` | `0` → `Steps[0].EndDistance` | constant at the parked heading |
| `Swing` | → `PushDistance` | lerped, **linearly in `Travelled`**, to `TargetHeading` |

with

```
PushDistance  = Plan.Steps[0].EndDistance + Rules.PushSwingLength
TargetHeading = tangent of Plan.Polyline at PushDistance
```

`Steps[0]` *is* the straight lead-in — `FAnchorLink::Gather` casts it as a straight ray and
builds the corner's entry sweeps as separate steps — so `Back` needs no steering law at all.

**`PushDistance` RUNS PAST THE CORNER, and an earlier draft of this spec had it stop at
`Steps[0].EndDistance`. That was wrong and would have left most of the defect in place.** The
plan's tangent at the end of the lead-in is still the *lead-in* direction; handing over there
with the body aligned to it leaves an aeroplane facing straight out of its stand, 90° off the
taxiway on an ordinary perpendicular layout, and `FRouteFollower` then slews that 90° away on
the spot — the same pirouette in §1, merely smaller. The aeroplane has to be dragged round the
corner and onto the taxiway, which is what a real turning pushback does.

Because `PushDistance` is closed-form and known **before the push starts**, §8's clearance can
reserve exactly the ground the push will use. A convergence rule ("swing until the heading
error closes") was rejected for precisely that: its length is not known until it is over.

Sanity checks the tests pin: a stand perpendicular to its taxiway swings 90°; a dead-end apron
whose taxi-out runs back the way the lead-in points swings 180°, and the tug turns the
aeroplane right round, which is correct.

- **the handover**: `Follower.Start(Plan, Airframe, Speed, Heading, Travelled = PushDistance)`,
  the same mid-plan splice the `Vacated` handover already uses. `TargetHeading` is by
  construction the plan tangent there, so the follower inherits **zero heading error**. **Not**
  `StartTaxi`, which writes `EngineRPM = 0.0` from cold and would undo §6 below.

**REUSING THE PLAN IS THE LOAD-BEARING CHOICE.** `FPushbackRun` keeps its own copy of the
`FRoutePlan` and its own `Travelled`, exactly as `FRouteFollower` does. Because there is
still one plan and one distance along it, the claim pass arbitrates a manoeuvring agent with
the machinery it already has (§8). An `FPushbackRun` that computed its own back-out line
would be a second evaluator of where the agent is — the thing the guideline invariant exists
to forbid.

### Naming

The **phase** is `Manoeuvring`, at both the agent and the flight level. The **service** is
*Pushback* — the depot, the tug, the job, the fee. A self-manoeuvring Twin Otter is not being
pushed, so calling the stage "pushback" would be false for exactly the case this slice
exists to express. GDD §7's lifecycle list is amended in the same commit to match.

## 5. When no push is needed

A stand with a taxiway on both sides is entered nose-first and left nose-first. The rule is a
**measurement, not a flag on the agent**:

> At `DepartAgent`, compare the departure plan's initial tangent at the pose node against the
> aircraft's parked heading. Within `FTrafficRules::StraightOutDegrees` (default 45°), the
> aeroplane can drive straight out: skip `Manoeuvring` entirely and dispatch the taxi as
> today.

This is deliberately not `EPushbackNeed`'s business and not a stand property the traffic
model reads. It is a fact about the geometry in front of the aeroplane, and asking the plan
answers it for every case at once — a taxi-through stand, a stand whose player-drawn taxiway
happens to run past it, and the case where a graph rebuild changed the answer since the
aircraft parked.

**The content half.** Today a taxi-through stand cannot be authored: `FAnchorLink::Gather`
adds the pose lead-in only when `Pose->Incident.Num() == 0`, and the anchor loop refuses a
second line into one nose-stop for the same reason. So `UStandDefinition` gains
`bTaxiThrough`, and `Gather` casts a **second ray along `+Heading`** when it is set — forward,
out the far side — in addition to the `Heading + PI` one. The existing single-lead-in rule is
unchanged for every stand that does not declare it; the comment explaining why one line is
normally right stays, with the exception named beside it.

Without that content change the rule in this section is correct but dormant, so both halves
ship together or neither does.

## 6. Push and start

Real practice: the crew spools the engines *while* the tug pushes, and the spool routinely
outlasts the manoeuvre. The model gets this nearly free, because `FRoadAgent::AdvanceEngine`
already runs first and unconditionally every frame, whatever phase is driving
(`RoadAgent.cpp:199-201`), interpolating `EngineRPM` toward the target over
`FEnginePerformance::SpoolUpSeconds`.

- `bEngineRunning = true` and `EngineRPM = 0.0` move from `StartTaxi` to the entry to
  `Manoeuvring`. The cold-start comment travels with them.
- The `Manoeuvring → Taxiing` handover calls `Follower.Start` directly and leaves `EngineRPM`
  where the spool has got to. A departing aeroplane therefore begins its taxi with a
  part-spooled propeller, which is the behaviour asked for.
- `StartTaxi`'s cold start stays for every other caller — a plain dispatch, a vehicle — so
  nothing that does not push regresses.

**`SelfManoeuvre` does not move until it has thrust.** A powerback is the engine doing the
work, so the push holds at zero speed until `EngineRPM` reaches the taxi-idle fraction, then
accelerates. A tug-pushed aeroplane moves from the first frame: the tug supplies the force
and the engines are incidental. This is one condition and it is the only place in slice 1
where `EPushbackNeed` changes what happens — justified because it is about the *aeroplane's*
physics, not about a tug that does not exist yet.

## 7. Figures, on `FTrafficRules`

Named flat fields plus one accessor, the shape `FootprintFor`/`GapFor` already use:

```cpp
/** How fast the push runs, uu/s. 1 uu is 1 cm - see UAircraftType::MainWheelRadius. */
UPROPERTY(EditAnywhere) double SelfManoeuvrePushSpeed = 200.0;  // 2.0 m/s, on the engine
UPROPERTY(EditAnywhere) double HandTugPushSpeed       = 80.0;   // 0.8 m/s, walking pace
UPROPERTY(EditAnywhere) double VehicleTugPushSpeed    = 150.0;  // 1.5 m/s

/** Into and out of the push, uu/s^2. Gentle: a towbar does not snatch. */
UPROPERTY(EditAnywhere) double PushAccel              = 30.0;   // 0.3 m/s^2

/** Room past the lead-in's corner the tug needs to straighten the aeroplane, uu. Sets
 *  PushDistance, and so exactly how much taxiway the clearance in section 8 reserves. */
UPROPERTY(EditAnywhere) double PushSwingLength        = 3000.0; // 30 m

/** Within this of the parked heading, the way out is forward and no push is needed. */
UPROPERTY(EditAnywhere) double StraightOutDegrees     = 45.0;

double PushSpeedFor(EPushbackNeed Need) const;
```

The three speeds are *figures*, not measurements: nothing about a real tug is being modelled
yet, and they exist so the manoeuvre reads at the right pace on screen. Slice 2 replaces the
two tug figures with the depot's own, which is why they are named for the tug rather than
for the aeroplane.

**ON THE RULES AND NOT THE AIRFRAME**, because push speed is a property of what is doing the
pushing. A hand tug is slower than a vehicle tug whatever it is pushing. `PushSpeedFor` is
the single consumer that must agree with `EPushbackNeed`, per "lists that must agree are ONE
list": adding a value to the enum without a case here is a compiler warning, not a silent
zero. Slice 2 moves these figures onto the depot's tug types and `PushSpeedFor` becomes the
fallback for `SelfManoeuvre` alone.

## 8. Claims and arbitration

**The claim WINDOW is direction-agnostic along the plan, so there is no new claim rule.**
`FClaimPass::WindowFor` takes `T = CentreOf(Agent)` and walks `Head = T + W`,
`Tail = T − F/2` in *plan* distance (`TrafficClaims.cpp:169-200`). A pushing aeroplane's
centre still advances from 0 along the same plan, and `Head`/`Tail` are symmetric about it.
The body is claimed, the lead-in held, the corner reserved ahead, all by code that exists.

**`CentreOf` IS NOT DIRECTION-AGNOSTIC, and an earlier draft of this spec said the whole
thing was.**

```cpp
return Agent.Follower.Travelled - Agent.Airframe.SteerAxleX + Agent.Airframe.BodyCentreX;
```

`BodyCentreX − SteerAxleX` is negative on a conforming airframe — the body's plan centre sits
**aft** of the nose gear — so this places the centre *behind* the steered axle in plan
distance. That is right for a taxi, where the nose gear leads. During a push the mains lead
and the body is *ahead* of the nose gear in plan distance, so the offset must be negated:

```cpp
const double Ahead = Agent.Airframe.BodyCentreX - Agent.Airframe.SteerAxleX;
return Agent.DistanceAlongPlan() + (Agent.Phase == EAgentPhase::Manoeuvring ? -Ahead : Ahead);
```

Left unfixed this misplaces the claimed body by `2 × |BodyCentreX|` — 12.6 m on plane2, whose
re-export about its nose gear is the reason `CentreOf` exists at all. It would show as an
aeroplane still holding its stand after it had left it, and as one pushing into ground it had
not claimed. Its own comment already warns that this figure moved silently once.

Three further changes follow:

1. **`FClaimPass::Run:925`** branches `Phase != Taxiing → HoldRunwayOnly`, which releases
   every guideline claim. `Manoeuvring` must take the Taxiing path or a pushing aeroplane
   drops the lead-in it is standing on and something can be cleared onto it.
2. **`Follower.{Plan, Travelled, Speed}` now has two possible owners.** Three accessors on
   `FRoadAgent` — `PlanInProgress()`, `DistanceAlongPlan()`, `SpeedAlongPlan()` — switch on
   phase, and the readers that must see a push go through them. Counted outside
   `RoadAgent.cpp`: 9 in `TrafficClaims.cpp`, 10 in `GroundTrafficRebuild.cpp`, 3 in
   `GroundTraffic.cpp`. The rebuild's are not optional: a graph rebuilt mid-push must
   re-resolve the push's plan, or it dies exactly as a taxi's would
   (`GroundTrafficRebuild.cpp:189` branches on `Phase == Taxiing`).

`GroundTrafficDeadlock.cpp`'s 4 reads stay `Taxiing`-only, because of:

### Pushback clearance

**`DepartAgent` refuses to enter `Manoeuvring` unless the whole push is free** — the lead-in
and the corner, checked once against the occupancy table before the phase starts. A new
`EDepartureRefusal::PushbackBlocked` slots into `UFuelService::DepartTheReady`'s existing
"logged on a change of reason, not every tick" path.

**WHY CLEARANCE RATHER THAN ARBITRATION.** A manoeuvring agent cannot replan: it must finish
the push it started, and there is no alternative back-out line. So if the deadlock resolver
ever picked one it would have no move to make, and the alternative was to special-case the
resolver to skip it — leaving a phase that can block a taxiway indefinitely with nothing able
to act on it. Granting the push up front makes it atomic and removes it as a deadlock source
entirely. It is also what ground control actually does: pushback clearance is granted or
withheld, never half-granted.

## 9. The flight board

- **`EFlightPhase` gains `Manoeuvring`, between `Turnaround` and `TaxiOut`.** `Flight.h:19`
  pins the declaration order as load-bearing: `FlightPhaseFromAgent` tests
  `Current >= EFlightPhase::Turnaround` to decide taxi-in from taxi-out, and inserting after
  `Turnaround` keeps that true. The comment that says Pushback is "deliberately absent …
  a phase nothing can enter is a lie" is rewritten, not deleted — the remaining absentees
  (`Diverted`, `Cancelled`) are still absent for the reason it gives.
- **`FlightPhaseFromAgent` gains the `EAgentPhase::Manoeuvring` case.** Without it the
  `default:` leaves the flight reading "Turnaround" while the aeroplane is visibly moving.
- **`UFuelService::OnAgentPhase` needs no change**: it keys on
  `From == Parked && To != Parked` (`FuelService.cpp:298`), which `Parked → Manoeuvring`
  satisfies, so the demand is dropped and any truck recalled exactly as before.
- **`InspectFacts` needs no change**: `bCanDepart` and `bOccupantParked` both test
  `Phase == Parked`, so a manoeuvring aircraft greys the Depart button and does not read as a
  free stand. Both are correct by construction rather than by accident, and a test pins them.

## 10. Logging

`LogAirsideTraffic` (model) and `LogAirportOps` (the flight's view of it):

| Line | When |
|---|---|
| `Agent %d pushing back from stand %d: %.0f uu, %s` (the need) | entering `Manoeuvring` |
| `Agent %d push complete; taxiing out.` | the handover |
| `Agent %d departs straight out of stand %d (%.0f deg)` | §5's skip, with the measured angle |
| `Fuel: aircraft %d is ready to leave stand %d but cannot: PushbackBlocked` | existing path |

The third matters most: when a player asks why an aeroplane did not push back, the angle
that was measured is the answer, and guessing at it from the geometry costs a PIE session.

## 11. Tests

World-free in `Airside.Model.*`, matching `LandingRun` / `TakeoffRun` / `TurnRate`. Leaf
names are distinct, per the automation tree's bare-parent trap.

| Test | Asserts |
|---|---|
| `Pushback.WalksTheLeadIn` | centre travels 0 → `Steps[0].EndDistance` and stops there |
| `Pushback.BodyIsReversed` | through the straight, heading ≈ parked heading, i.e. 180° off the direction of travel |
| `Pushback.SwingEndsOnTheTangent` | handover heading equals the plan tangent at `PushDistance` — no spin left for the follower. This is the defect in §1, measured |
| `Pushback.PerpendicularStandSwings90` | a stand square to its taxiway turns 90°, not 180°: the check that `PushDistance` runs past the corner |
| `Traffic.PushbackCentreLeadsTheNoseGear` | `CentreOf` on a manoeuvring agent is AHEAD of `Travelled`; fails on the un-negated offset, by 12.6 m on plane2 |
| `Pushback.HandoverIsContinuous` | no frame with zero motion across `Manoeuvring → Taxiing`, the same property the `Vacated` handover test pins |
| `Pushback.SpoolsWhilePushing` | `EngineRPM` rises during the push and is **not** reset at the handover |
| `Pushback.PowerbackWaitsForThrust` | a `SelfManoeuvre` airframe does not move until idle RPM; a `VehicleTug` one moves on frame 1 |
| `Pushback.StraightOutSkipsThePush` | a plan whose initial tangent is within `StraightOutDegrees` goes `Parked → Taxiing` directly |
| `Traffic.PushbackHoldsTheLeadIn` | a manoeuvring agent still occupies its lead-in — fails if `HoldRunwayOnly` is taken |
| `Traffic.PushbackRefusedWhenBlocked` | `DepartAgent` returns `PushbackBlocked` with an agent on the corner, and the aircraft stays `Parked` |
| `Rebuild.PushbackSurvivesRebuild` | the graph rebuilt mid-push re-resolves rather than stranding |
| `Build.TaxiThroughStandHasTwoLeadIns` | `bTaxiThrough` produces two incident edges at the pose node; a plain stand still produces one |
| `AirportOps.Model.ManoeuvringPhase` | `FlightPhaseFromAgent` maps it, and `Turnaround < Manoeuvring < TaxiOut` still routes taxi-in vs taxi-out |

Plus one at the level of the composition, per the refactor contract's "every seam you
introduce gets a test that fails if it is unwired": spawn `ARoadNetworkActor`, tick a parked
aircraft through `DepartAgent` to `Taxiing`, assert it passed through `Manoeuvring` and that
its view was posed every frame of it.

## 12. The seam slice 2 plugs into

`FPushbackRun` is the seam. Slice 2 adds a tug agent dispatched to the stand's existing
`TugStand` anchor, a coupled-pose rule that draws the tug on the towbar, `EPushbackNeed`
finally branching (no tug available → `PushbackBlocked`, which already exists by then), the
depot, and the fee. **The motion specified here does not change.** Whether that turns out to
be true is the test of whether this slice drew its boundary in the right place.
