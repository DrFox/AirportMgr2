# Landing gear retraction and bay doors

2026-09-19. Fills in "Stage 2", named but never specified at `FAgentMotion::bAirborne`
("Off the wheels. Stage 2's gear retraction hangs on this") and at
`UAirsideAgentAnim::WheelSpinDownSeconds` ("the gear stays out - Stage 2's retraction is
separate work").

Prompted by plane4, the Boeing 737-800W, whose rig is the first in the fleet with the bones
to show any of it: `gear_nose`, `gear_L`, `gear_R` retract, `door_nose_L`, `door_nose_R`
hinge. plane2 and plane3 have no such bones - both rigs are
`root, nosewheel_steer, nosewheel, prop_L, prop_R, wheel_L, wheel_R` and nothing more.

## 1. What is being modelled

An aircraft raises its gear on climb-out and lowers it on approach. On a 737 the nose bay
doors are **linked to the strut**: they hang open whenever the gear is down and shut only
over the stowed wheel. So a cycle is TWO stages, not three, and which stage comes first
depends on the direction - see section 2, which this paragraph originally got wrong. The
mains have no doors at all - they sit in a wheel well behind a fixed fairing - which is why
plane4's rig has nose doors only and not a `door_main_*` pair.

**RETRACTION IS NOT TRIGGERED BY LIFT-OFF.** It is a pilot command, given a few hundred
feet above the ground, not at the moment the wheels leave the tarmac. `bAirborne` is the
PRECONDITION - gear cannot retract while it is carrying the aeroplane - and height is the
CUE. The distinction is the whole reason section 3 carries a height field rather than
hanging the cycle off the flag the way `bAirborne`'s own comment predicted it would.

`AgentMotionTest` already guards half of this and should be read before touching it: case 4
asserts that `ETakeoffPhase::Rotate` is not airborne, "during the rotation the aircraft is
pitching with its mains still down, and gear retraction must not begin there". That test was
written for this feature before this feature existed. It stays, and grows a sibling.

### What the flight envelope allows

The numbers already in `Model/` decide how much of this is ever on screen:

| Figure | Value | In feet | Consequence |
|---|---|---|---|
| `FClimbPerformance::ClearAltitude` | 30000 uu | ~1000 ft | The departure vanishes here |
| `FApproachPerformance::FinalAltitude` | 2000 uu | ~66 ft | An arrival *spawns* here |
| `FApproachPerformance::FlareHeight` | 900 uu | ~30 ft | The flare begins here |

So retraction at a few hundred feet is comfortably inside a departure, and **extension can
never be seen**: an arrival appears on final at 66 ft, far below any height at which a real
aircraft is still configuring. Arrivals are therefore born down-and-locked.

**THE EXTEND HALF IS BUILT ANYWAY**, and that is a decision rather than an oversight. The
cycle in section 2 is one timeline with a direction, so extension costs a sign and not a
second state machine; and the day `FinalAltitude` rises - which is the only change needed to
put an approach on a longer final - the animation is already there rather than being
discovered missing. Building only the half that shows would make the state machine
asymmetric and leave the other half to be retrofitted by somebody who no longer has this
document.

## 2. One timer, four phases, one evaluator

    EGearPhase { Down, Raising, Up, Lowering }

plus an elapsed-seconds double. Both fractions are pure functions of that one timer.

**Amended 2026-09-19, from play.** This section originally specified a trapezoid - doors
open, gear travels, doors close - and it was wrong about the aeroplane. A 737's nose gear
doors are **linked to the strut**: they hang OPEN with the gear down and shut only once it is
stowed. The two rest states therefore DIFFER, and the door movement sits at the **gear-up end
of the cycle in both directions**:

    retracting   0s                     TravelSeconds      +DoorSeconds
                 |                            |                  |
      gear       down --[ RETRACTING ]------- up --------------- up
      doors      open ------------------- open --[ SHUTTING ]--- shut

    extending    0s       DoorSeconds                      +TravelSeconds
                 |             |                                 |
      gear       up ---------- up --------[ EXTENDING ]-------- down
      doors      shut --[ OPENING ]--- open ------------------- open

A trapezoid cannot express this, because it returns the doors to the same value at both ends,
so one of the two rest states is always wrong - and no sign convention fixes that. It was
reported from play twice before the shape itself was questioned: first as a parked aeroplane
with its bay hanging open, then, after the sign was flipped, as one with the bay shut around
its own extended gear. **The bug was in the curve, not in its direction.**

On extension the doors LEAD, because they are shut over the stowed wheel and it cannot come
down through them. On retraction they TRAIL, because they are already open. There is no door
stage at the gear-down end at all, so a cycle is `TravelSeconds + DoorSeconds` - **8 s for the
737, not 9**.

**The sequencing still falls out of the shape, not out of extra states** - there is no
`DoorsOpening` phase distinct from a `DoorsClosing` one, because the timer and the direction
already say which it is. Eight states were considered and rejected on those grounds.

`Lowering` is the exact **time-reverse** of `Raising` - `FractionsAt(t, false)` equals
`FractionsAt(CycleSeconds() - t, true)` in both outputs - which is what makes the extend half
nearly free, and what `Airside.Model.GearExtendMirrorsRetract` now pins. (It formerly
asserted a *complement*, which was a property of the trapezoid rather than of the aeroplane.)

**The parked pose is the bind pose.** Gear down and bay open is what `SK_Plane4` was modelled
in, so a stationary 737 asks the animgraph to rotate no bone at all. That is the most useful
single fact for anyone wiring or debugging this.

**A PHASE IS AN ENUM, NEVER A SET OF BOOLS** - CLAUDE.md's rule, and it applies with force
here: `bGearUp` plus `bDoorsOpen` plus `bInTransit` can express "up, in transit, doors shut",
which is the state where an aeroplane's wheels are passing through its own bay doors.

**ONE FUNCTION PRODUCES BOTH NUMBERS**, in `Model/`, and the model stores what it returns.
The view never re-derives them. This is the guideline graph's invariant applied to a second
place - a second evaluator lets the doors the player sees disagree with the doors the model
thinks it opened, visibly and only mid-cycle.

## 3. `FGearPerformance`, a new member of the `FAirframe` bundle

Beside `FGroundPerformance`, `FClimbPerformance`, `FApproachPerformance` and
`FEnginePerformance`, and shaped like the last of them - including its `IsSet()`.

| Field | plane4 | Why this figure |
|---|---|---|
| `TravelSeconds` | 7.0 | The real 737-800 transit time. **0 means fixed gear** |
| `DoorSeconds` | 1.0 | 0 means no bay doors; the gear still travels |
| `RetractAboveHeight` | 9000 uu (~295 ft) | The pilot command. Not lift-off - see section 1 |
| `ExtendBelowHeight` | 15000 uu (~500 ft) | Above `FinalAltitude`, so arrivals are born down |

All `EditAnywhere`, so retuning any of them is a data edit and not a build.

**THE TWO HEIGHTS ARE READ BY PHASE, NOT BY ALTITUDE ALONE**, and getting this wrong is a
defect the numbers alone invite: 15000 uu is ABOVE 9000 uu, so an arrival descending through
9000 uu is "airborne and above `RetractAboveHeight`" and would raise its gear on short final.
`RetractAboveHeight` is consulted only while `EAgentPhase::Departing`, `ExtendBelowHeight`
only while `EAgentPhase::Arriving`. The model already knows which it is; a height comparison
that has to infer direction of flight would be re-deriving something that is already stored.

**A CYCLE NEED NOT COMPLETE.** A departure reaching `ClearAltitude` mid-transit simply goes,
gear half-stowed, because the agent is removed and there is nothing left to draw. Nothing
waits for the doors.

**ZERO MEANS FIXED, NOT UNMEASURED**, and the difference from `FAirframe::MainGearTrack`
(where zero means "nobody has measured it") is deliberate: there is no third state to
confuse it with. An aeroplane either has retractable gear or it does not, and a Twin Otter
does not. plane2 is correct by construction and permanently.

## 4. What crosses into the view

`FAgentMotion` gains two doubles, copied from the model the way `EngineRPM` is:

- `GearDownFraction`, 1.0 = down and locked
- `BayDoorOpenFraction`, 0.0 = closed

`UAirsideAgentAnim` copies both, and publishes two derived angles beside them:

- `GearAngleDegrees` = `(1 - GearDownFraction) * GearRetractedAngleDegrees`
- `BayDoorAngleDegrees` = `BayDoorOpenFraction * BayDoorOpenAngleDegrees`

against two `EditDefaultsOnly` rig facts, **measured in plane4's `build_export.py` and not
invented here**: gear travel `+90` degrees, door travel `+81` degrees ("found by sweeping:
the two free edges meet on the centreline to 0.0 mm").

Both the fractions and the angles are `BlueprintReadOnly`. The angles are what the graph
hooks to a bone, one pin each, exactly as `PropAngleDegrees` and `SteerAngleDegrees` are; the
fractions stay visible for blending and for debugging, the same reason `WheelRateDegPerSec`
is exposed when nothing else reads it.

**THE ARITHMETIC IS IN C++ AND THE MEASURED ANGLE IS ON THE ANIM INSTANCE** - the split
`MainWheelRadius` already establishes. A rig angle typed into a Blueprint is a number the
next person has no way of knowing was measured.

### Where the model owns it, and why not the view

`FEnginePerformance` decides this. The propeller used to be `bEngineRunning ? PropellerRPM
: 0` computed in the anim instance, "which made the propeller a switch: full speed the
instant an aircraft was dispatched, stopped the instant it shut down", and it was moved into
the model so the model could say where the propeller had *got to*. A gear cycle run from the
anim instance is that same mistake with more states and a worse failure - a switch between
two poses instead of a travel.

`Model/` is also where it can be tested with no world, no skeleton and no actor, which
section 6 depends on.

## 5. A list that must agree

`Tools/Python/build_plane2_anim.py` maps each bone to an animation variable **by substring**
on the lowercased bone name: `prop` -> `PropAngleDegrees`, `steer` -> `SteerAngleDegrees`,
`wheel` -> `WheelAngleDegrees`, anything else reported UNRECOGNISED and driven by nothing.
plane4's `build_export.py` already predicts the consequence: "build_plane2_anim.py's plan
will list gear_nose, gear_L, gear_R, door_nose_L and door_nose_R as UNRECOGNISED. That is the
honest report, not a fault."

It stops being honest the moment this feature lands. Two rules are added - `gear` ->
`GearAngleDegrees`, `door` -> `BayDoorAngleDegrees` - placed AFTER the existing `wheel` rule
and before the UNRECOGNISED fallback.

**ORDER IS PART OF THE MATCHER, NOT AN ACCIDENT OF IT.** The `steer` rule already sits above
`wheel` and carries its own comment saying why: `nosewheel_steer` matches both, and the wheel
rule "would tell someone to wire a STEERING bone to the ROLL angle - which spins the nose
gear about the strut and looks like a broken castor". No bone in plane4's rig matches two of
the five rules today, so the new pair could sit anywhere - they go last because the
discipline is most-specific-first and the next rig is the one that will collide.

The export script's own warning extends with them: **NO NEW BONE MAY CONTAIN `wheel`,
`steer`, `prop`, and now `gear` or `door`** unless it wants that variable. `build_export.py`
already named `gear_nose` rather than `nosewheel_retract` for exactly this reason, which is
the precedent for adding to the list rather than loosening the matcher.

This is CLAUDE.md's "check where a list is CONSUMED" in its usual form: the bone plan is one
list, the anim instance's properties are another, and they have to agree by NAME.

## 6. Which airframes get data

**plane4 only.**

- **plane2** (Twin Otter) - fixed gear in reality. Unset, forever, and correct.
- **plane3** (Dash 8-Q400) - retractable in reality, but the rig has no bones to show it.
  Left unset rather than authored, because data nothing can consume reads as working. It
  gets `FGearPerformance` the same day its rig gets `gear_*` bones, and not before.
- **PiperMeridian** - retractable in reality; same position as plane3, and no glTF export in
  the models repo to add bones to.

"A log line is not evidence the thing it describes exists" generalises to authored data.

## 7. Tests

World-free, in `Model/`, which is where CLAUDE.md sends anything that can be tested without a
repro.

1. **On EXTENSION the gear does not move until the doors are open.** At `DoorSeconds` minus
   one frame, `GearDownFraction` is still 0.0 and `BayDoorOpenFraction` is short of 1.0.
2. **On RETRACTION the doors do not shut until the gear is stowed.** At `TravelSeconds`,
   `GearDownFraction` is 0.0 and `BayDoorOpenFraction` is still 1.0.
2b. **And a parked aeroplane has BOTH at 1.0** - gear down, bay open, the bind pose. This is
   the assertion that would have caught the trapezoid on the first run.
3. **Retraction fires at the height, not at lift-off.** Airborne and below
   `RetractAboveHeight`, the gear is down; carried past it, the cycle starts. The sibling of
   `AgentMotionTest` case 4, which guards the rotation.
4. **An unset `FGearPerformance` leaves the gear down for a whole flight.** Flown from
   line-up to `ClearAltitude`, `GearDownFraction` never leaves 1.0.
5. **Extension is the retraction curve reversed.** The same two assertions as 1 and 2 with
   the signs swapped, since that symmetry is the argument section 1 used for building the
   half nobody can currently see.

**EVERY ONE ASSERTS MOVEMENT AS WELL AS STILLNESS.** A test that only checks the gear is
down at a moment passes perfectly on a gear that never moves at all, which is the failure
mode `a-green-test-may-measure-nothing` records. Each case pins a value that must change and
a value that must not.

## 8. Build

New `USTRUCT`, new `UENUM`, new `UPROPERTY`s. Live Coding covers none of those, so this is a
full `Build.bat` with the editor closed, batched into one round rather than several.

## 9. Out of scope

- **Gear affecting the simulation.** Real gear is drag; nothing here reads that. The climb
  and approach figures are unchanged.
- **A gear-down speed limit or any refusal built on it.** The runway-length refusal stays the
  only thing that can turn a movement away.
- **Retrofitting plane2 or plane3's rigs.** Section 6.
- **Raising `FinalAltitude`** so extension becomes visible. Considered in section 1 and
  deliberately not taken: it lengthens every arrival and touches approach spacing and the
  refusal maths, which is a separate change with its own reasons.
