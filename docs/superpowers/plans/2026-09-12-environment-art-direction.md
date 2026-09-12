# Environment Art Direction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `M_Starter` a stylised-but-grounded look — locked-exposure lighting, a 3 km flat Landscape with a three-layer stylised ground material, and a sun that tracks the game clock without ever going unplayably dark.

**Architecture:** Three slices. Slice A sets the level's lighting actors and adds an unbounded post-process volume, all from one headless Python script. Slice B is a Landscape the user creates by hand (the engine exposes no way to script it) plus a scripted `M_Ground` layer-blend material. Slice G adds `FSunPath`, a plain world-free struct that maps a day fraction to a sun rotation, temperature and intensity, driven by a thin `ASunDriver` actor reading `USimClock`.

**Tech Stack:** UE 5.8 (stock, `D:/Epic/UE_5.8`), C++ for Slice G, headless Python commandlet for Slices A and B, `Tools/Run-AirsideTests.ps1` for automation tests.

**Spec:** `docs/superpowers/specs/2026-09-12-environment-art-direction-design.md`

## Global Constraints

- **The editor must be CLOSED** for every `Build.bat` run and every Python commandlet in this plan. Live Coding holds the DLLs.
- **Python scripts run headless:** `UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<file> -unattended -nosplash -nopause`. `print()` goes to the log, not stdout — every result line is prefixed `MARKER:` so it can be grepped from `Saved/Logs/AirportMgr.log`.
- **A headless level edit can report success while doing nothing** (saved memory: locked `.umap`). Every task that writes the level ends by RELOADING it from disk and asserting the values back.
- **A new test `.cpp` needs TWO builds.** The first reports `Result: Succeeded` without compiling it. Confirm by the test COUNT in the runner output, not by the exit code.
- **Never trust the automation runner's exit code.** Read its `N test(s) run, N failed, N crashed` line.
- **Tests are named `Airside.*`** so `Run-AirsideTests.ps1` picks them up by default, even though Slice G lives in the game module. Name leaf tests distinctly — the UE automation tree drops a bare-named parent once a dotted child exists.
- **`Tools/Check-Architecture.ps1` is the pre-commit lint** and runs first inside the test script.
- **Palette, verbatim from spec §1.1:** `GrassMown` `#7D8E47`, `GrassRough` `#748546`, `Dirt` `#C6B283`.
- **Landscape, verbatim from spec §2.3:** section 63×63 quads, 2×2 sections per component, 12×12 components, scale `X 200 Y 200 Z 100`, location `0,0,0`. Result: 3,024 m square, 2 m per quad, 144 components.
- **Exposure lock, verbatim from spec §4.1:** `auto_exposure_min_brightness` == `auto_exposure_max_brightness`, both overridden, **in EV100**, starting at **14.0**. This project sets `ExtendDefaultLuminanceRange=True`, which makes the units EV100. A value of 1.0 blows the screen to white.

## Build and test commands

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex

./Tools/Check-Architecture.ps1
./Tools/Run-AirsideTests.ps1 -Filter Airside.Sky
```

## File Structure

| File | Responsibility |
|---|---|
| `Tools/Python/build_environment.py` | **Create.** Slice A: sets the level's lighting actors, spawns the post-process volume, deletes the template floor. Applies, then reloads and verifies. |
| `Tools/Python/verify_landscape.py` | **Create.** Slice B: asserts the hand-made Landscape matches spec §2.3. |
| `Tools/Python/build_ground_material.py` | **Create.** Slice B: authors the three `ULandscapeLayerInfoObject` assets and `M_Ground`. |
| `Source/AirportMgr/SunPath.h` / `.cpp` | **Create.** Slice G: `FSunLighting` and `FSunPath`. Plain structs, no UObject, no world. |
| `Source/AirportMgr/SunPathTest.cpp` | **Create.** Five tests for the curve. |
| `Source/AirportMgr/SunDriver.h` / `.cpp` | **Create.** Slice G: `ASunDriver`, a forwarder actor. |
| `Source/AirportMgr/SunDriverTest.cpp` | **Create.** The seam test: no clock means noon. |
| `Content/Maps/M_Starter.umap` | **Modify** via script and by hand. |

`FSunPath` is a **plain struct, not a `USTRUCT`**, and `ASunDriver` holds its tunables as separate `UPROPERTY` doubles copied onto the struct — exactly the pattern `ARoadBuildController` already uses for `FBuildCameraRig` via `ApplyViewLimits` (`RoadBuildController.cpp:132-138`). This keeps the curve unit-testable with no UHT involvement.

---

### Task 1: Slice A — lighting and post-process

**Files:**
- Create: `Tools/Python/build_environment.py`
- Modify: `Content/Maps/M_Starter.umap` (written by the script)

**Interfaces:**
- Consumes: nothing.
- Produces: a lit `M_Starter` with an unbounded `APostProcessVolume`. No C++ symbols.

There is no unit test for a `.umap` edit, so **the script tests itself**: it applies, reloads the level from disk, and prints `MARKER: PASS`/`FAIL` per property. That readback is the test, and it exists because headless level edits are known to report success while doing nothing.

- [ ] **Step 1: Write the script**

Create `Tools/Python/build_environment.py`:

```python
"""Sets M_Starter's lighting for the stylised art direction and adds the post-process
volume. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

THE EXPOSURE LOCK IS THE POINT. Auto-exposure re-brightens the frame as the player pans
across a dark hangar, which reads as a rendering bug in a builder and makes judging a
colour impossible because nothing holds still. Scene.h:1988 - "Eye Adaptation is disabled
if Min = Max".

AND THE UNITS ARE EV100, NOT LINEAR. Config/DefaultEngine.ini sets
r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange=True, which per Scene.cpp:499-506
switches Min/Max to EV100. A sunlit exterior is near EV100 14; a value of 1 is a dim
interior and the screen blows out white.

APPLIES, THEN RELOADS AND VERIFIES. A headless level edit can report success and write
nothing (a locked .umap does exactly that), so returning without reading the values back
off disk would prove nothing.
"""
import unreal

LEVEL = "/Game/Maps/M_Starter"

# EV100. Tuned by eye against the screenshots; see spec section 4.1.
EXPOSURE_EV100 = 14.0

# Spec section 4.2. Slice G turns this pair into the noon point of a curve.
SUN_PITCH = -42.0
SUN_YAW = 150.0
SUN_TEMPERATURE = 5800.0

