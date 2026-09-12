"""Authors M_Ground and assigns it to the Landscape. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

Colours are sampled from the concept sheet's own swatch panel (spec section 1.1), not
chosen by eye. The ground is DELIBERATELY restrained - about 50% saturation - because a
calm ground is what makes a yellow tug and a red-and-white windsock read as function at a
glance. Saturating the grass would put the field in competition with the gameplay objects.

FOUR THINGS HERE ARE DELIBERATE AND EASY TO "FIX" WRONGLY:

1. MACRO VARIATION IS VALUE-ONLY AND MULTIPLIED IN, never a lerp between two colours. A
   colour lerp drifts hue and saturation as it varies, which is precisely the painterly
   look section 1 rejects. A multiply moves V and leaves S alone.

2. THE VARIATION IS APPLIED AFTER THE LAYER BLEND, not to each layer's colour. Same
   result, one multiply instead of three.

3. NO NORMAL MAPS, AND ROUGHNESS IS WHAT REPLACES THEM. At 6-600 m a grass normal map
   reads as mush. The actual failure mode of a flat untextured plane is not flatness, it
   is a broad SPECULAR SHEEN band under a soft sun with Lumen - and the fix for that is
   high, slightly varying roughness, not geometry detail.

4. WORLD POSITION, NOT LandscapeLayerCoords, drives the variation. LandscapeLayerCoords'
   UV0 is landscape-local in QUADS, so at this landscape's 2 m quads a MappingScale of 1
   would repeat every 2 metres. World position also survives a landscape resize and gives
   the SAME breakup on the apron and taxiway meshes, so ground and pavement agree.

The Noise node is deliberately not used: its defaults are Levels 6, which is expensive
evaluated full-screen over a 3 km landscape. One tiling texture sampled twice is cheaper
and is what the engine's own macro-variation material does.

The docstring header said "the three landscape layer infos" once; it does not author them
- see below.

THE LAYER INFO ASSETS ARE NOT AUTHORED HERE, because the engine does not expose it.
ULandscapeLayerInfoObject::LayerName is UPROPERTY(VisibleAnywhere) - read-only to Python,
which refuses with "Property 'LayerName' ... is read-only and cannot be set" - and
ALandscapeProxy::AddTargetLayer (LandscapeProxy.h:1606-1609) is LANDSCAPE_API C++ with no
UFUNCTION. Same family as the Landscape actor itself (spec 7.2). They are created in the
editor with the + button beside each target layer in the Paint tab, which also sets the
name correctly - and that has to happen in the editor anyway, because painting the initial
coverage has no Python API either.

UNPAINTED GROUND READS AS MOWN GRASS, NOT BLACK, and that is deliberate. A bare
LB_WeightBlend with no weight data outputs zero - a black field - which is the usual reason
a new landscape material looks broken. A coverage mask (see build_material) lerps from the
GrassMown colour to the blend, so the field can be looked at before a single stroke is
painted and painting still behaves normally afterwards.

WEIGHT-EDGE NOISE IS NOT DONE HERE, on purpose. The weightmap is one texel per quad = 2 m,
so painted layer edges are inherently blocky and want noise breaking them up. That needs
the weights extracted and the colours lerped by hand rather than a LandscapeLayerBlend.
It is deferred because the first pass paints GrassMown everywhere and has no edges yet -
it becomes necessary the moment dirt is painted at a hangar door.
"""
import unreal

OUT_DIR = "/Game/Environment"
MAT_NAME = "M_Ground"
LEVEL = "/Game/Maps/M_Starter"

# Good64x64TilingNoiseHighFreq, not T_Default_MacroVariation. The latter ships on disk but
# is NOT in the asset registry - does_asset_exist and load_asset both say no - so depending
# on it would fail at author time. This one is a purpose-built seamless tiling noise and
# loads cleanly; the repeat distance comes from the UV scale below, not from the texture.
MACRO_TEXTURE = "/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq"

# sRGB hex from spec 1.1, converted to linear.
#   GrassMown  #7D8E47   GrassRough #748546   Dirt #C6B283
# name -> (linear colour, roughness)
#
# Roughness is high and differs per layer: bare earth scatters more than leaf. The numbers
# matter because they, not normals, are what stop the flat plane showing a sheen band.
LAYERS = [
    ("GrassMown",  unreal.LinearColor(0.213, 0.280, 0.070, 1.0), 0.88),
    ("GrassRough", unreal.LinearColor(0.180, 0.245, 0.068, 1.0), 0.90),
    ("Dirt",       unreal.LinearColor(0.573, 0.448, 0.226, 1.0), 0.95),
]

