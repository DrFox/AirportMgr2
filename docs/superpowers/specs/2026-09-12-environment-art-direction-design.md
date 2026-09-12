# Environment art direction, level lighting and the grass field

2026-09-12. Supersedes the one-line "Visual style: realism" in the GDD.

Concept sheet: `C:\repos\AirportMgr2Models\concepts\concept.png` ("1A Grass Airfield").

## 1. Direction

**Stylised but grounded, not cartoon.** Low-poly hard-edged forms and simplified,
largely untextured materials, but believable proportions and soft realistic lighting.

The brief that opened this discussion said "almost cartoony, bright saturated colours".
The concept sheet says the opposite in its own palette note - *"A clean, vibrant but
believable palette. Muted environments with focused colour for vehicles, markings and
signage"* - and its ground swatches measure at roughly 50% saturation, 56% value. The
sheet won, deliberately: a calm ground is what makes a yellow tug and a red-and-white
windsock read as *function* at a glance, which is the whole job of colour in a builder.
Saturating the grass would put the field in competition with the gameplay objects.

### 1.1 Palette

Sampled from the concept sheet's swatch panel, not chosen by eye. These are the source of
truth for every material authored from here.

| Row | Colours |
|---|---|
| Terrain & Ground | `#7D8E47` olive · `#748546` deep olive · `#C6B283` tan · `#7B7977` grey · `#575660` dark slate |
| Buildings | `#E4E0D9` cream · `#CDC7BF` warm grey · `#918A76` khaki · `#4F5E6A` slate blue |
| Vehicles | `#F4BA38` yellow · `#DE743E` orange · `#D4443F` red · white · `#6C98BC` pale blue |
| Markings | `#F6D857` yellow · `#222121` near-black · `#C53033` red · `#1C5FA0` blue |
| Nature | `#5E7065` sage · `#745137` brown |

## 2. World scale

**Plot: 3,024 m square.** Field lengths compress to 0.6x real; aircraft, aprons and
stands stay real size.

### 2.1 Why compressed at all

`SimClock.h:30-34` states the contract: *"Airside agents run on `Multiplier()` alone,
never on `TimeScale()`"*. Ground movement is therefore **not** day-compressed. With
`RealSecondsPerGameDay = 1200` the clock runs 72x while aircraft taxi at 1-8x, so
distance is charged in *real seconds* against a game day only 20 real minutes long. At
`SpeedCap = 1000` uu/s (`RoadEntity.h:95`, 10 m/s):

| Taxi | at x1 | at x8 | share of a x8 game day (150 s) |
|---|---|---|---|
| 1 km | 100 s | 12 s | 8% |
| 2.5 km | 250 s | 31 s | 21% |
| 4 km | 400 s | 50 s | 33% |

A real-scale hub charges a third of a game day per long taxi. That is the cost the
compression buys off. The concept sheet's in-game view has already made this decision -
hangar, apron, car park and threshold sit within a few hundred metres of each other,
which no real airfield does.

### 2.2 Why 0.6x, and why the fleet is not capped

| | Real TOFL, MTOW, sea level | At 0.6x |
|---|---|---|
| Piper Meridian | 510 m | 306 m |
| A320neo | 2,100 m | 1,260 m |
| 787-9 | 2,800 m | 1,680 m |
| A380-800 | 3,000 m | 1,800 m |
| 777-300ER | 3,300 m | 1,980 m |

The longest runway the game ever needs is 1,980 m, inside a 3,024 m plot. That leaves a
kilometre along the axis for turn-offs and RESA and the full width for taxiways, aprons
and terminal. **Nothing is excluded - the A380 endgame survives.**

Real scale was considered and rejected: it needs a 6 km plot (576 landscape components),
puts the longest taxis in the 33% row, and requires the camera to pull back to 3 km.

**Slice E is not a blocker for A-D.** Until the compression lands, real field lengths
apply, and every current type still fits: the Piper needs 510 m and an A320 2,100 m
against a 3,024 m plot. Only a fully-laden 777 would want more room than the plot leaves
beside the runway, and nothing offers one yet.