# Engine default is 0.5357 - the real sun's angular diameter, hence razor-sharp shadow
# edges. The concept renders have a soft penumbra, and widening the source is nearly free.
SUN_SOURCE_ANGLE = 1.5


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def find_one(cls):
    """The single actor of this class, or None. Two is a level authoring error, not a
    thing to pick between silently."""
    found = [a for a in actors().get_all_level_actors() if isinstance(a, cls)]
    if len(found) != 1:
        fail("expected exactly 1 %s, found %d" % (cls.__name__, len(found)))
        return None
    return found[0]


def set_sun(light):
    comp = light.get_component_by_class(unreal.DirectionalLightComponent)
    light.set_actor_rotation(unreal.Rotator(0.0, SUN_PITCH, SUN_YAW), False)
    comp.set_editor_property("atmosphere_sun_light", True)
    comp.set_editor_property("light_source_angle", SUN_SOURCE_ANGLE)
    comp.set_editor_property("use_temperature", True)
    comp.set_editor_property("temperature", SUN_TEMPERATURE)
    say("sun set: pitch %.1f yaw %.1f source angle %.2f" % (SUN_PITCH, SUN_YAW, SUN_SOURCE_ANGLE))


def set_sky_light(sky):
    """Real Time Capture ASSERTED, not assumed. SkyLightComponent.cpp:331 defaults
    bRealTimeCapture to false; with a moving sun (Slice G) a static capture bakes the
    wrong ambient for 23 hours of every 24."""
    comp = sky.get_component_by_class(unreal.SkyLightComponent)
    comp.set_editor_property("real_time_capture", True)
    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    say("sky light: real time capture on")


def set_fog(fog):
    """Dialled well down. SkyAtmosphere's aerial perspective already gives distance haze;
    the stock fog is grey and flattens the scene."""
    comp = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    comp.set_editor_property("fog_density", 0.005)
    comp.set_editor_property("fog_height_falloff", 0.2)
    say("fog dialled down")


def add_post_process():
    ppv = actors().spawn_actor_from_class(
        unreal.PostProcessVolume, unreal.Vector(0.0, 0.0, 0.0))
    ppv.set_actor_label("PP_Environment")
    ppv.set_editor_property("unbound", True)

    # get_editor_property on a struct returns a COPY. Mutating it does nothing until it is
    # set back - the single most common way a Python post-process edit silently no-ops.
    s = ppv.get_editor_property("settings")

    s.set_editor_property("override_auto_exposure_min_brightness", True)
    s.set_editor_property("auto_exposure_min_brightness", EXPOSURE_EV100)
    s.set_editor_property("override_auto_exposure_max_brightness", True)
    s.set_editor_property("auto_exposure_max_brightness", EXPOSURE_EV100)

    s.set_editor_property("override_bloom_intensity", True)
    s.set_editor_property("bloom_intensity", 0.4)

    # Stylised means clean. A panning top-down camera plus motion blur is nausea.
    s.set_editor_property("override_motion_blur_amount", True)
    s.set_editor_property("motion_blur_amount", 0.0)
    s.set_editor_property("override_vignette_intensity", True)
    s.set_editor_property("vignette_intensity", 0.0)
    s.set_editor_property("override_film_grain_intensity", True)
    s.set_editor_property("film_grain_intensity", 0.0)

    # The blue shadow tint fakes sky bounce, and is what stops the olive greens reading
    # muddy. Saturation is a LIFT, not a push - section 1 wants the ground restrained.
    s.set_editor_property("override_color_saturation", True)
    s.set_editor_property("color_saturation", unreal.Vector4(1.05, 1.05, 1.05, 1.0))
    s.set_editor_property("override_color_saturation_shadows", True)
    s.set_editor_property("color_saturation_shadows", unreal.Vector4(1.0, 1.0, 1.08, 1.0))

    ppv.set_editor_property("settings", s)
    say("post process volume added, exposure locked at EV100 %.1f" % EXPOSURE_EV100)
    return ppv


def delete_floor():
    """The template floor is replaced by the Landscape in Slice B.

    SM_SkySphere is deliberately NOT deleted here. It may or may not be occluding the
    SkyAtmosphere, and that is a question for a screenshot, not for this script.
    """
    for a in actors().get_all_level_actors():
        if a.get_actor_label() == "Floor":
            actors().destroy_actor(a)
            say("deleted template floor")
            return
    say("no actor labelled 'Floor' - already gone")


def verify():
    """Reload from disk and read every value back. This is the test."""
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL)
    ok = True

    ppv = find_one(unreal.PostProcessVolume)
    if ppv is None:
        return False
    s = ppv.get_editor_property("settings")
    for prop, expected in (("auto_exposure_min_brightness", EXPOSURE_EV100),
                           ("auto_exposure_max_brightness", EXPOSURE_EV100),
                           ("motion_blur_amount", 0.0),
                           ("vignette_intensity", 0.0)):
        got = s.get_editor_property(prop)
        if abs(got - expected) > 1e-4:
            fail("%s is %r, expected %r" % (prop, got, expected))
            ok = False
        else:
            say("PASS %s = %r" % (prop, got))

    if not ppv.get_editor_property("unbound"):
        fail("post process volume is not unbound")
        ok = False
    else:
        say("PASS post process volume is unbound")

    sun = find_one(unreal.DirectionalLight)
    if sun is not None:
        comp = sun.get_component_by_class(unreal.DirectionalLightComponent)
        angle = comp.get_editor_property("light_source_angle")
        if abs(angle - SUN_SOURCE_ANGLE) > 1e-4:
            fail("light_source_angle is %r" % angle)
            ok = False
        else:
            say("PASS light_source_angle = %r" % angle)

    sky = find_one(unreal.SkyLight)
    if sky is not None:
        comp = sky.get_component_by_class(unreal.SkyLightComponent)
        if not comp.get_editor_property("real_time_capture"):
            fail("sky light real_time_capture is off")
            ok = False
        else:
            say("PASS sky light real_time_capture on")

    return ok


def run():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(LEVEL)
    say("loaded %s" % LEVEL)

    sun = find_one(unreal.DirectionalLight)
    sky = find_one(unreal.SkyLight)
    fog = find_one(unreal.ExponentialHeightFog)
    if sun is None or sky is None or fog is None:
        fail("level is not the expected template layout; nothing written")
        return

    set_sun(sun)
    set_sky_light(sky)
    set_fog(fog)
    add_post_process()
    delete_floor()

    levels.save_current_level()
    say("saved level")

    say("ALL VERIFIED" if verify() else "VERIFY FAILED")
    say("DONE")


run()
```

- [ ] **Step 2: Run it**

```bash
"D:/Epic/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "C:/repos/AirportMgr2/AirportMgr.uproject" -run=pythonscript \
  -script="C:/repos/AirportMgr2/Tools/Python/build_environment.py" \
  -unattended -nosplash -nopause >/dev/null 2>&1
