# Nose-gear steering: what tracks the line, and what limits a turn

2026-09-13. Supersedes the in-chat design for "expose a nosewheel angle to ABP_Plane2",
which derived the steer angle from the yaw rate. It is the other way round: the steer angle
is the input and the yaw rate is what falls out of it. Two models of one thing would drift.

## The report, and what was measured against it

Reported from play: plane2 "seems to be trying to keep the centre of its rear wheels on the
route line rather than the nose wheel", and turns look very slow.

Both halves were measured before designing, and they resolve differently.

**The tracked point.** `ARoadAgentActor::SetPose` places the actor at `Motion.Position` with
no along-axis offset (`RoadAgentActor.cpp:78-85`), so what is pinned to the line is the MESH
ORIGIN. Off `plane2.glb`: main wheels at X = +190 uu, nose wheel at X = +644 uu, nose tip at
+811.6. The tracked point is therefore 1.9 m BEHIND the main gear and 6.4 m behind the nose
wheel - the report is right, and understates it. Wheelbase measures 4.54 m against the real
Twin Otter's ~4.4.

**The slow turns.** Not caused by the tracked point. The corner cap is
`v <= MaxTurnRate * Length / Turn` (`SpeedProfile.cpp:66`), which is `v = wR` - a function of
path curvature and a flat per-type yaw rate only. `DA_Aircraft_Plane2` authors
`MaxTurnRateDegPerSec = 10.0`, so a 30 m bend caps at ~10 kn and a 15 m one at ~5 kn. The flat
figure is the cause, and moving the reference point would not have changed it.

The two are worth fixing together anyway, because the same geometry answers both.

## The reference point was already decided; plane2 breaks it

`UAircraftType` states the convention (`AircraftType.h:21`):

> *LOCAL SPACE has its origin at the NOSE GEAR, +X forward, +Y starboard. The nose gear is
> what stops on the mark painted on the stand, so an aircraft parked on a stand shares that
> stand's pose exactly - which is why composing the two needs no offset of its own.*

So `ARoadAgentActor::SetPose` pinning the origin to the line is already right, and plane2's
export is what is wrong. It is being re-exported about the nose gear, which fixes the reported
symptom with no code at all - the rest of this spec is about the turn, not the tracked point.

ONE TYPE DEVIATES ON PURPOSE. `SM_PiperMeridian` is imported about the main-gear axle
(`AircraftType.cpp:178-193`) so that the drawn envelope and the drawn aeroplane come from one
measurement, and that comment closes by naming today:

> *A stand's origin is the nose gear stop mark, so composing the two needs the wheelbase, which
> the other types do not. Nothing does that yet; the day something does, that offset belongs
> here as a field and not as a constant at the call site.*

Hence `SteerAxleX` below is a field with a default of zero meaning "conforms to the convention",
not machinery for tolerating arbitrary origins. Plane2 (after its re-export), the A320 and the
737 are all zero. The Piper carries its wheelbase and nothing else has to know.

## What this project already decided, and is only approximating

`FGroundPerformance::MinTaxiSpeed` (`RoadEntity.h:328`) already states the physics this design
implements: *"A wheeled aircraft cannot yaw without rolling... It has no way to pivot on the
spot. So a turn that cannot be made at speed is made at a crawl."*

`MaxTurnRateDegPerSec` is a stand-in for geometry the code does not have. The airliner's is
hand-tuned to 8 deg/s with the comment *"37 m of aeroplane pivoting about a nose gear 12 m
ahead of the mains does not change direction quickly... the mains track well inside the nose"*
(`AircraftType.cpp:113-119`) - which is exactly what a wheelbase and a steering lock give for
free, per type, without anyone tuning it.

So this is not a new model bolted on. It is the existing comments' own model, made arithmetic.

## Decisions taken, and why

1. **Full bicycle model** rather than moving the reference point alone. Only the geometric law
   makes turns faster for a stated reason instead of a raised number.
2. **`FAgentMotion::Position` keeps meaning the airframe origin**, which for a conforming type
   IS the nose gear. The follower tracks the line with the steered axle and derives the origin
   from it, so stands, claim geometry, holding positions, runway lineup and the pose keep the
   meaning they were authored against - and for every type but the Piper the derivation is the
   identity.
3. **Swept path and claims are OUT OF SCOPE.** The mains cutting inside a corner becomes
   visible; nothing re-reserves pavement for it yet. One subsystem per spec.