### 2.3 Landscape parameters

```
Section size            63 x 63 quads
Sections per component   2 x 2          -> 126 quads per component
Component count         12 x 12
Scale                   X 200  Y 200  Z 100
Location                0, 0, 0         (the New Landscape tool centres it)
```

3,024 m square, **2 m per quad**, 144 components.

Two sections per component rather than one: each component then covers 4x the area for
the same component count. The cost is coarser LOD granularity per component, which is
worth nothing on dead-flat ground and saves 4x the components.

2 m is the *paint* resolution, and that is what sets it - fine enough that a worn dirt
patch at a hangar door has a believable edge once the material noise-breaks the blend.
1 m would quadruple the component count for the same ground.

Z = 0 is safe: `RoadNetworkActor.h:819` sets `SurfaceZ = 10.0`, so road plates sit 10 cm
proud of the landscape. No z-fighting, and the 10 cm lip reads as the kerb that already
exists.

## 3. Scope: seven slices, three in this spec

| | Slice | This spec |
|---|---|---|
| A | Lighting, sky, post-process | yes |
| B | Landscape + `M_Ground` | yes |
| G | Sun tracks the game clock, floored at dusk | yes, after A and B |
| C | Grass scatter - `LandscapeGrassType` + clump meshes | no |
| D | The surround - farmland, hedgerows, trees past the fence | no |
| E | Field-length compression (section 2.2) | no - own spec |
| F | Camera `MaxViewDistance` 600 m -> ~1.5 km | no - own change |

**C and D wait** because the camera lives between 6 m and 600 m
(`BuildCameraRig.h:40-49`) and above ~100 m grass blades are sub-pixel. A+B is what makes
the view read correctly at the height the game is actually played at. C pays off on close
zoom, D at the plot edge.

**G lands after A and B**, not beside them: a fixed sun has to look right first,
because the editor viewport and the first PIE frame both have no time of day to read.

**E and F are excluded deliberately.** E is gameplay code with a test contract to honour,
not environment work, and nothing in A-D depends on it - the landscape is 3,024 m either
way. F is a one-property change whose default may already be dead (see 7.2).

## 4. Slice A - lighting

Every actor below already exists in `M_Starter` except the post-process volume; the level
is the stock UE Basic template.

### 4.1 PostProcessVolume (new)

Unbounded (`bUnbound = true`). **The highest-value change in this spec.**

- **Exposure locked** - `AutoExposureMinBrightness` = `AutoExposureMaxBrightness`,
  both overridden, **at an EV100 of roughly 13-15**, tuned by eye against a screenshot.
  Auto-exposure re-brightens the frame as the player pans across a dark hangar, which
  reads as a rendering bug in a builder, and it makes colour judgement impossible because
  nothing holds still. This is the single most common cause of a UE scene "looking wrong".

  **The units are the trap.** `Scene.h:1988-1991`: *"Eye Adaptation is disabled if Min =
  Max ... The Min/Max are expressed in pixel luminance (cd/m2) or in EV100 when using
  ExtendDefaultLuminanceRange."* This project **does** set
  `r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True`, so
  `Scene.cpp:499-506` gives the engine defaults as -10.0 and 20.0 **in EV100**. A sunlit
  exterior sits near EV100 13-15; a value of 1 would be a dim indoor room and the screen
  would blow out white. An earlier draft of this spec said "min = max = 1.0" - right
  mechanism, wrong units. Setting `AutoExposureMethod = AEM_Manual` is the alternative
  lock, driven by aperture/shutter/ISO instead; Min = Max is simpler and is what this
  spec uses.

  Slice G inherits this: with exposure pinned to a daylight EV, dusk genuinely darkens
  rather than being compensated. That is the intent, and it is why section 5's floor
  intensity has to be measured.
- Bloom 0.4. Vignette 0. Chromatic aberration 0. Film grain 0. Stylised means clean.
- **Motion blur off.** A panning top-down camera plus motion blur is nausea.
- Grade: global saturation x1.05; shadows tinted slightly blue. The blue shadow tint fakes
  sky bounce and is what stops the olive greens reading muddy.
