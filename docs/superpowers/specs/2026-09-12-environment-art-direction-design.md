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

## 3. Scope: four slices, two in this spec

| | Slice | This spec |
|---|---|---|
| A | Lighting, sky, post-process | yes |
| B | Landscape + `M_Ground` | yes |
| C | Grass scatter - `LandscapeGrassType` + clump meshes | no |
| D | The surround - farmland, hedgerows, trees past the fence | no |
| E | Field-length compression (section 2.2) | no - own spec |
| F | Camera `MaxViewDistance` 600 m -> ~1.5 km | no - own change |

**C and D wait** because the camera lives between 6 m and 600 m
(`BuildCameraRig.h:40-49`) and above ~100 m grass blades are sub-pixel. A+B is what makes
the view read correctly at the height the game is actually played at. C pays off on close
zoom, D at the plot edge.

**E and F are excluded deliberately.** E is gameplay code with a test contract to honour,
not environment work, and nothing in A-D depends on it - the landscape is 3,024 m either
way. F is a one-property change whose default may already be dead (see 6.2).

## 4. Slice A - lighting

Every actor below already exists in `M_Starter` except the post-process volume; the level
is the stock UE Basic template.

### 4.1 PostProcessVolume (new)

Unbounded (`bUnbound = true`). **The highest-value change in this spec.**

- **Exposure locked** - auto-exposure min brightness = max brightness = 1.0.
  Auto-exposure re-brightens the frame as the player pans across a dark hangar, which
  reads as a rendering bug in a builder, and it makes colour judgement impossible because
  nothing holds still. This is the single most common cause of a UE scene "looking wrong".
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
  a starting value, not a finding.
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
- `StaticMeshActor_0` (`SM_SkySphere` on `M_SimpleSkyDome`) - **only if measured.** The
  suspicion is that this legacy dome occludes SkyAtmosphere and the clouds, but that is a
  guess. Screenshot with it present and absent, compare, then decide.

### 4.7 What is NOT changed, and why

`DynamicShadowDistanceMovableLight` defaults to 40,000 uu = 400 m
(`DirectionalLightComponent.cpp:1043`) against a 600 m max camera distance, which looks
like a shadow cut-off waiting to happen. Virtual Shadow Maps are on
(`r.Shadow.Virtual.Enable=1`) and use clipmaps instead, and a grep of the renderer found
nothing consuming that field. So it is left alone and **verified by a zoomed-out
screenshot** rather than pre-emptively changed.

## 5. Slice B - ground

### 5.1 Layer info assets

Three `ULandscapeLayerInfoObject` assets in `/Game/Environment/`:
`LI_GrassMown`, `LI_GrassRough`, `LI_Dirt`.

### 5.2 M_Ground

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

## 6. Authoring and division of labour

### 6.1 Who does what

**You, in the editor (once):** create the Landscape with the section 2.3 parameters, and
assign `M_Ground` to it.

**Scripted, `Tools/Python/build_environment.py`:** the three layer info assets,
`M_Ground`, the lighting actor settings, the post-process volume, and the deletions. It
follows the existing `Tools/Python/build_*.py` convention - headless commandlet, every
result line prefixed `MARKER:` so it can be grepped out of the log.

### 6.2 Why the Landscape is not scripted

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

## 7. Verification

No claim of "looks right" without an image. After the script runs:

1. Open the editor.
2. `python Tools/Mcp.py shot` at three camera distances - **20 m, 150 m, 600 m**.
3. Put all three in front of the user.

Specific things the screenshots must answer, because reasoning cannot:

- Does `SM_SkySphere` occlude the atmosphere and clouds (4.6)?
- Do shadows survive to the far edge of the frame at 600 m (4.7)?
- Does the 10 cm road lip read as a kerb or as a floating plate from 20 m (2.3)?
- Is the VolumetricCloud actor visible enough to justify its cost (4.5)?

## 8. GDD change

`docs/AirportManagerGDD.md` line 30 currently reads **"Visual style: realism."** It is
replaced by a reference to this document. Design decisions in this project are revisable
with reasons recorded; this is the record.

## 9. Out of scope, named so it is not forgotten

- **Slice C**, grass scatter: `LandscapeGrassType` assets driven from an
  `M_Ground` Landscape Grass Output, plus low-poly clump meshes. Needs the
  `GeometryScripting` plugin enabled if the clumps are to be authored headlessly.
- **Slice D**, the surround: farmland fields, hedgerows and trees beyond the plot, so the
  3 km square does not end in void. The concept sheet's in-game view shows exactly this.
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