grep -h "MARKER:" Saved/Logs/AirportMgr.log | tail -25
```

Expected: a `PASS` line for each of the six verified properties, then `MARKER: ALL VERIFIED` and `MARKER: DONE`. **Any `FAIL` line means stop and fix — do not proceed to screenshots.**

- [ ] **Step 3: Open the editor and take the three screenshots**

Spec §8 requires 20 m, 150 m and 600 m. With the editor open:

```bash
python Tools/Mcp.py shot env_20m.png
python Tools/Mcp.py shot env_150m.png
python Tools/Mcp.py shot env_600m.png
```

- [ ] **Step 4: Answer the two open questions from the images**

Put all three images in front of the user, and answer explicitly:

1. **Does `SM_SkySphere` occlude the SkyAtmosphere and clouds?** (spec §4.6) Toggle its visibility, re-shoot, compare. Delete it only if the image says so.
2. **Do shadows survive to the far edge at 600 m?** (spec §4.7) `DynamicShadowDistanceMovableLight` is 40,000 uu = 400 m, but VSM is on and uses clipmaps. If the far half of the frame has no shadows, raise it; if not, leave it and say so.

- [ ] **Step 5: Commit**

```bash
git add Tools/Python/build_environment.py Content/Maps/M_Starter.umap
git commit -m "feat(env): slice A - locked exposure, soft sun, real-time sky capture

Exposure locked at EV100 14 with min == max (Scene.h:1988), which is what
stops the frame re-brightening as the camera pans and makes a colour
judgeable at all. Units are EV100 because the project sets
ExtendDefaultLuminanceRange; 1.0 would blow out white.

LightSourceAngle 0.5357 -> 1.5 for a soft penumbra, SkyLight real-time
capture asserted on for the moving sun in Slice G, template floor deleted
ahead of the Landscape. SM_SkySphere left alone pending a screenshot.

Script applies then reloads and reads every value back, because a headless
level edit can report success and write nothing."
```

---

### Task 2: Slice B part 1 — the Landscape, by hand

**Files:**
- Create: `Tools/Python/verify_landscape.py`
- Modify: `Content/Maps/M_Starter.umap` (by hand, in the editor)

**Interfaces:**
- Consumes: Task 1's lit level.
- Produces: an `ALandscape` in `M_Starter`, 3,024 m square, flat, at Z=0.

**This task is done by the user, in the editor.** `ALandscapeProxy::Import` is editor-only C++ with no `UFUNCTION` (`LandscapeProxy.h:1418`, inside the `#if WITH_EDITOR` at 1326), so Python cannot create a Landscape. See spec §7.2.

- [ ] **Step 1: Create the Landscape in the editor**

Open `M_Starter`. Select **Landscape** from the mode dropdown (top-left of the viewport), then the **Manage** tab, **New** section, **Create New**. Set exactly:

| Field | Value |
|---|---|
| Section Size | `63 x 63 Quads` |
| Sections Per Component | `2 x 2 Sections` |
| Number of Components | `12 x 12` |
| Location | `0, 0, 0` |
| Rotation | `0, 0, 0` |
| Scale | `200, 200, 100` |

The panel should report **Overall Resolution 1513 x 1513** and **Total Components 144**. Click **Create**. Return to Select mode and save the level.

Z=0 is correct and does not z-fight: `RoadNetworkActor.h:819` puts `SurfaceZ` at 10 uu, so road plates sit 10 cm proud.

- [ ] **Step 2: Write the verification script**

Create `Tools/Python/verify_landscape.py`:

```python
"""Asserts the hand-made Landscape matches the spec. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

The Landscape cannot be created from Python (ALandscapeProxy::Import is editor-only C++
with no UFUNCTION), so it is made by hand - which means nothing but this script stands
between a mistyped component count and a plot that is silently the wrong size.
"""
import unreal

LEVEL = "/Game/Maps/M_Starter"

EXPECTED_COMPONENTS = 144
EXPECTED_SCALE_XY = 200.0
EXPECTED_METRES = 3024.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.Landscape)]
    if len(found) != 1:
        fail("expected exactly 1 Landscape, found %d" % len(found))
        say("DONE")
        return

    land = found[0]
    ok = True

    comps = land.get_components_by_class(unreal.LandscapeComponent)
    if len(comps) != EXPECTED_COMPONENTS:
        fail("component count is %d, expected %d - check Number of Components is 12 x 12 "
             "and Sections Per Component is 2 x 2" % (len(comps), EXPECTED_COMPONENTS))
        ok = False
    else:
        say("PASS component count = %d" % len(comps))

    scale = land.get_actor_scale3d()
    if abs(scale.x - EXPECTED_SCALE_XY) > 1e-3 or abs(scale.y - EXPECTED_SCALE_XY) > 1e-3:
        fail("scale is %r, expected X and Y of %r" % (scale, EXPECTED_SCALE_XY))
        ok = False
    else:
        say("PASS scale = %r" % scale)

    origin, extent = land.get_actor_bounds(False)
    metres_x = (extent.x * 2.0) / 100.0
    if abs(metres_x - EXPECTED_METRES) > 20.0:
        fail("extent is %.0f m across, expected about %.0f m" % (metres_x, EXPECTED_METRES))
        ok = False
    else:
        say("PASS extent = %.0f m across" % metres_x)

    loc = land.get_actor_location()
    if abs(loc.z) > 1e-3:
        fail("landscape Z is %r, expected 0 - roads sit at SurfaceZ 10 uu above it" % loc.z)
        ok = False
    else:
        say("PASS landscape Z = 0")

    say("ALL VERIFIED" if ok else "VERIFY FAILED")
    say("DONE")


run()
```

- [ ] **Step 3: Run it**

```bash
"D:/Epic/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "C:/repos/AirportMgr2/AirportMgr.uproject" -run=pythonscript \
  -script="C:/repos/AirportMgr2/Tools/Python/verify_landscape.py" \
  -unattended -nosplash -nopause >/dev/null 2>&1
grep -h "MARKER:" Saved/Logs/AirportMgr.log | tail -8
```

Expected: four `PASS` lines and `MARKER: ALL VERIFIED`. A component count of 36 means Sections Per Component was left at 1×1; a count of 576 means Number of Components was 24×24.

- [ ] **Step 4: Commit**