- Ambient occlusion left modest - Lumen already provides occlusion, and screen-space AO on
  top double-darkens contacts.

`Config/DefaultEngine.ini` already sets local exposure highlight/shadow contrast to 0.8.
Those stay.

### 4.2 DirectionalLight

Already Movable. Set:

- `Atmosphere Sun Light` on, so SkyAtmosphere takes its sun from this light.
- Rotation pitch -42 deg, yaw 150 deg. Chosen by eye against the sheet's shadow direction;
  a starting value, not a finding. **Slice G turns this pair into the noon point of a
  curve** rather than a magic number - see section 5.
- Temperature ~5800 K for a slightly warm sun.
- **`LightSourceAngle` 0.5357 -> 1.5 deg.** The engine default
  (`DirectionalLightComponent.cpp:1050`) is the real sun's angular diameter, which gives
  razor-sharp shadow edges. The concept renders have a soft penumbra. Nearly free.

### 4.3 SkyLight

Movable, and **Real Time Capture asserted on**. The component default is
`bRealTimeCapture = false` (`SkyLightComponent.cpp:331`); the template may or may not have
set it. With a moving sun a static capture bakes the wrong ambient for 23 hours of 24. The
script asserts the value rather than assuming it.

`bLowerHemisphereIsBlack` stays at its default `true` - Lumen supplies the ground bounce,
and a lit lower hemisphere on top of it double-counts.

### 4.4 ExponentialHeightFog

Dialled well down. SkyAtmosphere's aerial perspective already provides distance haze; the
stock fog is grey and flattens the scene. Volumetric fog stays off - it mostly buys god
rays, at a cost.

### 4.5 VolumetricCloud

**Kept as-is for now**, on `m_SimpleVolumetricCloud_Inst`. It is expensive and barely
visible below a 600 m camera, so it is a candidate for removal - but the decision is to
see what it adds before dropping it. Revisit after the first screenshots.

### 4.6 Deletions

- `Floor_0` (`SM_Template_Map_Floor`) - replaced by the Landscape.
- `StaticMeshActor_0` (`SM_SkySphere` on `M_SimpleSkyDome`) - **deleted 2026-09-12, and
  measured first.** It contributes nothing: a before/after pair at the same locked exposure
  differs by 0.37 / 255 mean (max 2) in the band below the horizon where it would show.
  Note the trap that nearly gave the wrong answer - the WHOLE-image diff read 12.7 / 255,
  which looks like a real contribution and is entirely the volumetric clouds animating
  between the two captures (18.94 in the sky band, 0.37 below the horizon). **Diff by band,
  not by frame**, whenever anything in shot animates.

### 4.7 What is NOT changed, and why

`DynamicShadowDistanceMovableLight` defaults to 40,000 uu = 400 m
(`DirectionalLightComponent.cpp:1043`) against a 600 m max camera distance, which looks
like a shadow cut-off waiting to happen. Virtual Shadow Maps are on
(`r.Shadow.Virtual.Enable=1`) and use clipmaps instead, and a grep of the renderer found
nothing consuming that field. So it is left alone and **verified by a zoomed-out
screenshot** rather than pre-emptively changed.

## 5. Slice G - the sun tracks the game clock

Lands **after** A and B are on screen. A and B must stand on their own with a fixed sun,
because the editor viewport and PIE-before-the-clock-ticks both have no time of day.

### Why floored, and not a real day/night cycle

`RealSecondsPerGameDay = 1200` (`SimClock.h:57`) - 20 real minutes per game day at x1. A
literal sun sweeps **360 deg in 20 minutes (18 deg/min)**, and **144 deg/min at x8**.
Roughly 40% of a day is dark, so a literal cycle spends about 8 of every 20 real minutes
in the dark.

That dark would be total. There are no runway edge lights, no taxiway centreline lights,
no apron floods, no lit windows, no headlights - and section 4.1 deliberately locks
exposure, so nothing compensates. An unreadable 40% of play time is a defect, not a mood.