4. **Two steering laws, selected by data, not by subclass.** Strategy-by-parameters, the same
   way `FRouteFollower` already reads every other per-type figure off `FAirframe`. A `UClass`
   per airframe would move figures from DA assets into C++, which is backwards for this project.
5. **The figures live on `FAirframe`, not `FEntityFootprint`.** That struct is aircraft-only on
   purpose - `UEntityDefinition:131` says *"A BOX AND NOT FEntityFootprint. That struct is
   aircraft-shaped - nose, wingspan"* - and vehicles carry a plain `FVector2D FootprintExtent`.
   Axle figures on the aircraft footprint would give the law to aeroplanes and strand every
   truck on the old path permanently.
6. **Articulation is shape, not feature.** Nothing in the game is articulated, though towing is
   anticipated (`AircraftType.cpp:107` places a `NoseGear` point commented *"where the tug
   couples"*, `EServiceRole::Tug`). The trailing computation is written as a free function so a
   towed unit is that same function applied again, and nothing more is built for it.

## Why two laws rather than one

A van is authored at `MaxTurnRateDegPerSec = 90` - *"NINE TIMES an airframe's 10 deg/s"* - with
the note *"a van CAN pivot, but a follower allowed to stop dead mid-turn would snap its heading
round rather than swing it"* (`AirsideSettings.cpp:100-106`). Put that through a bicycle model:
90 deg/s at the van's 0.5 m/s creep needs `tan d = wL/v`, about 83 degrees of lock. That is not
steering, it is pivoting. A single geometric law would quietly cripple every service vehicle.

So the discriminator is the airframe's own data:

- **Rolling-steer** (axle figures present): yaw only while moving, from the steering geometry.
- **Pivot** (no axle figures): today's flat rate, unchanged, and permanent rather than a
  migration crutch - a van genuinely is not a bicycle.

Zero therefore means "exactly what happens today", which is what makes this safe to land one
type at a time.

## The model

### New figures

`FAirframe` (the bundle BOTH agent kinds already carry into the follower):

| Field | Meaning | Unset |
|---|---|---|
| `SteerAxleX` | Steered axle, uu along local +X. Zero means the origin IS that axle, which is the class convention. Non-zero is a documented deviation - today only the Piper. | 0 |
| `FixedAxleX` | Fixed axle, uu along local +X. Aircraft: main gear. Vehicle: rear axle. Negative on a conforming type, since the mains are aft of the nose gear. | 0 |

Names carry no aeroplane in them because trucks use the same struct. Wheelbase is
`L = SteerAxleX - FixedAxleX`, derived rather than authored, so it cannot disagree with the two
positions it comes from. `HasAxles()` is `L > KINDA_SMALL_NUMBER` and is the law selector.

`FGroundPerformance`, read only by the rolling-steer law:

| Field | Meaning | Default |
|---|---|---|
| `MaxSteerDegrees` | Steering lock. | 60.0 |
| `MaxLateralAccelUu` | What a turn may pull, uu/s^2. 147 is 0.15 g. | 147.0 |

`MaxLateralAccelUu` is authored PER TYPE from the start, not left to the default, for the reason
`AirsideSettings.cpp:92` already gives about the van's figures: *"this is where a truck's
performance is DECIDED, and a reader must be able to see the figures without opening another
header to find out they happen to coincide."* A Twin Otter on a quiet apron and a 737 on a
crowded one do not corner alike, and the default exists for types nobody has thought about yet.

`MaxTurnRateDegPerSec` stays, with ONE meaning narrowed rather than two: *how fast the nose may
be swung when geometry is not what limits it*. That is the pivot law, and the take-off lineup
slew (`TakeoffRun.cpp:128`), which is not a tracked turn at all. It is NOT a ceiling on the
geometric law - the airliner's 8 deg/s would bind at every ordinary bend (a 30 m turn at 5 m/s
needs 9.5 deg/s) and defeat the whole change - and the rolling-steer path never reads it, so
the two laws cannot both claim one turn. It stays authored on every type, including those that
gain axles, because the lineup still uses it; its doc comment is rewritten to say which turns
it governs and which it does not.

### The law, and how small the diff is

`FRouteFollower::Advance` already computes the heading error at the tracked point
(`RouteFollower.cpp:88`):

```
Error = UnwindRadians(LineHeading - Heading)
```

That error IS the steering angle - the angle between the body axis and the direction the steered
axle is being asked to travel. So the whole change at that site is which line clamps it:

```
today:          Step = clamp(Error, +/- MaxTurnRate * dt)
rolling-steer:  d    = clamp(Error, +/- MaxSteerDegrees)
                Step = (Speed * sin(d) / L) * dt
```

`Speed` there is the STEERED axle's speed along the line, which is what `Follower.Speed`
already is once `Travelled` measures that point - hence `sin(d)` rather than the `tan(d)` of
the rear-axle form of the same model. Using the wrong one of those two overstates the yaw by
`1/cos(d)`, which is invisible at small angles and 2x at full lock.

No lookahead, no gain to tune, no oscillation: the steer-axle point is CONSTRAINED to the line
rather than chasing it, so there is no lateral error to feed back - only a heading that aligns
to the tangent at the rate the geometry allows. This is the kinematic bicycle and a
zero-lookahead pursuit controller at the same time, which is why it needs neither's tuning.

The crab term below it (`RouteFollower.cpp:101-118`) is unchanged and keeps its job: it measures
what the lock could NOT take out this frame, and `MinTaxiSpeed` still floors the crawl.

### Where the origin comes from

`Travelled` comes to mean the steer axle's distance along the polyline. The origin is then

```
Position = SteerPoint - SteerAxleX * (cos Heading, sin Heading)
```

which is one free function in `Solve/` (`CoreMinimal.h` only, no engine types) rather than
inline arithmetic, so a towed unit can call it again with its own offset. `SteerAxleX = 0`
returns the line point unchanged, which is today's behaviour bit for bit.

### Authored stop points do not move

`Travelled` measures the steered axle, so on a type whose origin is NOT that axle an agent told
to stop at distance D would stop with its nose gear at D rather than its origin. The follower
converts at the boundary - `StopAt_steer = StopAt_origin + SteerAxleX` - which is a no-op for
every conforming type and is exactly the composition the Piper's comment predicted would be
needed one day.

### What limits a turn now

Under the geometric law the yaw-rate cap disappears, and this is worth stating plainly because
it is the answer to "why are turns slow". Required yaw is `v/R`; available yaw is
`v sin(d_max)/L`. Speed cancels. A corner is followable if and only if

```
R >= L / sin(d_max)
```

which for plane2 is 5.24 m - tighter than any taxiway bend. Corner speed is therefore not a
steering question at all, and `FSpeedProfile`'s span cap becomes the honest physical limit,
lateral acceleration:

```
Cap = sqrt(MaxLateralAccelUu * R),   R = Length / Turn
```

At 0.15 g that is ~13 kn where a 30 m bend gives 10 today, and ~9 kn where a 15 m bend gives 5.
Faster, for a reason that can be stated rather than a figure someone typed.

Vertex caps are unchanged: a heading that changes instantly has `R = 0`, is followable at no
speed, and still falls to `MinTaxiSpeed`. Pivot-law agents keep `w_max * R` exactly as now.

### The animation, folded in

`FAgentMotion` gains `SteerAngleDegrees` - the signed `d` above, the actual control input rather
than a cosmetic derivation. `UAirsideAgentAnim` exposes it `BlueprintReadOnly` beside
`PropAngleDegrees`; `ABP_Plane2` gets one more Transform (Modify) Bone with Rotation = ADD TO
EXISTING in Bone Space, the two settings that have already been got wrong once here.

The rig needs the steer bone as a SEPARATE PARENT of the rolling nosewheel bone
(`nosewheel_steer` -> `nosewheel`). One bone cannot both yaw about the strut and roll about the
axle: a single Add-to-Existing rotation applies them in a fixed order, so the wheel wobbles
instead of steering.

Sign convention: positive `SteerAngleDegrees` is the direction that increases `Heading`. Which
way that turns the bone depends on the strut axis, so the ABP node may need a negate - confirmed
against the rig, not predicted.

## Tests

Written first, all world-free in `Model/` and `Solve/` except the last.

