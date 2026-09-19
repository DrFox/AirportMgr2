"""Authors M_TyreSmoke and points DA_AirsideContent at it. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

The puff of tyre smoke under the main wheels at touchdown - the project's FIRST effect, so
the shape of this file is the precedent for any that follow.

NOT NIAGARA, AND THAT WAS A DECISION RATHER THAN AN OVERSIGHT. Niagara is the engine's
answer and would be the right one the moment a second effect exists. It has one cost that
decided it here: a Niagara system is authored by hand in the editor and cannot practically
be built from a script, so it would have been the ONLY content in this project that cannot
be rebuilt from Tools/Python. Everything else - every material, every data asset, every
runway profile - resolves through a build_*.py. For two puffs on one event, a pooled sphere
with an animated material is less machinery than a Niagara system plus its parameter
plumbing, and it keeps the pipeline whole.

Stated plainly so the trade is re-checkable: this does NOT scale. Jet exhaust, prop wash,
wing vortices, rain or engine smoke are Niagara's job, and the day one of those is wanted
this file is the thing to delete rather than extend.

A SPHERE, NOT A CAMERA-FACING SPRITE, and that is the art direction talking. The direction
is a "living airport diorama" - an architectural model come alive - and a model-maker's puff
of smoke is a blob of cotton wool, not a wisp. A chunky sphere eaten away by noise reads as
made rather than simulated, which is the whole point. It also sidesteps a real problem: the
build camera now sits nearly horizontal at full zoom, and billboards seen edge-on from a
near-ground camera are where sprite effects look worst.

THE NOISE IS THE SAME RECIPE AS THE GRASS AND THE PAVEMENT. airside_matnodes' gradient noise
through a narrow contrast window is what gave the field regions with defined edges instead
of blur; here it does the same job on a silhouette, eating the sphere into a cauliflower
rather than fading it out like an airbrush. Reusing it is not thrift - it is what makes the
effect look like it belongs to this game rather than to a plugin.

DRIVEN BY AN AGE PARAMETER, 0 at birth and 1 at death, set per puff per frame by the C++
component. Everything the puff does over its life is a function of that one number, so the
material has no state and two puffs at different ages are the same material with different
parameters. Growth is in the component (it scales the component), not here: scaling a
transform is free and a vertex shader is not.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airside_matnodes as nodes

MAT_DIR = "/Game/Materials"
MAT_NAME = "M_TyreSmoke"
CONTENT = "/Game/DA_AirsideContent"

# Blue-grey, and DESATURATED. Burnt rubber smoke is blue-grey rather than white, and a white
# puff on grey pavement would read as steam. Pale enough to show against tarmac at the build
# camera, nowhere near bright enough to compete with the white markings it sits among.
SMOKE_COLOUR = "#B9BEC2"

# Peak opacity, at the age the puff is thickest. Well under 1: this is smoke over pavement,
# and a puff that hid the runway markings under it would be worse than no puff.
SMOKE_OPACITY = 0.42

# WHERE in its life the puff is thickest, as a fraction of Age. Not 0: real smoke takes a
# moment to billow, and a puff that is at full strength on the frame it is born reads as a
# pop rather than a puff.
PEAK_AT = 0.18

# The noise that eats the silhouette. Metres of WORLD space, so two puffs side by side under
# the two main wheels are eaten differently rather than being twins.
BLOB_SIZE = 1.1
BLOB_LEVELS = 3
# A NARROW window, unlike the pavement's. This is the setting that decides chunky-versus-
# wispy: widen it and the sphere fades out like an airbrush, narrow it and it breaks into
# defined lumps. Chunky is the brief.
BLOB_LO = 0.42
BLOB_HI = 0.58

# How much of the sphere the noise is allowed to eat at the END of the puff's life. The puff
# dies by being eaten away rather than by fading uniformly - a uniform fade is what every
# stock smoke does and it is the thing that reads as a filter.
ERODE_AT_DEATH = 0.85

# HOW HARD THE EROSION CUTS, and the first version of this file got it wrong in a way worth
# keeping. The erosion was saturate(blob - threshold), which is a RAMP: at mid-life it
# averages about 0.12, and multiplied by SmokeOpacity it left the puff roughly 4% opaque -
# invisible on screen while every log line said it was there.
#
# Erosion is meant to CUT, not to dim. Multiplying the difference by a sharpness before the
# saturate makes it nearly binary: a lump is either there at full strength or eaten. The
# envelope is what fades the puff; this decides its shape, and the two were fighting.
ERODE_SHARPNESS = 8.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def build_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, MAT_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        material = unreal.EditorAssetLibrary.load_asset(path)
        nodes.clear_graph(lib, material, fail)
        say("rebuilding %s in place" % path)
    else:
        material = tools.create_asset(
            MAT_NAME, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        fail("no material at %s" % path)
        return None

    # Translucent and UNLIT, for the same reasons the rubber is. Unlit because a puff has no
    # surface worth lighting: giving it one would have it catch a specular highlight, and a
    # shiny puff of smoke is a plastic one. Two-sided because the camera goes inside it - the
    # aircraft drives through its own smoke at 40 m/s.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)

    colour = nodes.vector(lib, material, "SmokeColour", SMOKE_COLOUR, -700, -400)
    lib.connect_material_property(colour, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # --- Age: 0 at birth, 1 at death -----------------------------------------------------
    age = nodes.scalar(lib, material, "Age", 0.0, -2800, 200)

    # --- Envelope: rise to PeakAt, fall to nothing ----------------------------------------
    #
    # Two smoothsteps rather than one curve: the rise and the fall want different lengths and
    # a single ease cannot give them. Multiplying them makes the shape obvious to read and
    # each half tunable on its own.
    peak = nodes.scalar(lib, material, "PeakAt", PEAK_AT, -2800, 300)
    rise = lib.create_material_expression(material, unreal.MaterialExpressionSmoothStep, -2400, 200)
    rise.set_editor_property("const_min", 0.0)
    lib.connect_material_expressions(peak, "", rise, "Max")
    lib.connect_material_expressions(age, "", rise, "Value")

    # 1 - smoothstep(PeakAt, 1, Age): full until the peak, gone by death.
    fall_step = lib.create_material_expression(material, unreal.MaterialExpressionSmoothStep, -2400, 420)
    lib.connect_material_expressions(peak, "", fall_step, "Min")
    fall_step.set_editor_property("const_max", 1.0)
    lib.connect_material_expressions(age, "", fall_step, "Value")
    fall = lib.create_material_expression(material, unreal.MaterialExpressionOneMinus, -2150, 420)
    lib.connect_material_expressions(fall_step, "", fall, "")

    envelope = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -1900, 300)
    lib.connect_material_expressions(rise, "", envelope, "A")
    lib.connect_material_expressions(fall, "", envelope, "B")

    # --- The blobbing, off WORLD position -------------------------------------------------
    blob_size = nodes.scalar(lib, material, "BlobSize", BLOB_SIZE, -2800, 800)
    blob_lo = nodes.scalar(lib, material, "BlobLo", BLOB_LO, -2100, 740)
    blob_hi = nodes.scalar(lib, material, "BlobHi", BLOB_HI, -2100, 820)
    blob_noise = nodes.gradient_noise(lib, material, blob_size, BLOB_LEVELS, -2700, 900, fail)
    blob = nodes.contrast_window(lib, material, blob_noise, "", blob_lo, blob_hi, -1800, 800)

    # --- Erosion: the puff dies by being eaten, not by dimming -----------------------------
    #
    # threshold = Age * ErodeAtDeath, and the puff keeps only where the noise beats it. At
    # birth nothing is eaten; by death most of it is, and what is left is lumps rather than a
    # uniform ghost.
    erode = nodes.scalar(lib, material, "ErodeAtDeath", ERODE_AT_DEATH, -2100, 980)
    threshold = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -1800, 1000)
    lib.connect_material_expressions(age, "", threshold, "A")
    lib.connect_material_expressions(erode, "", threshold, "B")

    kept = lib.create_material_expression(material, unreal.MaterialExpressionSubtract, -1500, 900)
    lib.connect_material_expressions(blob, "", kept, "A")
    lib.connect_material_expressions(threshold, "", kept, "B")

    # Sharpened before the clamp - see ERODE_SHARPNESS for why a ramp here made the whole
    # puff invisible.
    sharpness = nodes.scalar(lib, material, "ErodeSharpness", ERODE_SHARPNESS, -1500, 1080)
    cut = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -1350, 900)
    lib.connect_material_expressions(kept, "", cut, "A")
    lib.connect_material_expressions(sharpness, "", cut, "B")

    # Saturate, so an eaten lump is gone rather than negative - a negative opacity multiplies
    # back to positive further down and would make holes glow.
    kept_clamped = lib.create_material_expression(material, unreal.MaterialExpressionSaturate, -1150, 900)
    lib.connect_material_expressions(cut, "", kept_clamped, "")

    # --- Opacity --------------------------------------------------------------------------
    strength = nodes.scalar(lib, material, "SmokeOpacity", SMOKE_OPACITY, -1300, 400)
    shaped = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -1000, 500)
    lib.connect_material_expressions(envelope, "", shaped, "A")
    lib.connect_material_expressions(kept_clamped, "", shaped, "B")

    opacity = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -750, 500)
    lib.connect_material_expressions(shaped, "", opacity, "A")
    lib.connect_material_expressions(strength, "", opacity, "B")
    lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("built %s" % path)
    for info in lib.get_scalar_parameter_names(material):
        say("scalar parameter %s" % info)
    return material


def point_content_at(material):
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - content set not updated" % CONTENT)
        return
    content.set_editor_property("tyre_smoke_material", material)
    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)
    say("%s.tyre_smoke_material = %s"
        % (CONTENT, content.get_editor_property("tyre_smoke_material")))


MATERIAL = build_material()
if MATERIAL is not None:
    point_content_at(MATERIAL)
say("done")