The concept sheet already anticipated this: the Lighting Mast is captioned *"For when you
operate later."* **Night operations are a designed progression unlock.** Arriving at night
before the lights exist gets the order backwards.

So: the sun follows the clock across an arc **floored above the horizon**. The player gets
what is actually worth having - shadows rotating and lengthening, light warming toward
evening, the field reading differently at 08:00 and 17:00 - with no unplayable dark and no
new assets. When the lighting mast and runway lights land, the floor lifts and true night
becomes the unlock the sheet implies.

### Shape

Azimuth sweeps the full 360 deg over the game day, continuously, so shadows rotate right
round and there is no jump at midnight. Elevation follows a sine peaking at noon and
**clamped to `MinElevationDegrees`** rather than going negative. Between 18:00 and 06:00
the sun therefore sits low while its azimuth continues north - which reads as a long
high-latitude twilight, and is a real thing the sky does.

Starting values, all tunable, none of them findings:

| | Value | Note |
|---|---|---|
| `MaxElevationDegrees` | 42 | section 4.2's -42 deg pitch, now the noon peak |
| `NoonAzimuthDegrees` | 150 | section 4.2's yaw |
| `MinElevationDegrees` | 8 | the dusk floor |
| Temperature | 3200 K at floor -> 5800 K at noon | warmth is most of what sells time of day |
| Intensity | ~35% of noon at floor | with exposure locked, this sets how dark dusk reads |

The intensity and temperature ends are the values most likely to move. With exposure
locked and SkyAtmosphere reddening a low sun, dusk may land too dark to play - that is
measured against a screenshot, not argued.

### Structure

`FSunPath` - a **plain struct**, no UObject, no world: it maps a time-of-day fraction to a
rotation, a colour temperature and an intensity, and knows nothing about lights, actors or
clocks. Same shape and same reasoning as `FBuildCameraRig` (`BuildCameraRig.h:5-17`), and
world-free means it is unit-testable with `NewObject` and no level.

`ASunDriver` - a small actor in `Source/AirportMgr`, holding
`UPROPERTY(EditAnywhere) TObjectPtr<ADirectionalLight> Sun` and the `FSunPath` tunables.
It reads `UOpsRuntimeSubsystem` -> `OpsRuntime::GetClock()` (`OpsRuntime.h:41`;
`AirportMgr.Build.cs` already has `AirportOps` as a public dependency) and applies the
result. A forwarder, nothing more.

An explicit level-authored light pointer rather than a `TActorIterator<ADirectionalLight>`
search: the search silently picks one of two lights, and this project puts knobs in
properties on purpose. **Falls back to noon when there is no clock**, which is what the
editor viewport and the first PIE frame both need.

### Tests

`FSunPath` is world-free, so these are plain automation tests:

- noon returns `MaxElevationDegrees`
- elevation never drops below `MinElevationDegrees`, at any fraction including midnight
- azimuth is continuous across midnight - no wrap discontinuity
- elevation rises monotonically from dawn to noon
- `ASunDriver` with no clock applies the noon rotation

The last one is the seam test the refactor contract asks for: it fails if the driver is
spawned but never wired.

## 6. Slice B - ground

### 6.1 Layer info assets

Three `ULandscapeLayerInfoObject` assets in `/Game/Environment/`:
`LI_GrassMown`, `LI_GrassRough`, `LI_Dirt`.

### 6.2 M_Ground

A `Landscape Layer Blend` node over three layers:

| Layer | Base colour | Use |
|---|---|---|
| `GrassMown` | `#7D8E47` | the field; default coverage |
| `GrassRough` | `#748546` blended toward `#C6B283` | unmaintained edges, past the fence |
| `Dirt` | `#C6B283` | worn ground at hangar doors and parking |

Each layer is a flat base colour multiplied by two octaves of noise for macro variation.
Roughness constant per layer, metallic zero.

