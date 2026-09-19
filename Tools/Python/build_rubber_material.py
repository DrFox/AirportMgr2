"""Authors M_RunwayRubber and points DA_AirsideContent at it. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

The tyre rubber in a runway's touchdown zone. FRunwayMarkingBuilder::BuildRubber lays two
bands per runway end, either side of the centreline; this is what they are drawn with.

TRANSLUCENT, AND THAT IS THE DESIGN DECISION HERE.

The first plan was opaque: the rubber quad would REPRODUCE the pavement beneath it from
world position - the same BaseColour, wear noise and grain the pavement uses, since all
three are functions of position - and then darken it. That works, lights identically and
avoids the translucency pass entirely.

It was abandoned for a reason the plan had not noticed: ONE component carries the rubber of
EVERY runway, and runways do not all have the same surface. A patch reproducing tarmac
would be visibly wrong on a concrete runway, and a component has one material. The opaque
version would therefore have needed per-surface material ids on the rubber quads and a slot
per runway surface - real machinery, to solve a problem translucency does not have.

A translucent stain darkens whatever is under it without knowing what that is. It also
keeps working for any pavement added later, which the opaque version would not have.

What it costs, stated plainly: translucent surfaces take a different lighting path from the
opaque pavement around them, so in principle the rubber can catch light slightly differently
from what it lies on. On a near-black stain on flat ground this is not expected to be
visible - but it is an expectation, not a measurement, and the screenshot is what settles
it.

THE MASK IS THE WHOLE MATERIAL. Opacity, not colour, is what makes this read as a stain:

  edge  = smoothstep(0, EdgeSoftness, min(u, 1-u)) * smoothstep(0, EdgeSoftness, min(v, 1-v))
  blotch = gradient noise off world position
  opacity = edge * blotch * RubberStrength

SYMMETRIC IN U AND V ON PURPOSE, and this is not tidiness. MarkingQuads::AddQuad decides its
corner order by SIGNED AREA - it swaps two corners when the caller hands them over
clockwise - so which of UV0's axes ends up along the band and which across is not knowable
here. A mask that treated u and v differently would fade along the runway on some quads and
across it on others. Applying one softness to both is immune to the swap, and it happens to
be what is wanted anyway: the UV is 0..1 on both axes while the band is roughly 435 m long
and 12 m wide, so the same softness in UV is a long fade along the strip and a tight one
across it - which is what rubber does.

The noise is off WORLD position, not UV, so the blotching does not repeat identically on
every patch and does not stretch with the band.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airside_matnodes as nodes

MAT_DIR = "/Game/Materials"
MAT_NAME = "M_RunwayRubber"
CONTENT = "/Game/DA_AirsideContent"

# Near-black, faintly warm: burnt rubber is not a neutral grey. Dark enough to read as a
# stain at the build camera, nowhere near black enough to read as a hole in the pavement.
RUBBER_COLOUR = "#1A1614"

# Peak opacity at the centre of a band, before the blotch noise takes its bite. Well under
# 1: rubber stains pavement, it does not resurface it, and the markings underneath have to
# stay legible - that is the whole reason the rubber is drawn below them.
RUBBER_STRENGTH = 0.55

# How much of the band's own UV is spent fading. 0.35 means the middle 30% is at full
# strength and everything outside ramps away - a stain with no edge anywhere.
EDGE_SOFTNESS = 0.35

# The blotching. Metres, off world position: big enough that a patch is not uniform, small
# enough that several cycles cross one band.
BLOTCH_SIZE = 14.0
BLOTCH_LEVELS = 3
# A wide window, unlike the grass: rubber has no edges of its own, only more and less.
BLOTCH_LO = 0.15
BLOTCH_HI = 0.85
# How much of the opacity the blotch is allowed to remove. 1.0 would punch clean holes.
BLOTCH_DEPTH = 0.55


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def axis_falloff(lib, mat, uv, channel, softness, x, y):
    """smoothstep(0, softness, min(c, 1 - c)) for one UV channel - 0 at both edges, 1 in
    the middle. The min() is what makes it symmetric, so a quad whose corners were swapped
    by AddQuad's winding fix fades exactly the same way."""
    mask = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, x, y)
    mask.set_editor_property("r", channel == 0)
    mask.set_editor_property("g", channel == 1)
    mask.set_editor_property("b", False)
    mask.set_editor_property("a", False)
    lib.connect_material_expressions(uv, "", mask, "")

    flipped = lib.create_material_expression(mat, unreal.MaterialExpressionOneMinus, x + 180, y + 90)
    lib.connect_material_expressions(mask, "", flipped, "")

    nearest = lib.create_material_expression(mat, unreal.MaterialExpressionMin, x + 360, y)
    lib.connect_material_expressions(mask, "", nearest, "A")
    lib.connect_material_expressions(flipped, "", nearest, "B")

    ramp = lib.create_material_expression(mat, unreal.MaterialExpressionSmoothStep, x + 540, y)
    ramp.set_editor_property("const_min", 0.0)
    lib.connect_material_expressions(softness, "", ramp, "Max")
    lib.connect_material_expressions(nearest, "", ramp, "Value")
    return ramp


