"""Authors M_ApronConcrete. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

An apron is concrete, not asphalt. Beyond realism, distinctness is the point: while the
aprons borrowed M_RoadSurface a new one was very hard to tell from the taxiway lying on it
and from the ground under it, and "hard to tell from the ground" is indistinguishable from
not rendering at all.

RESTYLED 2026-09-19, THE SAME ROUND AS M_RoadSurface AND FOR THE SAME REASON. This script
used to import a four-map `degraded-concrete` PBR set from a vendor directory outside the
repository. Once the taxiway beside it went flat and stylised, the apron stopped merely
being photoreal and started being WRONG - a brown pebbled gravel pad butted against clean
grey tarmac, visibly from another game. A surface does not get to opt out of the art
direction because it has its own graph.

It is now the same three-part structure as the road: a flat palette colour, a paler second
tone in low-frequency patches, and a fine value-only grain. Warm concrete `#9C9B91` against
the taxiway's `#454D50`, which is the contrast that does the work the old brown was doing
badly.

ONE DELIBERATE DEPARTURE FROM M_RoadSurface SURVIVES: NO UV1. That material reads UV1 for
its centreline, and an apron's UV1 is zero at every vertex because lateral offset and
distance along a centreline mean nothing for a polygon. Sampling it would paint the whole
apron as one enormous centre marking - not a marking, a bug that happens to be visible.

TWO FACTS KEPT FROM THE IMPORT MACHINERY, because they apply to any texture this material
samples:

  - Sampler type MUST agree with the texture's compression setting. GetSamplerTypeForTexture
    maps TC_Masks to SAMPLERTYPE_Masks and TC_Grayscale-without-sRGB to
    SAMPLERTYPE_LinearGrayscale, and VerifySamplerType rejects any mismatch outright: the
    material fails to compile and every surface using it falls back to the engine default.
  - The normal map is gone, so the tangent-frame hazard it carried is gone with it. For the
    record, because it will come up again if anything here ever samples one: the component
    leaves tangents ExternallyProvided and falls back to a frame derived from the normal
    alone, which on a flat +Z surface is a valid constant basis. Under AutoCalculated it
    would instead derive the frame from the UV layers, hit this mesh's degenerate UV1, and
    produce NaN tangents the GPU silently discards.

The old concrete textures stay in /Game/Textures, unreferenced. Headless deletion reports
success while leaving the asset on disk, so removing them is a separate, verified step.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airside_palette as palette
import airside_matnodes as nodes

MAT_DIR = "/Game/Materials"
MAT_NAME = "M_ApronConcrete"

NOISE_TEXTURE = "/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq"

# Warm concrete against the taxiway's cool asphalt grey. The pale tone is the palette's
# concrete highlight, so wear on an apron reads as sun-bleached slab rather than as dirt -
# concrete weathers LIGHTER, which is the opposite of what asphalt does and the reason the
# two surfaces do not share a wear colour.
BASE_COLOUR = palette.APRON_CONCRETE
WEAR_COLOUR = palette.CONCRETE_HIGHLIGHT

# Larger than the road's 40 m: an apron is a wide open pad rather than a ribbon, so its
# variation can afford to be read across, and slab-scale patches would read as damage.
WEAR_SIZE = 70.0
WEAR_LEVELS = 3
WEAR_CONTRAST_LO = 0.32
WEAR_CONTRAST_HI = 0.68

GRAIN_SIZE = 2.0
GRAIN_AMOUNT = 0.07

# Slightly smoother than asphalt. Concrete is a closed surface where asphalt is an open
# aggregate one, and the difference is visible as a broader, softer sheen.
ROUGHNESS_BASE = 0.74


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def build_apron_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, MAT_NAME)

    # REBUILT IN PLACE, and this material is the reason the helper exists.
    #
    # ARoadNetworkActor resolves it by path in its constructor, so it is already in memory
    # when this script runs. delete_asset then reports success while the package stays
    # loaded, and create_asset refuses with "already exists ... cannot ask the user as the
    # application is running unattended" - failing the whole script. Re-creating would be
    # wrong even when it works: a new asset is a new object, and DA_RoadMaterials slot 1
    # plus the actor itself would be left pointing at a deleted package.
    #
    # An earlier note here claimed clearing in place was impossible, because
    # delete_all_material_expressions "asserts !IsRooted() on a material the CDO is
    # holding". Checked against the engine on 2026-09-19 and that is not what the code
    # does: DeleteMaterialExpression (MaterialEditingLibrary.cpp:590-612) calls
    # MarkAsGarbage on the EXPRESSION, never on the material, and expressions are not
    # rooted. What that function IS guilty of is deleting half a graph when called through
    # delete_all_material_expressions - see nodes.clear_graph, which is why this goes
    # through the helper rather than the library call.
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

    noise_texture = unreal.EditorAssetLibrary.load_asset(NOISE_TEXTURE)
    if noise_texture is None:
        fail("grain texture missing: %s" % NOISE_TEXTURE)
        return None

    # --- Colour and wear ---------------------------------------------------------------
    base_colour = nodes.vector(lib, material, "BaseColour", BASE_COLOUR, -2000, -700)
    wear_colour = nodes.vector(lib, material, "WearColour", WEAR_COLOUR, -2000, -560)

    wear_size = nodes.scalar(lib, material, "WearSize", WEAR_SIZE, -3000, -400)
    wear_lo = nodes.scalar(lib, material, "WearContrastLo", WEAR_CONTRAST_LO, -2000, -420)
    wear_hi = nodes.scalar(lib, material, "WearContrastHi", WEAR_CONTRAST_HI, -2000, -340)

    wear_noise = nodes.gradient_noise(lib, material, wear_size, WEAR_LEVELS, -2900, -400, fail)
    wear = nodes.contrast_window(lib, material, wear_noise, "", wear_lo, wear_hi, -1750, -400)

    weathered = lib.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -1500, -600)
    lib.connect_material_expressions(base_colour, "", weathered, "A")
    lib.connect_material_expressions(wear_colour, "", weathered, "B")
    lib.connect_material_expressions(wear, "", weathered, "Alpha")

    grain_size = nodes.scalar(lib, material, "GrainSize", GRAIN_SIZE, -3000, -100)
    grain_amount = nodes.scalar(lib, material, "GrainAmount", GRAIN_AMOUNT, -3000, -20)
    grain = nodes.value_grain(
        lib, material, noise_texture, grain_size, grain_amount, -2900, -100)

    surface = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -1300, -500)
    lib.connect_material_expressions(weathered, "", surface, "A")
    lib.connect_material_expressions(grain, "", surface, "B")

    lib.connect_material_property(surface, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Roughness ---------------------------------------------------------------------
    # The same grain drives the sheen, so surface and shine agree rather than forming two
    # independent patterns.
    rough_base = nodes.scalar(lib, material, "RoughnessBase", ROUGHNESS_BASE, -650, 300)
    grain_offset = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -500, 380)
    grain_offset.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(grain, "", grain_offset, "A")

    grain_scaled = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -350, 380)
    grain_scaled.set_editor_property("const_b", 0.35)
    lib.connect_material_expressions(grain_offset, "", grain_scaled, "A")

    roughness = lib.create_material_expression(
        material, unreal.MaterialExpressionAdd, -200, 340)
    lib.connect_material_expressions(rough_base, "", roughness, "A")
    lib.connect_material_expressions(grain_scaled, "", roughness, "B")

    lib.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("%s built and saved" % path)

    for info in lib.get_vector_parameter_names(material):
        say("vector parameter %s" % info)
    return material


build_apron_material()
say("done")