**No normal maps.** At 6-600 m a grass normal map reads as mush; the stylised look comes
from colour, and the surface detail comes from the grass clumps in Slice C. Adding normals
now would be work that Slice C makes invisible.

Scriptable: **verified**, see section 11.1 for the working recipe and the readback gotcha.

## 7. Authoring and division of labour

### 7.1 Who does what

**You, in the editor (once):** create the Landscape with the section 2.3 parameters, and
assign `M_Ground` to it.

**Scripted, `Tools/Python/build_environment.py`:** the three layer info assets,
`M_Ground`, the lighting actor settings, the post-process volume, and the deletions. It
follows the existing `Tools/Python/build_*.py` convention - headless commandlet, every
result line prefixed `MARKER:` so it can be grepped out of the log.

### 7.2 Why the Landscape is not scripted

**It cannot be.** `ALandscapeProxy::Import` (`LandscapeProxy.h:1418`) is the only function
that creates one, it sits inside the `#if WITH_EDITOR` block opened at line 1326, and it
carries **no `UFUNCTION` macro** - so it is invisible to reflection and therefore to
Python. Everything Landscape exposes to script (`Landscape.h:340-400`) is read-only:
`GetGrassEnabled`, `RenderHeightmap`, `GetTargetLayerNames`.

The alternative considered and rejected: a `UAirsideLandscapeTools` blueprint function
library in the `AirsideEditor` module wrapping the engine's own creation sequence
(`LandscapeEditorDetailCustomization_NewLandscape.cpp:1221-1263`). It works, and it would
make the map reproducible from nothing, but it costs a full build and pins the project to
an engine-internal signature for a job done once. Not worth it for a single Landscape.

The consequence is stated plainly: **the map is not rebuildable from zero.** One manual
step stands in the way. Everything else is.

## 8. Verification

No claim of "looks right" without an image. After the script runs:

1. Open the editor.
2. `python Tools/Mcp.py shot` at three camera distances - **20 m, 150 m, 600 m**.
3. Put all three in front of the user.

Specific things the screenshots must answer, because reasoning cannot:

- Does `SM_SkySphere` occlude the atmosphere and clouds (4.6)?
- Do shadows survive to the far edge of the frame at 600 m (4.7)?
- Does the 10 cm road lip read as a kerb or as a floating plate from 20 m (2.3)?
- Is the VolumetricCloud actor visible enough to justify its cost (4.5)?
- **After Slice G:** does a sun sweeping 144 deg/min at x8 make Lumen shimmer or
  ghost, with a RealTimeCapture SkyLight recapturing behind it (section 5)? This was
  flagged as Lumen's known weakness when the stack was chosen. Screenshot at x8;
  do not reason about it.
- **After Slice G:** is the dusk floor still playable with exposure locked (section 5)?

## 9. GDD change

`docs/AirportManagerGDD.md` line 30 currently reads **"Visual style: realism."** It is
replaced by a reference to this document. Design decisions in this project are revisable
with reasons recorded; this is the record.

## 10. Out of scope, named so it is not forgotten

- **Slice C**, grass scatter: `LandscapeGrassType` assets driven from an
  `M_Ground` Landscape Grass Output, plus low-poly clump meshes. Needs the
  `GeometryScripting` plugin enabled if the clumps are to be authored headlessly.
- **Slice D**, the surround: farmland fields, hedgerows and trees beyond the plot, so the
  3 km square does not end in void. The concept sheet's in-game view shows exactly this.
  **Raised in priority 2026-09-12, after the first screenshots.** From 200 m the landscape
  edge is plainly visible and everything past it is a flat dark navy band up to the horizon
  - the SkyAtmosphere's ground, with nothing on it. It reads as the world ending, and it is
  considerably more prominent than this spec assumed when it deferred the slice. Not caused
  by the SkySphere: see section 4.6 for the measurement that rules that out.