# Two octaves of value-only variation, both off absolute world position.
#
# 0.00008 repeats about every 125 m and carries most of the amplitude, because it is the
# one that still RESOLVES at the 600 m camera; 0.0006 repeats about every 17 m and gives
# the near-camera break-up. Each is remapped to 0.93-1.07 and the two multiply, so the net
# is about 0.87-1.15 - roughly a third of the contrast a painterly tutorial would use.
MACRO_FAR_SCALE = 0.00008
MACRO_NEAR_SCALE = 0.0006
MACRO_HALF_RANGE = 0.07


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def macro_octave(lib, mat, texture, scale, y):
    """One octave: world XY * scale -> texture -> remapped to 1 +/- MACRO_HALF_RANGE."""
    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -1900, y)
    mask = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1700, y)
    mask.set_editor_property("r", True)
    mask.set_editor_property("g", True)
    mask.set_editor_property("b", False)
    mask.set_editor_property("a", False)
    lib.connect_material_expressions(world, "", mask, "")

    scaled = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1500, y)
    scaled.set_editor_property("const_b", scale)
    lib.connect_material_expressions(mask, "", scaled, "A")

    sample = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -1300, y)
    sample.set_editor_property("texture", texture)
    # Shared: Wrap, so sampling this texture twice does not burn two sampler slots.
    sample.set_editor_property("sampler_source", unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
    lib.connect_material_expressions(scaled, "", sample, "UVs")

    # 0..1 -> (1 - half) .. (1 + half)
    span = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1050, y)
    span.set_editor_property("const_b", MACRO_HALF_RANGE * 2.0)
    lib.connect_material_expressions(sample, "R", span, "A")

    out = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -900, y)
    out.set_editor_property("const_b", 1.0 - MACRO_HALF_RANGE)
    lib.connect_material_expressions(span, "", out, "A")
    return out