```bash
git add Tools/Python/verify_landscape.py Content/Maps/M_Starter.umap
git commit -m "feat(env): slice B - a 3,024 m flat landscape

144 components at 2 m per quad. Two sections per component rather than one,
so each component covers 4x the area for the same count - the coarser LOD
granularity is worth nothing on dead-flat ground.

Made by hand because ALandscapeProxy::Import is editor-only C++ with no
UFUNCTION, so Python cannot reach it. verify_landscape.py is therefore the
only thing standing between a mistyped component count and a plot that is
silently the wrong size."
```

---

### Task 3: Slice B part 2 — `M_Ground`

**Files:**
- Create: `Tools/Python/build_ground_material.py`
- Creates assets: `/Game/Environment/LI_GrassMown`, `LI_GrassRough`, `LI_Dirt`, `M_Ground`

**Interfaces:**
- Consumes: Task 2's Landscape.
- Produces: `M_Ground` assigned to the Landscape, with paint layers `GrassMown`, `GrassRough`, `Dirt`.

- [ ] **Step 1: Research the ground material technique first**

Per spec §11.0, craft is unverified here. Before writing the material, spend a short pass on **technique only, not look** — most published stylised-UE work targets a far more saturated Ghibli/Zelda result than spec §1 wants, and the concept sheet is the better reference for look.

Specifically find out:
- How macro colour variation is normally layered on a stylised landscape (noise octaves, scales, and how strong).
- Whether `LandscapeLayerCoords` or world-position-based UVs is the usual choice, and why.
- Whether flat colour with no normal map is actually viable at close range, or whether a very weak normal is standard.

Write what you find into the script's docstring as the justification for the numbers chosen. If the research contradicts the "two octaves of noise, no normal maps" starting point in spec §6.2, **follow the research and amend the spec**, saying why.

- [ ] **Step 2: Write the script**

Create `Tools/Python/build_ground_material.py`. The verified layer-blend recipe (spec §11.1) is:

```python
"""Authors the three landscape layer infos and M_Ground. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Colours are sampled from the concept sheet's own swatch panel (spec section 1.1), not
chosen by eye. The ground is DELIBERATELY restrained - about 50% saturation - because a
calm ground is what makes a yellow tug and a red-and-white windsock read as function at a
glance. Saturating the grass would put the field in competition with the gameplay objects.

NO NORMAL MAPS. At 6-600 m a grass normal map reads as mush; the stylised look comes from
colour, and surface detail comes from the grass clumps in Slice C.
"""
import unreal

OUT_DIR = "/Game/Environment"
MAT_NAME = "M_Ground"
LEVEL = "/Game/Maps/M_Starter"

# name -> (linear colour, noise strength)
# sRGB hex from spec 1.1, converted: #7D8E47, #748546, #C6B283.
LAYERS = [
    ("GrassMown",  unreal.LinearColor(0.213, 0.280, 0.070, 1.0)),
    ("GrassRough", unreal.LinearColor(0.180, 0.245, 0.068, 1.0)),
    ("Dirt",       unreal.LinearColor(0.573, 0.448, 0.226, 1.0)),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def make_layer_infos():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    infos = {}
    for name, _ in LAYERS:
        path = "%s/LI_%s" % (OUT_DIR, name)
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            info = unreal.EditorAssetLibrary.load_asset(path)
        else:
            info = tools.create_asset(
                "LI_%s" % name, OUT_DIR, unreal.LandscapeLayerInfoObject, None)
        info.set_editor_property("layer_name", name)
        unreal.EditorAssetLibrary.save_asset(info.get_path_name(), only_if_is_dirty=False)
        infos[name] = info
        say("layer info %s" % path)
    return infos


def build_material():
    lib = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (OUT_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = tools.create_asset(MAT_NAME, OUT_DIR, unreal.Material, unreal.MaterialFactoryNew())

    blend = lib.create_material_expression(
        mat, unreal.MaterialExpressionLandscapeLayerBlend, -400, 0)

    entries = []
    for name, _ in LAYERS:
        item = unreal.LayerBlendInput()
        item.set_editor_property("layer_name", name)
        item.set_editor_property("blend_type", unreal.LandscapeLayerBlendType.LB_WEIGHT_BLEND)
        item.set_editor_property("preview_weight", 1.0 if name == "GrassMown" else 0.0)
        entries.append(item)
    blend.set_editor_property("layers", entries)

    # The per-layer pin is named "Layer <LayerName>" - NOT the bare name, not an index.
    # Verified by spike, spec section 11.1.
    for index, (name, colour) in enumerate(LAYERS):
        node = lib.create_material_expression(
            mat, unreal.MaterialExpressionConstant3Vector, -800, -200 + index * 200)
        node.set_editor_property("constant", colour)
        if not lib.connect_material_expressions(node, "", blend, "Layer %s" % name):
            fail("could not wire layer %s" % name)

    lib.connect_material_property(blend, "", unreal.MaterialProperty.MP_BASE_COLOR)

    rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -800, 500)
    rough.set_editor_property("r", 0.9)
    lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("built %s" % path)
    return mat


def assign(mat):
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.Landscape)]
    if len(found) != 1:
        fail("expected exactly 1 Landscape, found %d" % len(found))
        return
    found[0].set_editor_property("landscape_material", mat)
    levels.save_current_level()
    say("assigned M_Ground to the landscape")


def verify():
    """connect_material_expressions returning True is NOT evidence the wiring survived a
    save. layer_input is UPROPERTY without EditAnywhere so reflection cannot read it -
    export_text() on the layer struct is the only readback. Spec section 11.1."""
    mat = unreal.EditorAssetLibrary.load_asset("%s/%s" % (OUT_DIR, MAT_NAME))
    ok = False
    for expr in unreal.MaterialEditingLibrary.get_material_expressions(mat):
        if isinstance(expr, unreal.MaterialExpressionLandscapeLayerBlend):
            for item in expr.get_editor_property("layers"):
                text = item.export_text()
                name = item.get_editor_property("layer_name")
                if "Expression=None" in text.split("HeightInput")[0]:
                    fail("layer %s has no wired input" % name)
                else:
                    say("PASS layer %s is wired" % name)
                    ok = True
    return ok


def run():
    make_layer_infos()
    mat = build_material()
    assign(mat)
    say("ALL VERIFIED" if verify() else "VERIFY FAILED")
    say("DONE")


run()
```

- [ ] **Step 3: Run it and check the readback**

```bash
"D:/Epic/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" \
  "C:/repos/AirportMgr2/AirportMgr.uproject" -run=pythonscript \
  -script="C:/repos/AirportMgr2/Tools/Python/build_ground_material.py" \
  -unattended -nosplash -nopause >/dev/null 2>&1
grep -h "MARKER:" Saved/Logs/AirportMgr.log | tail -15
```