| Test | Pins |
|---|---|
| `Airside.Model.TurnRate` (rewritten) | The new law at a genuine 90-degree corner: heading only ever advances at `v sin(d)/L`, crab bounded and decaying. |
| `Airside.Model.NoseGearTracks` | On a bend, the steer-axle point is EXACTLY on the polyline and the fixed axle is measurably INSIDE it. This is the reported defect, pinned. |
| `Airside.Model.UnmeasuredAirframeIsUnchanged` | Axles zero: position equals the line point every frame, and the flat rate still governs. The van's 90 deg/s survives. |
| `Airside.Model.SteerNeverExceedsLock` | `d` clamped; equals `Error` while inside the lock. |
| `Airside.Model.CornerSpeedIsLateralAccel` | Measured speed through a known-radius arc matches `sqrt(a*R)`, and a straight still reaches the taxi cap. |
| `Airside.Model.CornerTighterThanLockCrawls` | `R < L/sin(d_max)` falls to `MinTaxiSpeed` rather than being taken wide. |
| `Airside.Model.AuthoredStopPointsDoNotMove` | An agent stopped at an authored distance parks its ORIGIN where it does today, to the millimetre. |
| `Airside.Present.AgentMotion` (extended) | The anim instance copies `SteerAngleDegrees` - a forwarder seam, so it gets its own assert. |
| `Airside.Content.FootprintMatchesTheMesh` | A type's `NoseX`/`TailX` match the bounds of the mesh it names. Nothing asserts this today, which is why a half-run content pipeline - new mesh, old DA - would ship an envelope 6.4 m from its aeroplane and look like a physics bug. |

## Content

**plane2 is re-exported about the nose gear**, which is what actually fixes the reported
symptom. Two scripts break on that re-export and are fixed BEFORE it lands, because both fail
in ways that look like a bad model rather than a stale script:

- `import_plane2.py` decides facing from the bounds - `nose reach > tail reach`, +811.6 against
  782.0 today. With the origin at the nose gear that becomes +167.6 against 1426 and it reports
  *"the airframe is backwards and would taxi tail-first"* on a correct export. The check moves
  to the RIG, where it belongs: the nose wheel joint is forward of the mains. True wherever the
  origin sits, and it reads the export's own authority on itself the way `joint_names()` already
  does.
- `build_plane2_type.py` hard-codes `FOOTPRINT` - `nose_x 811.6, tail_x -782.0, wing_x 204.8,
  tailplane_x -622.0` - all in the old frame, in a file whose own header says *"GEOMETRY IS
  MEASURED, PERFORMANCE IS PUBLISHED"*. They become wrong by 6.4 m on re-export. They are
  measured off `SK_Plane2` instead, which is what the header always claimed.

It then measures `FixedAxleX` (the mains, ~-454 uu once the origin is the nose gear) and authors
`MaxSteerDegrees` and `MaxLateralAccelUu`.

**The Piper is measured too**, and keeps its main-gear origin: `SteerAxleX` = its wheelbase,
`FixedAxleX` = 0. Its figures come from `SM_PiperMeridian` the way `import_piper.py` already
measures the rest, so the deviation costs one field and no second measurement.

**The A320 and 737 stay**, unmeasured, on the pivot law. They carry no mesh, but they are not
idle: `EntityDefinition.cpp:15` builds the A320 in production, `LeadInSweepTest` sizes stand
lead-in geometry against it and `StandPlaceToolTest` against the 737 - the only large footprints
in the project, and the pair the class docstring's stand/aircraft split rests on. The zero
default is what lets them sit this out rather than be deleted or invented. Their published
wheelbases are available (the A320's own comment says *"a nose gear 12 m ahead of the mains"*)
if a later pass wants them on the geometric law.

The van stays on the pivot law permanently - see "Why two laws rather than one".

## Risks

- `TakeoffRun.cpp:128` slews heading at `MaxTurnRateDegPerSec` for the runway lineup. Left alone
  deliberately: the take-off roll is straight and the figure still governs the pivot law. Named
  here so the next reader knows it was seen, not missed.
- `FSpeedProfile`'s span-cap tests pin `w*R` and will be rewritten against `sqrt(a*R)`.
- New UPROPERTYs, so a full rebuild with the editor closed.
- The two content scripts above break on a CORRECT re-export. Fixed first, or the model gets
  blamed for a stale script - which is the failure mode `import_plane2.py` was written to
  prevent and would then commit itself.
- `DA_Aircraft_Plane2` is re-authored by the script after the re-export, so its measured
  figures all move together. A partially re-run pipeline - new mesh, old DA - is an aircraft
  whose envelope is 6.4 m from its body, and nothing asserts that today. The re-run is one
  step, not two.

## Settled, previously open

1. `MaxLateralAccelUu` is per-type from the start. See the figures table.
2. The Piper is measured in this work; the A320 and 737 are kept and stay on the pivot law.
   See Content.