def build_material():
    lib = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (OUT_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mat = tools.create_asset(MAT_NAME, OUT_DIR, unreal.Material, unreal.MaterialFactoryNew())

    texture = unreal.EditorAssetLibrary.load_asset(MACRO_TEXTURE)
    if texture is None:
        fail("macro variation texture missing: %s" % MACRO_TEXTURE)
        return None

    far = macro_octave(lib, mat, texture, MACRO_FAR_SCALE, -600)
    near = macro_octave(lib, mat, texture, MACRO_NEAR_SCALE, 0)
    variation = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -700, -300)
    lib.connect_material_expressions(far, "", variation, "A")
    lib.connect_material_expressions(near, "", variation, "B")

    # --- Base colour ------------------------------------------------------------------
    colour_blend = lib.create_material_expression(
        mat, unreal.MaterialExpressionLandscapeLayerBlend, -500, 400)
    rough_blend = lib.create_material_expression(
        mat, unreal.MaterialExpressionLandscapeLayerBlend, -500, 900)

    def entries():
        out = []
        for name, _, _ in LAYERS:
            item = unreal.LayerBlendInput()
            item.set_editor_property("layer_name", name)
            item.set_editor_property("blend_type", unreal.LandscapeLayerBlendType.LB_WEIGHT_BLEND)
            # GrassMown previews at full weight so the material's thumbnail and the
            # unpainted landscape both read as grass rather than black.
            item.set_editor_property("preview_weight", 1.0 if name == "GrassMown" else 0.0)
            out.append(item)
        return out

    colour_blend.set_editor_property("layers", entries())
    rough_blend.set_editor_property("layers", entries())

    for index, (name, colour, roughness) in enumerate(LAYERS):
        node = lib.create_material_expression(
            mat, unreal.MaterialExpressionConstant3Vector, -900, 300 + index * 150)
        node.set_editor_property("constant", colour)
        # The per-layer pin is named "Layer <LayerName>" - not the bare name, not an index.
        if not lib.connect_material_expressions(node, "", colour_blend, "Layer %s" % name):
            fail("could not wire colour layer %s" % name)

        rnode = lib.create_material_expression(
            mat, unreal.MaterialExpressionConstant, -900, 850 + index * 120)
        rnode.set_editor_property("r", roughness)
        if not lib.connect_material_expressions(rnode, "", rough_blend, "Layer %s" % name):
            fail("could not wire roughness layer %s" % name)

    # --- Coverage: what makes unpainted ground GRASS rather than black -----------------
    #
    # A weight-blended layer set sums to 1 where anything is painted and to 0 where nothing
    # is. Feeding a constant 1 to every layer therefore yields exactly that coverage mask,
    # and lerping from the GrassMown colour to the blend by it means an untouched landscape
    # reads as mown grass instead of the black a bare LB_WeightBlend would give.
    #
    # This is the "base layer that absorbs unpainted weight" in another form, and it is what
    # lets the field be looked at before anyone has painted a single stroke. Painting still
    # behaves normally: the moment a layer has weight, coverage is 1 and the blend wins.
    coverage = lib.create_material_expression(
        mat, unreal.MaterialExpressionLandscapeLayerBlend, -500, 1500)
    coverage.set_editor_property("layers", entries())
    for name, _, _ in LAYERS:
        one = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -900, 1450)
        one.set_editor_property("r", 1.0)
        if not lib.connect_material_expressions(one, "", coverage, "Layer %s" % name):
            fail("could not wire coverage layer %s" % name)

    base_colour = lib.create_material_expression(
        mat, unreal.MaterialExpressionConstant3Vector, -500, 200)
    base_colour.set_editor_property("constant", LAYERS[0][1])

    covered = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -300, 400)
    lib.connect_material_expressions(base_colour, "", covered, "A")
    lib.connect_material_expressions(colour_blend, "", covered, "B")
    lib.connect_material_expressions(coverage, "", covered, "Alpha")

    tinted = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -200, 400)
    lib.connect_material_expressions(covered, "", tinted, "A")
    lib.connect_material_expressions(variation, "", tinted, "B")
    lib.connect_material_property(tinted, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Roughness --------------------------------------------------------------------
    #
    # The SAME variation drives roughness, so the sheen breaks up wherever the colour does
    # rather than on an independent pattern. (variation - 1) is about -0.13..+0.15; a third
    # of that is the +/-0.05 wobble that keeps the specular band from forming.
    offset = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, -400, 1250)
    offset.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(variation, "", offset, "A")

    scaled_offset = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -300, 1250)
    scaled_offset.set_editor_property("const_b", 0.35)
    lib.connect_material_expressions(offset, "", scaled_offset, "A")

    # Same coverage trick, so unpainted ground gets GrassMown's roughness rather than 0 -
    # a roughness of zero would turn the whole field into a mirror.
    base_rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -500, 800)
    base_rough.set_editor_property("r", LAYERS[0][2])

    covered_rough = lib.create_material_expression(
        mat, unreal.MaterialExpressionLinearInterpolate, -300, 950)
    lib.connect_material_expressions(base_rough, "", covered_rough, "A")
    lib.connect_material_expressions(rough_blend, "", covered_rough, "B")
    lib.connect_material_expressions(coverage, "", covered_rough, "Alpha")

    rough_sum = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -150, 950)
    lib.connect_material_expressions(covered_rough, "", rough_sum, "A")
    lib.connect_material_expressions(scaled_offset, "", rough_sum, "B")
    lib.connect_material_property(rough_sum, "", unreal.MaterialProperty.MP_ROUGHNESS)

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
    say("assigned %s to the landscape" % MAT_NAME)


def verify():
    """connect_material_expressions returning True is NOT evidence the wiring survived a
    save. layer_input is UPROPERTY without EditAnywhere, so reflection cannot read it -
    export_text() on the layer struct is the only readback (spec section 11.1)."""
    ok = True
    mat = unreal.EditorAssetLibrary.load_asset("%s/%s" % (OUT_DIR, MAT_NAME))
    blends = [e for e in unreal.MaterialEditingLibrary.get_material_expressions(mat)
              if isinstance(e, unreal.MaterialExpressionLandscapeLayerBlend)]
    if len(blends) != 3:
        fail("expected 3 layer blends (colour, roughness, coverage), found %d" % len(blends))
        return False
    for blend in blends:
        for item in blend.get_editor_property("layers"):
            name = item.get_editor_property("layer_name")
            before_height = item.export_text().split("HeightInput")[0]
            if "Expression=None" in before_height:
                fail("layer %s has no wired input" % name)
                ok = False
            else:
                say("PASS layer %s is wired" % name)

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.Landscape)]
    if found and found[0].get_editor_property("landscape_material") == mat:
        say("PASS landscape material is %s" % MAT_NAME)
    else:
        fail("landscape material is not %s" % MAT_NAME)
        ok = False
    return ok


def run():
    mat = build_material()
    if mat is None:
        say("DONE")
        return
    assign(mat)
    say("ALL VERIFIED" if verify() else "VERIFY FAILED")
    say("DONE")


run()