def build_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, MAT_NAME)

    # In place, like every other material script here - the actor and the content set both
    # hold pointers to the object, not to the path.
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

    # TRANSLUCENT, and unlit. Unlit because a stain has no surface of its own: it darkens
    # what is behind it, and giving it a lit surface would have it catch a specular
    # highlight the pavement underneath is not catching.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)

    # --- The stain's colour --------------------------------------------------------------
    colour = nodes.vector(lib, material, "RubberColour", RUBBER_COLOUR, -700, -400)
    lib.connect_material_property(colour, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # --- The mask ------------------------------------------------------------------------
    uv0 = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -2600, 200)
    uv0.set_editor_property("coordinate_index", 0)

    softness = nodes.scalar(lib, material, "EdgeSoftness", EDGE_SOFTNESS, -2600, 60)
    along = axis_falloff(lib, material, uv0, 0, softness, -2300, 100)
    across = axis_falloff(lib, material, uv0, 1, softness, -2300, 420)

    edge = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -1500, 250)
    lib.connect_material_expressions(along, "", edge, "A")
    lib.connect_material_expressions(across, "", edge, "B")

    # The blotching, off world position so no two patches are identical.
    blotch_size = nodes.scalar(lib, material, "BlotchSize", BLOTCH_SIZE, -2600, 700)
    blotch_lo = nodes.scalar(lib, material, "BlotchLo", BLOTCH_LO, -1800, 640)
    blotch_hi = nodes.scalar(lib, material, "BlotchHi", BLOTCH_HI, -1800, 720)
    blotch_noise = nodes.gradient_noise(lib, material, blotch_size, BLOTCH_LEVELS, -2500, 800, fail)
    blotch = nodes.contrast_window(lib, material, blotch_noise, "", blotch_lo, blotch_hi, -1500, 700)

    # lerp(1 - depth, 1, blotch): the noise REMOVES up to BlotchDepth of the opacity and
    # never adds any. Multiplying by the raw noise instead would let a patch reach zero
    # opacity wherever the noise did, punching clean holes in the stain.
    depth = nodes.scalar(lib, material, "BlotchDepth", BLOTCH_DEPTH, -1500, 860)
    floor = lib.create_material_expression(material, unreal.MaterialExpressionOneMinus, -1300, 860)
    lib.connect_material_expressions(depth, "", floor, "")

    blotch_amount = lib.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -1100, 750)
    lib.connect_material_expressions(floor, "", blotch_amount, "A")
    blotch_amount.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(blotch, "", blotch_amount, "Alpha")

    strength = nodes.scalar(lib, material, "RubberStrength", RUBBER_STRENGTH, -1100, 420)

    masked = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -800, 300)
    lib.connect_material_expressions(edge, "", masked, "A")
    lib.connect_material_expressions(blotch_amount, "", masked, "B")

    opacity = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -600, 300)
    lib.connect_material_expressions(masked, "", opacity, "A")
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
    content.set_editor_property("rubber_material", material)
    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)
    # Read back: a soft pointer set to an asset that failed to save is a null at load time.
    say("%s.rubber_material = %s" % (CONTENT, content.get_editor_property("rubber_material")))


MATERIAL = build_material()
if MATERIAL is not None:
    point_content_at(MATERIAL)
say("done")