- **Slice E**, field-length compression. One named factor,
  `UAirsideSettings::FieldScale = 0.6`, applied in exactly one function. Authored assets
  keep the **real POH figures** - reality stays the source of truth - and the scale is
  applied on resolve: `Takeoff.Accel` and `Landing.Decel` x 1/S, published field lengths
  x S. Both sides scale together, so `Airside.Model.FieldLengthsCoverTheRoll`
  (`FieldLengthTest.cpp:26-32`) keeps passing rather than being weakened. The `Taxi`
  regime is untouched, so aircraft still taxi at real speed. Visible cost: a take-off roll
  runs 0.6x its real duration - an A320 rotates in ~21 s rather than ~35 s.
- **Slice F**, camera range. `MaxViewDistance = 60000.0` is `EditAnywhere`
  (`RoadBuildController.h:199`), so the constructor default may already be overridden by
  `BP_RoadBuildGameMode` or a level instance. Read the instance before changing the
  constructor.

## 11. What is verified, and what is not

Checked in session against engine source or this repository, each cited at its file and
line above: no toon shading model; Landscape unreachable from Python; the DirectionalLight
and SkyLight defaults; `SurfaceZ`; the camera range; the SimClock two-clock contract; the
field-length test; the exposure units; the landscape arithmetic; the palette.

Also checked and dismissed as irrelevant:

- **Substrate is off.** `r.Substrate` defaults to 0 (`RenderUtils.cpp:2068`) and the
  comment there confirms an existing project stays off. Material authoring is classic PBR.
- **MegaLights is off**, `r.MegaLights.EnableForProject` = 0, and its own description says
  it *"does not support Directional Lights"* - so it cannot matter to a sun-lit exterior.
- **Python can author material node graphs**, not just instances: the existing
  `Tools/Python/build_*.py` use `unreal.MaterialEditingLibrary.create_material_expression`
  throughout.

### 11.0 Mechanism is verified; craft is not

Everything above is *mechanism* - how the engine behaves - and it is grepped from engine
source. **Craft is a different question and is not verified.** Section 6.2's "flat base
colour times two octaves of noise" is reasoning, not a cited technique, and stylised grass
(Slice C) has known pitfalls that a naive `LandscapeGrassType` setup walks straight into.

So **Slices B and C each open with a short technique research step**, done just before the
slice rather than now - research against a real screenshot beats research against an
imagined one, and Slice A needs none of it.

Two things to confirm rather than assume when that happens:

- Grass clump normals probably need pushing toward the landscape normal (or straight up)
  rather than using the mesh's own, or blades self-shade into dark noise. Believed to be
  the standard fix; unverified in 5.8.
- The landscape's own colour has to match the grass colour at the cull distance, or a
  visible ring appears where clumps stop. Confident in the principle, not in the numbers.

**Caution on sources.** Most published "stylised UE landscape" work targets a saturated
Ghibli or Zelda look - the opposite of section 1's muted ground with colour reserved for
vehicles. The concept sheet is a better reference for LOOK than anything findable;
research is for TECHNIQUE only.

### 11.1 Wiring the layer blend from Python - RESOLVED, it works

Spiked headlessly on 2026-09-12 and settled by measurement, not argument. The recipe,
verified end to end:

```python
blend = lib.create_material_expression(mat, unreal.MaterialExpressionLandscapeLayerBlend, x, y)

item = unreal.LayerBlendInput()
item.set_editor_property("layer_name", "GrassMown")
item.set_editor_property("blend_type", unreal.LandscapeLayerBlendType.LB_WEIGHT_BLEND)
item.set_editor_property("preview_weight", 1.0)
blend.set_editor_property("layers", [item, ...])

# The per-layer pin is named "Layer <LayerName>".
lib.connect_material_expressions(colour, "", blend, "Layer GrassMown")
lib.connect_material_property(blend, "", unreal.MaterialProperty.MP_BASE_COLOR)
```

**The gotcha, recorded so it is not rediscovered.** `FLayerBlendInput` exposes
`layer_name`, `blend_type`, `preview_weight`, `const_layer_input` and `const_height_input`
to Python, but **NOT `layer_input` or `height_input`** - those two are `UPROPERTY` with no
`EditAnywhere`, so reflection skips them and `get_editor_property("layer_input")` raises
*"Failed to find property"*. Connections therefore cannot be read back through the normal
property API.