Expected: `PASS layer GrassMown is wired`, same for `GrassRough` and `Dirt`, then `ALL VERIFIED`.

- [ ] **Step 4: Paint a base coverage and re-screenshot**

Open the editor. In Landscape mode → Paint, the three layers now appear. Assign each target layer its `LI_*` asset (right-click the layer → the layer info slot), then fill the whole landscape with `GrassMown`. Save.

Re-shoot at 20 m, 150 m and 600 m and compare against the Task 1 images.

- [ ] **Step 5: Commit**

```bash
git add Tools/Python/build_ground_material.py Content/Environment Content/Maps/M_Starter.umap
git commit -m "feat(env): slice B - M_Ground, three stylised layers

Colours sampled from the concept sheet's swatch panel, about 50% saturation
by design: a calm ground is what makes the vehicles and markings read as
function. No normal maps - at 6-600 m they are mush, and Slice C's clumps
are where surface detail belongs.

Per-layer pins are named 'Layer <LayerName>'. Readback is export_text(),
because layer_input is UPROPERTY without EditAnywhere and reflection cannot
see it - so a True from connect_material_expressions proves nothing."
```

---

### Task 4: Slice G part 1 — `FSunPath`

**Files:**
- Create: `Source/AirportMgr/SunPath.h`, `Source/AirportMgr/SunPath.cpp`
- Test: `Source/AirportMgr/SunPathTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `struct FSunLighting { FRotator Rotation; float TemperatureKelvin; float Intensity; };`
  - `struct FSunPath` with public members `MaxElevationDegrees`, `MinElevationDegrees`, `NoonAzimuthDegrees`, `NoonTemperatureKelvin`, `DuskTemperatureKelvin`, `NoonIntensity`, `DuskIntensityFraction`, and `FSunLighting At(double DayFraction) const;`

**The editor must be CLOSED for this task.**

- [ ] **Step 1: Write the failing tests**

Create `Source/AirportMgr/SunPathTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SunPath.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named under "Airside." although the path lives in the game module, for the same reason
 * BuildCameraRigTest gives: Run-AirsideTests.ps1 filters on that prefix, and a test the
 * pre-commit run does not pick up is one found failing by the next person to touch it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathNoonTest,
	"Airside.Sky.SunPath.NoonIsThePeak",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathNoonTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// Noon is the peak BY CONSTRUCTION: spec 4.2's hand-picked -42 degrees is not a magic
	// number any more, it is the top of this curve. If this drifts, the sun angle the
	// whole art direction was judged against has silently moved.
	const FSunLighting Noon = Path.At(0.5);
	TestEqual(TEXT("noon pitch is minus MaxElevation"), Noon.Rotation.Pitch, -Path.MaxElevationDegrees, 1e-6);
	TestEqual(TEXT("noon temperature is the noon value"), Noon.TemperatureKelvin, Path.NoonTemperatureKelvin, 1e-3f);
	TestEqual(TEXT("noon intensity is the noon value"), Noon.Intensity, Path.NoonIntensity, 1e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathFloorTest,
	"Airside.Sky.SunPath.NeverBelowTheFloor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathFloorTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// THE WHOLE POINT OF THE SLICE. A literal sun would put 40% of play time in the dark
	// with no runway lights and exposure deliberately locked, which is unreadable rather
	// than atmospheric. Sampling every 1/64 of a day catches a floor that holds at the
	// sampled hours and fails between them.
	for (int32 Step = 0; Step < 64; ++Step)
	{
		const double Fraction = Step / 64.0;
		const FSunLighting At = Path.At(Fraction);
		const double Elevation = -At.Rotation.Pitch;
		TestTrue(
			*FString::Printf(TEXT("elevation %.3f at fraction %.4f is at or above the floor %.3f"),
				Elevation, Fraction, Path.MinElevationDegrees),
			Elevation >= Path.MinElevationDegrees - 1e-6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathMidnightContinuityTest,
	"Airside.Sky.SunPath.AzimuthIsContinuousAcrossMidnight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathMidnightContinuityTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// A jump here would read as the sun teleporting once per game day - at x8 that is every
	// 2.5 real minutes, so it would be seen. Compared on the normalised delta because the
	// two yaws are 360 degrees apart in raw value and identical as directions.
	const FSunLighting Before = Path.At(0.9999);
	const FSunLighting After = Path.At(0.0);
	const double Delta = FMath::Abs(FRotator::NormalizeAxis(After.Rotation.Yaw - Before.Rotation.Yaw));
	TestTrue(*FString::Printf(TEXT("yaw step across midnight is %.4f degrees, expected under 1"), Delta),
		Delta < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathDawnRisesTest,
	"Airside.Sky.SunPath.ElevationRisesFromDawnToNoon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathDawnRisesTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// Monotonic, not merely higher at the ends: a curve that dipped in the middle of the
	// morning would look like weather rather than time passing.
	double Previous = -Path.At(0.25).Rotation.Pitch;
	for (int32 Step = 1; Step <= 16; ++Step)
	{
		const double Fraction = 0.25 + (0.25 * Step) / 16.0;
		const double Elevation = -Path.At(Fraction).Rotation.Pitch;
		TestTrue(*FString::Printf(TEXT("elevation at %.4f (%.3f) is at or above the previous (%.3f)"),
			Fraction, Elevation, Previous), Elevation >= Previous - 1e-6);
		Previous = Elevation;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunPathDuskWarmsTest,
	"Airside.Sky.SunPath.DuskIsWarmerAndDimmer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunPathDuskWarmsTest::RunTest(const FString& Parameters)
{
	const FSunPath Path;

	// Warmth is most of what sells time of day; dimness is what the locked exposure then
	// shows honestly rather than compensating away.
	const FSunLighting Midnight = Path.At(0.0);
	TestEqual(TEXT("midnight temperature is the dusk value"), Midnight.TemperatureKelvin, Path.DuskTemperatureKelvin, 1e-3f);
	TestEqual(TEXT("midnight intensity is the dusk fraction of noon"), Midnight.Intensity,
		Path.NoonIntensity * Path.DuskIntensityFraction, 1e-3f);
	TestTrue(TEXT("dusk is dimmer than noon"), Midnight.Intensity < Path.At(0.5).Intensity);
	return true;
}

#endif
```

- [ ] **Step 2: Build and run to verify they fail**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

Expected: **build FAILS** with `Cannot open include file: 'SunPath.h'`. That is the failing test.

- [ ] **Step 3: Write the header**

Create `Source/AirportMgr/SunPath.h`:

```cpp
#pragma once

#include "CoreMinimal.h"

/** Everything the sun driver sets on the directional light for one instant. */
struct FSunLighting
{
	FRotator Rotation = FRotator::ZeroRotator;

	float TemperatureKelvin = 5800.0f;

	/** Lux, matching ULightComponent::Intensity for a directional light. */
	float Intensity = 10.0f;
};

/**
 * Maps a time of day to where the sun is and what colour it is.
 *
 * A PLAIN STRUCT - no UObject, no world, no knowledge of lights, actors or clocks. Same
 * shape and same reason as FBuildCameraRig: it computes values from numbers, which is what
 * makes the curve unit-testable with no level. ASunDriver holds the tunables as UPROPERTYs
 * and copies them on, exactly as ARoadBuildController::ApplyViewLimits does for the rig.
 *
 * THE ARC IS FLOORED ABOVE THE HORIZON, and that is the whole design rather than a
 * simplification. RealSecondsPerGameDay is 1200, so a literal sun sweeps 18 degrees a
 * minute at x1 and 144 at x8, and roughly 40% of a day is dark. With no runway lights, no
 * apron floods and exposure deliberately locked, that dark is unreadable rather than
 * atmospheric - a defect, not a mood. The concept sheet agrees: its lighting mast is
 * captioned "For when you operate later", so night operations are a progression unlock.
 * When those lights exist, the floor lifts and night becomes the unlock.
 *
 * Between 18:00 and 06:00 the sun therefore sits at the floor while its azimuth carries on
 * round to the north, which reads as a long high-latitude twilight - a real thing the sky
 * does, not a glitch.
 */
struct FSunPath
{
	/** Elevation at noon. Spec 4.2's hand-picked angle, now the peak of a curve. */
	double MaxElevationDegrees = 42.0;

	/** The dusk floor: the sun never goes below this. See the class comment. */
	double MinElevationDegrees = 8.0;

	/** Compass bearing the sun sits at when it is at MaxElevationDegrees. */
	double NoonAzimuthDegrees = 150.0;

	float NoonTemperatureKelvin = 5800.0f;

	/** Warmer at the floor. Warmth is most of what sells time of day. */
	float DuskTemperatureKelvin = 3200.0f;

	/** Lux at noon. The engine's own directional default is 10. */
	float NoonIntensity = 10.0f;

	/**
	 * Floor intensity as a fraction of noon.
	 *
	 * With exposure locked to a daylight EV this is what decides whether dusk is playable,
	 * so it is the figure most likely to move once there is a screenshot to judge.
	 */
	float DuskIntensityFraction = 0.35f;

	/**
	 * The sun at DayFraction, where 0 is midnight and 0.5 is noon.
	 *
	 * Values outside [0, 1) are wrapped, so a caller may hand over a raw clock reading
	 * without first reducing it.
	 */
	FSunLighting At(double DayFraction) const;
};
```

- [ ] **Step 4: Write the implementation**

Create `Source/AirportMgr/SunPath.cpp`:

```cpp
#include "SunPath.h"

FSunLighting FSunPath::At(double DayFraction) const
{
	// Wrap rather than clamp: a clock reading is unbounded and a caller reducing it first
	// is a step that can be forgotten. FMath::Fmod keeps the sign of its argument, so a
	// negative fraction needs the extra turn.
	double Fraction = FMath::Fmod(DayFraction, 1.0);
	if (Fraction < 0.0)
	{
		Fraction += 1.0;
	}

	// T is -1 at 06:00, 0 at noon, +1 at 18:00. Cosine of a quarter turn gives 1 at noon
	// falling to 0 at each end, so the elevation reaches MaxElevationDegrees exactly once
	// and meets the floor smoothly rather than stepping onto it.
	constexpr double Noon = 0.5;
	constexpr double QuarterDay = 0.25;
	const double T = (Fraction - Noon) / QuarterDay;

	double Elevation = MinElevationDegrees;
	if (FMath::Abs(T) < 1.0)
	{
		const double Shape = FMath::Cos(T * UE_DOUBLE_HALF_PI);
		Elevation = FMath::Lerp(MinElevationDegrees, MaxElevationDegrees, Shape);
	}

	// Alpha is 0 at the floor and 1 at noon, and drives colour and brightness together so
	// the two can never disagree about what time it is. Guarded because a path configured
	// with Min == Max is a legitimate "fixed sun" and must not divide by zero.
	const double Span = MaxElevationDegrees - MinElevationDegrees;
	const double Alpha = FMath::IsNearlyZero(Span)
		? 1.0
		: FMath::Clamp((Elevation - MinElevationDegrees) / Span, 0.0, 1.0);

	// A FULL 360 over the day, deliberately not reversed at dusk. The sun carrying on to
	// the north through the night is what makes the azimuth continuous across midnight;
	// turning it back would put a visible kink in the shadow direction twice a day.
	const double Azimuth = NoonAzimuthDegrees + 360.0 * (Fraction - Noon);

	FSunLighting Out;
	// Negative pitch points the light down: elevation above the horizon, not below it.
	Out.Rotation = FRotator(-Elevation, Azimuth, 0.0);
	Out.TemperatureKelvin = FMath::Lerp(DuskTemperatureKelvin, NoonTemperatureKelvin, static_cast<float>(Alpha));
	Out.Intensity = FMath::Lerp(NoonIntensity * DuskIntensityFraction, NoonIntensity, static_cast<float>(Alpha));
	return Out;
}
```

- [ ] **Step 5: Build TWICE**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

**Twice is not superstition.** A new test `.cpp` is not compiled on the first build, which still reports `Result: Succeeded`. The second build picks it up.

- [ ] **Step 6: Run the tests**

```powershell
./Tools/Run-AirsideTests.ps1 -Filter Airside.Sky
```

Expected: **`5 test(s) run, 0 failed, 0 crashed`**. Read that line — the exit code is not trustworthy here, and a count below 5 means the test file was not compiled in, so build again.

- [ ] **Step 7: Commit**

```bash
git add Source/AirportMgr/SunPath.h Source/AirportMgr/SunPath.cpp Source/AirportMgr/SunPathTest.cpp
git commit -m "feat(env): FSunPath - time of day as a world-free curve

A plain struct on the FBuildCameraRig pattern: it turns a day fraction into
a rotation, a temperature and an intensity and knows nothing about lights,
actors or clocks, which is what makes the curve testable with no level.

The arc is floored above the horizon by design. 1200 real seconds per game
day means a literal sun sweeps 144 deg/min at x8 and leaves 40% of play time
dark, with no runway lights and exposure deliberately locked - unreadable,
not atmospheric. The concept sheet's lighting mast says night is an unlock.

Five tests: noon is the peak, the floor holds at every 1/64 of a day, the
azimuth is continuous across midnight, dawn rises monotonically, and dusk is
warmer and dimmer."
```

---

### Task 5: Slice G part 2 — `ASunDriver`

**Files:**
- Create: `Source/AirportMgr/SunDriver.h`, `Source/AirportMgr/SunDriver.cpp`
- Test: `Source/AirportMgr/SunDriverTest.cpp`
- Modify: `Content/Maps/M_Starter.umap` (place the actor by hand)

**Interfaces:**
- Consumes: `FSunPath`, `FSunLighting` from Task 4; `USimClock` from the `AirportOps` plugin (already a public dependency in `AirportMgr.Build.cs`).
- Produces: `ASunDriver` with `static double ResolveDayFraction(const USimClock* Clock)` and `void ApplyToSun()`.

**The editor must be CLOSED.** `ASunDriver` is a new `UCLASS` with new `UPROPERTY`s, so Live Coding cannot patch it in.

- [ ] **Step 1: Write the failing test**

Create `Source/AirportMgr/SunDriverTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "SunDriver.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SEAM TEST the refactor contract asks for: it fails if the driver is spawned but
 * never wired to a clock. USimClock is world-free by design ("built with NewObject and
 * advanced by hand in tests"), so this needs no level.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunDriverNoClockIsNoonTest,
	"Airside.Sky.SunDriver.NoClockMeansNoon",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunDriverNoClockIsNoonTest::RunTest(const FString& Parameters)
{
	// The editor viewport and the first PIE frame both have no time of day to read. Noon
	// is the right answer there because it is the angle the whole art direction was judged
	// against - falling back to midnight would make the editor look broken.
	TestEqual(TEXT("no clock resolves to noon"), ASunDriver::ResolveDayFraction(nullptr), 0.5, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSunDriverReadsClockTest,
	"Airside.Sky.SunDriver.ReadsTheGameClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FSunDriverReadsClockTest::RunTest(const FString& Parameters)
{
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());

	// A game day of 86,400 real seconds makes TimeScale() exactly 1, so Advance() moves
	// game time one-for-one and the numbers below read as the times they are. There is no
	// SetGameSeconds accessor and this needs none.
	Clock->RealSecondsPerGameDay = USimClock::SecondsPerDay;

	TestEqual(TEXT("a fresh clock is midnight"), ASunDriver::ResolveDayFraction(Clock), 0.0, 1e-9);

	const double Quarter = USimClock::SecondsPerDay * 0.25;
	Clock->Advance(Quarter);
	TestEqual(TEXT("06:00 is a quarter of a day"), ASunDriver::ResolveDayFraction(Clock), 0.25, 1e-9);

	Clock->Advance(Quarter);
	TestEqual(TEXT("12:00 is half a day"), ASunDriver::ResolveDayFraction(Clock), 0.5, 1e-9);

	// The one that matters: a day boundary must wrap to 0, not keep counting, or the sun
	// climbs out of the sky on day two.
	Clock->Advance(USimClock::SecondsPerDay);
	TestEqual(TEXT("12:00 on day two wraps to half"), ASunDriver::ResolveDayFraction(Clock), 0.5, 1e-9);
	return true;
}

#endif
```

- [ ] **Step 2: Build and run to verify it fails**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
```

Expected: **build FAILS** with `Cannot open include file: 'SunDriver.h'`.

- [ ] **Step 3: Write the header**

Create `Source/AirportMgr/SunDriver.h`:

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SunPath.h"
#include "SunDriver.generated.h"

class ADirectionalLight;
class USimClock;

/**
 * Points the level's sun at wherever the game clock says the sun should be.
 *
 * A FORWARDER, deliberately. All the reasoning lives in FSunPath, which is world-free and
 * tested; this class reads a clock, calls it, and sets three values on a light. Putting
 * the curve here instead would make it reachable only by spawning an actor in a level.
 *
 * THE LIGHT IS AN EXPLICIT LEVEL-AUTHORED POINTER, not a TActorIterator<ADirectionalLight>
 * search. A search silently picks one of two lights and the wrong choice looks like the
 * feature not working; this project also puts knobs in properties on purpose.
 *
 * The tunables are UPROPERTYs copied onto a plain FSunPath rather than a USTRUCT member,
 * which is exactly what ARoadBuildController does with FBuildCameraRig through
 * ApplyViewLimits - details-panel edits take effect live, and the struct stays free of UHT.
 */
UCLASS()
class AIRPORTMGR_API ASunDriver : public AActor
{
	GENERATED_BODY()

public:
	ASunDriver();

	/** The level's sun. Nothing happens if this is empty, and a warning says so once. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	TObjectPtr<ADirectionalLight> Sun;

	/** Elevation at noon, degrees above the horizon. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "1.0", ClampMax = "89.0"))
	double MaxElevationDegrees = 42.0;

	/** The dusk floor. The sun never drops below this - see FSunPath for why. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	double MinElevationDegrees = 8.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	double NoonAzimuthDegrees = 150.0;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float NoonTemperatureKelvin = 5800.0f;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float DuskTemperatureKelvin = 3200.0f;

	UPROPERTY(EditAnywhere, Category = "Airside|Sky")
	float NoonIntensity = 10.0f;

	/** Floor brightness as a fraction of noon. Decides whether dusk is playable. */
	UPROPERTY(EditAnywhere, Category = "Airside|Sky", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DuskIntensityFraction = 0.35f;

	/**
	 * Where in the day this clock is, as a fraction in [0, 1). Noon when Clock is null.
	 *
	 * Static and clock-in-hand so the fallback is testable without a world: the editor
	 * viewport and the first PIE frame both have no clock, and noon is the angle the art
	 * direction was judged against.
	 */
	static double ResolveDayFraction(const USimClock* Clock);

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	/** Copy the tunables above onto a path, so details-panel edits take effect live. */
	FSunPath MakePath() const;

	/** Read the clock, evaluate the path, set the light. */
	void ApplyToSun();

private:
	/** So a missing Sun warns once rather than every frame. */
	bool bWarnedAboutMissingSun = false;
};
```

- [ ] **Step 4: Write the implementation**

Create `Source/AirportMgr/SunDriver.cpp`:

```cpp
#include "SunDriver.h"

#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogSunDriver, Log, All);

ASunDriver::ASunDriver()
{
	PrimaryActorTick.bCanEverTick = true;
}

double ASunDriver::ResolveDayFraction(const USimClock* Clock)
{
	if (Clock == nullptr)
	{
		return 0.5;
	}

	// TimeOfDay() is the clock's OWN answer to "where in the day are we", and it already
	// floors correctly across a day boundary (SimClock.cpp:9-12). Re-deriving it here with
	// an Fmod would be a second implementation of the day wrap, and the two would drift.
	return Clock->TimeOfDay() / USimClock::SecondsPerDay;
}

FSunPath ASunDriver::MakePath() const
{
	FSunPath Path;
	Path.MaxElevationDegrees = MaxElevationDegrees;
	Path.MinElevationDegrees = MinElevationDegrees;
	Path.NoonAzimuthDegrees = NoonAzimuthDegrees;
	Path.NoonTemperatureKelvin = NoonTemperatureKelvin;
	Path.DuskTemperatureKelvin = DuskTemperatureKelvin;
	Path.NoonIntensity = NoonIntensity;
	Path.DuskIntensityFraction = DuskIntensityFraction;
	return Path;
}

void ASunDriver::ApplyToSun()
{
	if (Sun == nullptr)
	{
		if (!bWarnedAboutMissingSun)
		{
			bWarnedAboutMissingSun = true;
			UE_LOG(LogSunDriver, Warning,
				TEXT("ASunDriver '%s' has no Sun set, so the day cycle does nothing. "
				     "Point it at the level's DirectionalLight in the Details panel."),
				*GetName());
		}
		return;
	}

	UDirectionalLightComponent* Component = Sun->FindComponentByClass<UDirectionalLightComponent>();
	if (Component == nullptr)
	{
		return;
	}

	// UOpsRuntimeSubsystem::Get is built for exactly this: its own comment says "editor
	// worlds have no game instance" and it returns null there rather than making every
	// caller unpick the chain. ResolveDayFraction turns that null into noon.
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	const USimClock* Clock = Runtime != nullptr ? Runtime->GetClock() : nullptr;

	const FSunLighting Lighting = MakePath().At(ResolveDayFraction(Clock));
	Sun->SetActorRotation(Lighting.Rotation);
	Component->SetTemperature(Lighting.TemperatureKelvin);
	Component->SetIntensity(Lighting.Intensity);
}

void ASunDriver::BeginPlay()
{
	Super::BeginPlay();

	// Once up front so the first frame is already at the right time of day, rather than
	// snapping to it after a tick.
	ApplyToSun();
}

void ASunDriver::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ApplyToSun();
}
```

- [ ] **Step 5: Build TWICE and run the tests**

```powershell
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development `
  -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
./Tools/Check-Architecture.ps1
./Tools/Run-AirsideTests.ps1 -Filter Airside.Sky
```

Expected: `Check-Architecture: PASS`, then **`7 test(s) run, 0 failed, 0 crashed`** — five from Task 4 plus two here. A count of 5 means `SunDriverTest.cpp` was not compiled in.

- [ ] **Step 6: Place the actor and verify in PIE**

Open the editor, drag an `ASunDriver` into `M_Starter`, set its `Sun` property to the level's `DirectionalLight`, and save.

Run PIE. Watch the shadows rotate. Take screenshots at two clearly different times of day and confirm:

1. **Does dusk stay playable** with exposure locked (spec §5)? If the field goes unreadably dark, raise `DuskIntensityFraction` — that is what it is for.
2. **Does Lumen shimmer or ghost** at ×8, with the sun sweeping 144°/min and the SkyLight recapturing behind it (spec §8)? This was flagged as Lumen's known weakness when the stack was chosen. **Screenshot at ×8; do not reason about it.**

- [ ] **Step 7: Commit**

```bash
git add Source/AirportMgr/SunDriver.h Source/AirportMgr/SunDriver.cpp \
        Source/AirportMgr/SunDriverTest.cpp Content/Maps/M_Starter.umap
git commit -m "feat(env): ASunDriver - the sun follows the game clock

A forwarder: all the reasoning is in FSunPath, and this reads a clock, calls
it and sets three values on a light. The light is an explicit level-authored
pointer, not a TActorIterator search - a search silently picks one of two
lights and the wrong choice looks like the feature not working.

Tunables are UPROPERTYs copied onto a plain FSunPath, the same shape
ARoadBuildController uses for FBuildCameraRig via ApplyViewLimits, so
details-panel edits take effect live and the struct stays clear of UHT.

Two tests, the first of them the seam: no clock resolves to noon, which is
what the editor viewport and the first PIE frame both need."
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §1 Direction, §1.1 Palette | 3 (colours), constraints block |
| §2.3 Landscape parameters | 2 |
| §4.1 PostProcessVolume | 1 |
| §4.2 DirectionalLight | 1 |
| §4.3 SkyLight | 1 |
| §4.4 ExponentialHeightFog | 1 |
| §4.5 VolumetricCloud (keep, observe) | 1 step 4 |
| §4.6 Deletions | 1 (floor deleted; SkySphere decided from the image) |
| §4.7 Shadow distance (verify, don't change) | 1 step 4 |
| §5 Slice G | 4 and 5 |
| §6.1 Layer infos, §6.2 M_Ground | 3 |
| §7.1 Who does what | 2 by hand, rest scripted |
| §8 Verification screenshots | 1 step 3-4, 3 step 4, 5 step 6 |
| §11.0 Research before B and C | 3 step 1 |
| §11.1 Layer blend recipe | 3 step 2 |

**Not covered, and deliberately** — spec §10 out of scope: Slice C (grass), Slice D (surround), Slice E (field-length compression), Slice F (camera range). Each needs its own plan. Slice E in particular is gameplay code with a test contract, and nothing here depends on it.

**Two APIs were verified during self-review rather than left as "go and check":**

- `ASunDriver::ResolveDayFraction` uses `USimClock::TimeOfDay()` (`SimClock.cpp:9-12`), which already floors correctly across a day boundary. An `Fmod` here would have been a second implementation of the day wrap, and the two would drift.
- `ApplyToSun` uses the static `UOpsRuntimeSubsystem::Get(const UWorld*)`, whose own comment says *"editor worlds have no game instance"* — it returns null there, which is exactly the case `ResolveDayFraction` turns into noon.
- `USimClock` has no `SetGameSeconds` accessor and needs none: `RealSecondsPerGameDay` is a public `UPROPERTY`, so setting it to `SecondsPerDay` makes `TimeScale()` exactly 1 and `Advance()` moves game time one-for-one.

**Type consistency checked:** `FSunPath`'s seven members are named identically in `SunPath.h`, in `ASunDriver`'s `UPROPERTY`s, in `MakePath()` and in every test. `FSunLighting`'s three fields likewise.