That matters because `connect_material_expressions` returning `True` is not evidence the
wiring survived a save. **Read it back with `export_text()` on the layer struct**, which
serialises the whole `FExpressionInput`. After the spike's save and reload, the two layers
carried distinct expressions - `MaterialExpressionConstant3Vector_0` and `_1` - which is
the proof.

## 12. Considered and rejected

Two Fab products were weighed against this spec on 2026-09-12. Both are in the library;
neither was installed for 5.8 at the time (UDS's content was a 5.4 build, and deleted;
Landscape Background's cached manifest was 5.1).

**Landscape Background** - rejected on look. Reviewed against the concept sheet and judged
not to fit the stylised direction section 1 sets. Slice D stays hand-built, which also
keeps the plot edge under our own control.

**Ultra Dynamic Sky** - rejected, despite being a genuinely good fit on paper. It would
have subsumed Slice G entirely: it carries its own Time of Day, and its moon, stars and
configurable night brightness solve the hard part of section 5 - the dusk floor becomes
tuning rather than a curve we write.

Rejected anyway for two reasons that outweigh that:

- **It is photoreal by default.** Reaching the stylised look of section 1 would mean
  fighting a system built for the opposite, across hundreds of parameters. A small system
  that starts where we want to end up beats a large one tuned backwards into it.
- **It is Blueprint.** Section 5 rests on `FSunPath` being a plain world-free struct with
  unit tests - noon elevation, the floor, azimuth continuity across midnight. None of
  those tests can exist against a Blueprint. That trade runs against how this whole
  codebase is built.

Secondary: it is a content pack, so it lands in `Content/` and the repository grows by its
size; and per-frame Blueprint plus volumetric clouds is a poor trade in a top-down builder
where the sky is a small fraction of the frame.

**What NOT to lose with it.** UDS bundled a weather system, and weather is a real airport
mechanic rather than decoration - crosswind limits, low visibility holding inbounds, snow
closing a runway. **None of that needs UDS.** It is `Model/` code, world-free and testable
like everything else there, and its visual side is modest. Dropping the plugin is not a
decision about weather; that remains open and worth doing.

**Cel shading** - rejected, 2026-09-12. UE 5.8 has no toon shading model
(`EngineTypes.h:709-721` lists all thirteen), so it would mean one of: forking the engine
to add `MSM_Toon` (this project builds against stock `D:/Epic/UE_5.8`); a post-process
blendable that quantises the final image, which bands the sky and clouds too and fights
Lumen; or unlit materials with hand-computed N.L, which discards Lumen, shadows and the
sun entirely.

The decisive argument is not cost, though. **Cel shading and Slice A are substitutes, not
complements.** Quantising to three bands throws away precisely what section 4's stack was
chosen to buy - Lumen's bounce into a hangar, the 1.5 deg soft penumbra, the real-time sky
capture, and Slice G's light warming toward evening. Adopting it would mean DELETING most
of Slice A, not adding to it. And the concept sheet has no banded terminators anywhere:
the clubhouse and hangar are shaded with smooth gradients and corner occlusion.

**The stylisation is already here, it just is not in the shading.** It lives in the
geometry and the materials - hard-edged low-poly forms, flat base colours, no normal maps
(section 6.2), restrained bloom, zero grain (section 4.1). The hard edges come from
silhouettes, not from light.

If this is ever revisited, **decide it before Slice A rather than after**: the engine-fork
question is far cheaper to answer before a level is tuned against Lumen.

**Outlines are a separate question and stay open.** A post-process Sobel on depth plus
normals gives a dark line on silhouettes and creases without any banding, and in a
top-down builder it is arguably functional - it separates a building from the apron it
stands on. Risk: at 600 m every grass clump and road edge gets a line too, so the depth
threshold needs tuning by eye and it may simply read as noise. Worth trying once Slice C
exists and there is something worth outlining.
