"""Authors M_Ground and assigns it to the Landscape. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

Colours are sampled from the concept sheet's own swatch panel (spec section 1.1), not
chosen by eye. The ground is DELIBERATELY restrained - about 50% saturation - because a
calm ground is what makes a yellow tug and a red-and-white windsock read as function at a
glance. Saturating the grass would put the field in competition with the gameplay objects.

FOUR THINGS HERE ARE DELIBERATE AND EASY TO "FIX" WRONGLY:

1. MACRO VARIATION IS A LERP BETWEEN THREE TONES. This REVERSES what this file said until
   2026-09-19, which was: "value-only and multiplied in, never a lerp between two colours.
   A colour lerp drifts hue and saturation as it varies, which is precisely the painterly
   look section 1 rejects."

   That reasoning was sound and the result was still wrong, which is worth keeping rather
   than deleting. A multiply DOES leave hue alone - so completely that the field measured
   at hue stdev 0.55 degrees, total spread 3.8 degrees, across the entire visible plot. One
   hue times a brightness wobble is one colour, and that is what the screen showed: a flat
   olive sheet. The guard against painterly drift had been set so tight that it forbade the
   variation it was guarding.

   What replaces it is a BOUNDED lerp, which is not the same thing as the painterly ramp
   that was rejected. Three named tones straight out of the palette, a smoothstep with an
   explicit contrast window, and patches sized in hundreds of metres. Drift is prevented by
   the endpoints being palette entries - you cannot land between them and arrive somewhere
   unsanctioned - not by refusing to move at all.

   The value-only wobble SURVIVES on top, because it was never the problem. It breaks up
   the near-camera surface; it just cannot make a region.

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

THE NOISE NODE IS USED FOR THE REGIONS AND THE TEXTURE FOR THE FINE DETAIL, and the split
is the point. This file used to say: "The Noise node is deliberately not used: its defaults
are Levels 6, which is expensive evaluated full-screen over a 3 km landscape. One tiling
texture sampled twice is cheaper." The cost argument was about the DEFAULT, not the node,
and Levels is a property we set - the regions use 2.

What forced the split is a measurement, 2026-09-19. Good64x64TilingNoiseHighFreq is
TMGS_NO_MIPMAPS, in TEXTUREGROUP_UI, TC_GRAYSCALE - it has exactly ONE mip. It is a
high-frequency noise with no low-frequency content in it anywhere, and stretching its tile
to 350 m does not create any: it draws the same busy speckle larger. Sampling a coarse mip
to low-pass it was tried and is a silent no-op, because there is no coarse mip to sample.
So a macro REGION cannot be made from this texture by any scaling, blurring or gain.

The texture keeps the job it is good at - the 17 m value break-up near the camera. The two
region octaves come from Noise nodes, which have genuine low-frequency content and a full
0-1 range without gain.

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
MI_NAME = "MI_Ground"
LEVEL = "/Game/Maps/M_Starter"

# THE LANDSCAPE IS ASSIGNED MI_Ground, NOT M_Ground, and that is the whole point of this
# revision. A Material is compiled shader code: changing a constant in it costs a recompile,
# which here means closing the editor and re-running this script. A MaterialInstanceConstant
# holds parameter OVERRIDES against a parent and costs nothing to change - the tones can be
# dragged in the editor with the level open and judged on screen immediately.
#
# The ruling on 2026-09-19 was "build it tunable, decide on screen", so the defaults below
# are a STARTING POINT, not a finding. When the values are settled in the editor, bake them
# back into this file so a rebuild does not silently undo them.

# Good64x64TilingNoiseHighFreq, not T_Default_MacroVariation. The latter ships on disk but
# is NOT in the asset registry - does_asset_exist and load_asset both say no - so depending
# on it would fail at author time. This one is a purpose-built seamless tiling noise and
# loads cleanly; the repeat distance comes from the UV scale below, not from the texture.
MACRO_TEXTURE = "/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq"

def srgb(hexcode):
    """sRGB hex -> linear FLinearColor. The hex stays visible so it can be diffed against
    the palette table in spec 1.1; converting by hand is how a swatch drifts from its
    source."""
    h = hexcode.lstrip("#")
    out = []
    for i in (0, 2, 4):
        c = int(h[i:i + 2], 16) / 255.0
        out.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    return unreal.LinearColor(out[0], out[1], out[2], 1.0)


# name -> (linear colour, roughness)
#
# Roughness is high and differs per layer: bare earth scatters more than leaf. The numbers
# matter because they, not normals, are what stop the flat plane showing a sheen band.
#
# GrassMown's COLOUR is no longer used for the ground - the region lerp below supplies it -
# but the entry stays, because the layer still needs a roughness and the blend still needs
# a pin per layer. GrassRough and Dirt keep flat colours on purpose: they are PAINTED
# intent, and a painted patch that also wandered through three tones would stop reading as
# a deliberate mark.
LAYERS = [
    ("GrassMown",  srgb("#7D8E47"), 0.88),
    ("GrassRough", srgb("#748546"), 0.90),
    ("Dirt",       srgb("#C6B283"), 0.95),
]

# --- Region tones -----------------------------------------------------------------------
#
# Defaults are the 2026-09-19 proposal (a true green at hue 97 deg), NOT the concept sheet's
# sampled olives (hue 74 deg), so that the greens under discussion are what appears on the
# first screenshot and can be judged rather than imagined. The sampled olives are one drag
# away on the instance, and are kept here so the comparison costs nothing:
#
#   sampled alternative:  ToneA #7D8E47   ToneB #748546   ToneC #C6B283
#
# Whichever wins gets baked in and the palette table in spec 1.1 is amended to match.
TONE_A = srgb("#6F9B58")   # main grass
TONE_B = srgb("#547C49")   # darker areas
TONE_C = srgb("#B6A66A")   # dry / straw patches

# Region sizes in METRES, converted to a UV scale in the graph. Exposed in metres because a
# parameter reading 0.00008 is unjudgeable in the editor, and this has to be judged there.
# Sizes are the BASE octave; with REGION_LEVELS = 4 the visible patches are smaller than
# this - 800 m gives structure at 800, 400, 200 and 100 m, and it is the 100-400 m bands the
# eye actually reads as patches.
#
# Raised from 350/110 on 2026-09-19 after looking at both ends. At 350 m the field was right
# at the 60 m camera and read as CAMOUFLAGE at 600 m, because a zoomed-out frame held about
# twenty cycles of the pattern and twenty cycles of anything is a texture, not terrain. The
# count in frame is what matters, not the size in metres, and the far camera is the one that
# sets it: at 800 m a 600 m view holds three or four regions.
REGION_SIZE_FAR = 800.0    # the big tonal areas the feedback asked for
REGION_SIZE_NEAR = 260.0   # the sparser dry patches
VALUE_NOISE_SIZE = 17.0    # near-camera break-up, the octave that already worked

# Octaves per region noise. FOUR, and the reason is the second failure this file has
# recorded rather than a preference.
#
# Two octaves was tried first, on the argument that finer ones "are smaller than the value
# texture already covers, so they cost instructions to add detail that is then drowned".
# That was wrong in one specific way: the value texture is BRIGHTNESS-ONLY and it is
# 17 m, so past about 100 m it mips to its own average and contributes nothing. With two
# octaves the frame at 600 m therefore contained slow colour gradients and NOTHING ELSE,
# which is precisely what an out-of-focus photograph contains. The user's words for it were
# "blurry splodges that look out of focus when zoomed out", and that is the correct reading
# of the image.
#
# Blur is the absence of detail at the scale you are looking at, so the cure is detail at
# more scales. At LevelScale 2 and a 350 m base, four levels put structure at 350, 175, 87
# and 44 m - the last two are what give a region an irregular, crinkled EDGE instead of the
# smooth oval a two-octave field produces, and an edge is what stops it reading as defocus.
#
# NOT a material parameter: Levels is a plain UPROPERTY on the node, so changing it needs
# this script re-run. The contrast window below does the same job continuously and IS live.
REGION_LEVELS = 4

# GradientALU, not GradientTex, and this one was also settled by looking.
#
# GradientTex is the cheaper function - it reads the engine's noise lookup texture instead
# of computing the gradients - and it was the first choice for exactly that reason. At the
# 60 m camera it laid a net of thin dark DOTTED CONTOUR LINES across the whole field: the
# lookup table is 8-bit, and stretching 256 quantised levels across a region hundreds of
# metres wide puts a visible step at every iso-value. Magnified, the field looked crazed.
#
# The computational variant has no table to quantise, so the artefact cannot occur. It costs
# more ALU and no texture fetch, which on a 3 km landscape evaluated at 2 levels is a trade
# worth making - and the alternative, raising Quality, only buys more samples of the same
# quantised table.
REGION_FUNCTION = unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_ALU

# The contrast window that turns a smooth noise into REGIONS. Without it a lerp driven by
# noise is a soft wash everywhere and still reads as one colour with a bruise on it. Widen
# the window for a gentle gradient, narrow it for distinct patches with defined edges.
# Narrowed from 0.40/0.60 on 2026-09-19, same round as the octaves above. A fractal sum
# concentrates toward its midpoint - the more octaves, the tighter - so a window that was
# already generous for a 2-level noise spanned nearly the whole distribution of a 4-level
# one, putting most of the field mid-lerp and averaging the tones back together. Narrow
# these further for hard-edged patches, widen them for a gentle wash; this is the knob to
# reach for FIRST, because it is live on MI_Ground and needs no rebuild.
REGION_CONTRAST_LO = 0.46
REGION_CONTRAST_HI = 0.54

# How much of the frame the dry tone is allowed to claim. Kept low: straw patches are an
# accent, and a field that is a third straw stops looking maintained. Lowered from 0.35 with
# the sizes above - the tan is the highest-contrast tone against the greens, so it is what
# tipped the zoomed-out frame into reading as a pattern rather than as a field.
PATCH_AMOUNT = 0.25

# The surviving value-only wobble, as a full width: 0.20 means roughly 1 +/- 0.10.
VALUE_VARIATION = 0.20

# All three octaves ride absolute world position - see point 4 in the header for why that
# beats LandscapeLayerCoords, and note it also means the apron and taxiway meshes break up
# on the SAME pattern as the ground rather than on one of their own.


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def clear_graph(lib, mat):
    """Empty a material's node graph, one expression at a time.

    DO NOT USE delete_all_material_expressions HERE. It is broken in 5.8 and deletes
    roughly HALF the graph. MaterialEditingLibrary.cpp:559-568:

        for (UMaterialExpression* Expression : Material->GetExpressions())
        {
            DeleteMaterialExpression(Material, Expression);
        }

    - it range-iterates the live array while DeleteMaterialExpression removes entries from
    that same array, so every second element is stepped over. Measured here on 2026-09-19:
    of three LandscapeLayerBlends at y = 400, 900 and 1500 it removed only the middle one,
    and the rebuild finished with five blends and two orphaned texture samplers in an asset
    that still compiled and still rendered.

    That is the dangerous part: the material was VALID, just wrong, and only a node count in
    verify() caught it. get_material_expressions returns a copied list, so iterating that
    and deleting individually is safe."""
    for expression in list(lib.get_material_expressions(mat)):
        lib.delete_material_expression(mat, expression)
    left = len(lib.get_material_expressions(mat))
    if left:
        fail("graph not empty after clear: %d expressions remain" % left)
    return left == 0


def scalar(lib, mat, name, default, x, y):
    """A named ScalarParameter. Named, because the whole point of this revision is that the
    value can be changed on MI_Ground without recompiling M_Ground."""
    node = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node


def vector(lib, mat, name, default, x, y):
    """A named VectorParameter, same reasoning as scalar()."""
    node = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node


def region_noise(lib, mat, size_param, y):
    """Low-frequency gradient noise for one region octave, 0..1, in METRES.

    GradientTex rather than a computational variant: it is a lookup into the engine's own
    noise texture and is the cheapest of the six functions, which matters because this is
    evaluated per pixel over a 3 km landscape.

    Scale stays 1 and the SIZE is applied by dividing the position instead, because Scale is
    a plain UPROPERTY and cannot be a material parameter - dividing the Position input is
    what keeps the size tunable on MI_Ground, which is the whole point of the instance.

    The full 3D world position is fed, not a masked XY. The landscape is dead flat at Z=0
    and road plates sit 10 cm above it, so the Z contribution is 0.001 of one noise unit at
    these sizes - below anything that could show."""
    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -2400, y)

    cm = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -2200, y + 120)
    cm.set_editor_property("const_b", 100.0)
    lib.connect_material_expressions(size_param, "", cm, "A")

    pos = lib.create_material_expression(mat, unreal.MaterialExpressionDivide, -2000, y)
    lib.connect_material_expressions(world, "", pos, "A")
    lib.connect_material_expressions(cm, "", pos, "B")

    noise = lib.create_material_expression(mat, unreal.MaterialExpressionNoise, -1750, y)
    noise.set_editor_property("noise_function", REGION_FUNCTION)
    noise.set_editor_property("scale", 1.0)
    noise.set_editor_property("quality", 2)
    noise.set_editor_property("levels", REGION_LEVELS)
    noise.set_editor_property("output_min", 0.0)
    noise.set_editor_property("output_max", 1.0)
    noise.set_editor_property("turbulence", False)
    # "" - THE EMPTY STRING - NOT "Position", although the UPROPERTY is called Position.
    # MaterialEditingLibrary.cpp:46-70 resolves an input name by comparing against
    # UMaterialExpression::GetInputName(index), and UMaterialExpressionNoise does not
    # override it, so its inputs are nameless and "Position" matches nothing. The same
    # function short-circuits on an empty name and returns GetInput(0), which IS Position -
    # it is declared first in the header. Passing the obvious string returns false and
    # leaves the node reading a constant zero position, which renders as FLAT GROUND, not
    # as an error. It is only caught because this call site checks the return.
    if not lib.connect_material_expressions(pos, "", noise, ""):
        fail("could not wire Position on the region noise at y=%d" % y)
    return noise


def noise_octave(lib, mat, texture, size_param, y):
    """One octave of tiling noise off world XY, at a repeat distance given in METRES by
    size_param. Returns the TextureSample node - read its "R" output.

    The division rather than a multiply by a tiny constant is deliberate: it is what lets
    the parameter read 350 instead of 0.0000286, and a number a human cannot judge is a
    number that never gets tuned."""
    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -2400, y)
    mask = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -2200, y)
    mask.set_editor_property("r", True)
    mask.set_editor_property("g", True)
    mask.set_editor_property("b", False)
    mask.set_editor_property("a", False)
    lib.connect_material_expressions(world, "", mask, "")

    # metres -> centimetres, because world position is in uu and 1 uu is 1 cm.
    cm = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -2000, y + 120)
    cm.set_editor_property("const_b", 100.0)
    lib.connect_material_expressions(size_param, "", cm, "A")

    uv = lib.create_material_expression(mat, unreal.MaterialExpressionDivide, -1800, y)
    lib.connect_material_expressions(mask, "", uv, "A")
    lib.connect_material_expressions(cm, "", uv, "B")

    sample = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -1600, y)
    sample.set_editor_property("texture", texture)
    # Shared: Wrap, so sampling this texture three times does not burn three sampler slots.
    sample.set_editor_property("sampler_source", unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
    lib.connect_material_expressions(uv, "", sample, "UVs")

    return sample


def build_material():
    lib = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (OUT_DIR, MAT_NAME)
    # REBUILD IN PLACE - do not delete and recreate. This file used to delete M_Ground and
    # make a new one, which was harmless while nothing referenced it. MI_Ground now does,
    # and a delete-and-recreate strands that reference: the instance keeps pointing at the
    # object that was destroyed, not at the one with the same path. DeleteAllMaterialExpressions
    # empties the graph while the asset keeps its identity, so both the instance's parent
    # pointer and the Landscape's assignment survive a rebuild untouched.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        clear_graph(lib, mat)
        say("rebuilding %s in place" % MAT_NAME)
    else:
        mat = tools.create_asset(MAT_NAME, OUT_DIR, unreal.Material, unreal.MaterialFactoryNew())

    texture = unreal.EditorAssetLibrary.load_asset(MACRO_TEXTURE)
    if texture is None:
        fail("macro variation texture missing: %s" % MACRO_TEXTURE)
        return None

    # --- Macro colour regions ---------------------------------------------------------
    #
    # This is the answer to "the grass is one colour". Three tones, two noise octaves, and
    # a contrast window - NOT a brightness wobble, which is what was here before and which
    # measured at 0.55 degrees of hue spread across the whole plot.
    size_far = scalar(lib, mat, "RegionSizeFar", REGION_SIZE_FAR, -2600, -1400)
    size_near = scalar(lib, mat, "RegionSizeNear", REGION_SIZE_NEAR, -2600, -800)
    size_val = scalar(lib, mat, "ValueNoiseSize", VALUE_NOISE_SIZE, -2600, -200)

    n_far = region_noise(lib, mat, size_far, -1400)
    n_near = region_noise(lib, mat, size_near, -800)
    n_val = noise_octave(lib, mat, texture, size_val, -200)

    # SmoothStep is what makes a REGION rather than a wash. A raw noise lerped between two
    # colours puts every intermediate tone everywhere and averages back to one colour at any
    # distance; remapping it through a narrow window pushes most of the field to one end or
    # the other, which is what "patches tens or hundreds of metres across" actually means.
    lo = scalar(lib, mat, "RegionContrastLo", REGION_CONTRAST_LO, -1350, -1600)
    hi = scalar(lib, mat, "RegionContrastHi", REGION_CONTRAST_HI, -1350, -1500)
    region = lib.create_material_expression(mat, unreal.MaterialExpressionSmoothStep, -1150, -1400)
    lib.connect_material_expressions(lo, "", region, "Min")
    lib.connect_material_expressions(hi, "", region, "Max")
    lib.connect_material_expressions(n_far, "", region, "Value")

    tone_a = vector(lib, mat, "ToneA", TONE_A, -1350, -1150)
    tone_b = vector(lib, mat, "ToneB", TONE_B, -1350, -1000)
    tone_c = vector(lib, mat, "ToneC", TONE_C, -1350, -850)

    two_tone = lib.create_material_expression(
        mat, unreal.MaterialExpressionLinearInterpolate, -950, -1100)
    lib.connect_material_expressions(tone_a, "", two_tone, "A")
    lib.connect_material_expressions(tone_b, "", two_tone, "B")
    lib.connect_material_expressions(region, "", two_tone, "Alpha")

    # The dry tone rides its own octave at a different size, so straw patches do not line up
    # with the boundaries between the two greens. Two patterns agreeing is how procedural
    # ground starts looking like a tiled texture.
    patch_amt = scalar(lib, mat, "PatchAmount", PATCH_AMOUNT, -1350, -700)
    patch = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1150, -750)
    lib.connect_material_expressions(n_near, "", patch, "A")
    lib.connect_material_expressions(patch_amt, "", patch, "B")

    regions = lib.create_material_expression(
        mat, unreal.MaterialExpressionLinearInterpolate, -800, -950)
    lib.connect_material_expressions(two_tone, "", regions, "A")
    lib.connect_material_expressions(tone_c, "", regions, "B")
    lib.connect_material_expressions(patch, "", regions, "Alpha")

    # --- The value wobble, kept -------------------------------------------------------
    #
    # It was never the problem - it just could not make a region. On top of three tones it
    # does the job it was always doing: breaking up the near-camera surface and stopping the
    # specular sheen band forming. One octave now instead of two, because the far octave's
    # work is done by the regions above.
    amount = scalar(lib, mat, "ValueVariation", VALUE_VARIATION, -1350, -300)
    dev = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, -1350, -150)
    dev.set_editor_property("const_b", 0.5)
    lib.connect_material_expressions(n_val, "R", dev, "A")

    scaled_dev = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1150, -150)
    lib.connect_material_expressions(dev, "", scaled_dev, "A")
    lib.connect_material_expressions(amount, "", scaled_dev, "B")

    variation = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -950, -150)
    variation.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(scaled_dev, "", variation, "A")

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
        # GRASSMOWN TAKES THE REGION CHAIN, NOT A FLAT COLOUR, and getting this wrong is
        # how the whole feature ships invisible. The coverage lerp further down only shows
        # its A input where NOTHING is painted; the first landscape pass painted GrassMown
        # across the entire plot, so coverage is 1 everywhere and the layer blend wins
        # everywhere. Feeding the regions only to the coverage fallback would therefore have
        # produced a graph that is correct, compiles, verifies - and renders flat olive.
        if name == "GrassMown":
            node = regions
        else:
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

    # The unpainted fallback is the SAME region chain, so painted-everywhere and
    # painted-nowhere land on identical ground and no seam appears at the coverage edge.
    covered = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -300, 400)
    lib.connect_material_expressions(regions, "", covered, "A")
    lib.connect_material_expressions(colour_blend, "", covered, "B")
    lib.connect_material_expressions(coverage, "", covered, "Alpha")

    tinted = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -200, 400)
    lib.connect_material_expressions(covered, "", tinted, "A")
    lib.connect_material_expressions(variation, "", tinted, "B")
    lib.connect_material_property(tinted, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Roughness --------------------------------------------------------------------
    #
    # The SAME variation drives roughness, so the sheen breaks up wherever the surface does
    # rather than on an independent pattern. (variation - 1) is now +/- VALUE_VARIATION/2,
    # so +/-0.10 at the default; a third of that is the +/-0.035 wobble that keeps the
    # specular band from forming. Note it rides the VALUE octave, not the region noise -
    # the regions are a colour statement and should not also change how the ground shines.
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


def build_instance(mat):
    """MI_Ground, the thing the Landscape actually uses.

    Created ONLY if absent. On a rebuild the existing instance is left exactly as it is,
    overrides and all - which is the entire point. Tones tuned in the editor must survive
    re-running this script, or "decide on screen" becomes "decide on screen and lose it"."""
    lib = unreal.MaterialEditingLibrary
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (OUT_DIR, MI_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mi = unreal.EditorAssetLibrary.load_asset(path)
        say("%s exists - leaving its overrides alone" % MI_NAME)
    else:
        mi = tools.create_asset(
            MI_NAME, OUT_DIR, unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
        say("created %s" % MI_NAME)
    # Asserted every run rather than only on creation: an instance whose parent silently
    # became None renders the default checker and looks like a material bug.
    lib.set_material_instance_parent(mi, mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return mi


def assign(mi):
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.Landscape)]
    if len(found) != 1:
        fail("expected exactly 1 Landscape, found %d" % len(found))
        return
    found[0].set_editor_property("landscape_material", mi)
    levels.save_current_level()
    say("assigned %s to the landscape" % MI_NAME)


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

    # The region chain, checked by its OUTCOME rather than by counting nodes. A node count
    # passes on a graph whose wires all go to the wrong pins; what actually has to be true
    # is that the parameters exist by the names the instance overrides them by, and that
    # GrassMown's colour pin is fed by something that is NOT a constant.
    names = set()
    for e in unreal.MaterialEditingLibrary.get_material_expressions(mat):
        if isinstance(e, (unreal.MaterialExpressionScalarParameter,
                          unreal.MaterialExpressionVectorParameter)):
            names.add(str(e.get_editor_property("parameter_name")))
    expected = {"ToneA", "ToneB", "ToneC", "RegionSizeFar", "RegionSizeNear",
                "ValueNoiseSize", "RegionContrastLo", "RegionContrastHi",
                "PatchAmount", "ValueVariation"}
    missing = expected - names
    if missing:
        fail("parameters missing from %s: %s" % (MAT_NAME, ", ".join(sorted(missing))))
        ok = False
    else:
        say("PASS all %d tunable parameters present" % len(expected))

    # GrassMown must be fed the region chain, not a Constant3Vector - see the comment at
    # the wiring site for why a flat colour there renders the whole feature invisible.
    for blend in blends:
        for item in blend.get_editor_property("layers"):
            if str(item.get_editor_property("layer_name")) != "GrassMown":
                continue
            wired = item.export_text().split("HeightInput")[0]
            if "Constant3Vector" in wired:
                fail("GrassMown colour is a flat constant - the regions will not render")
                ok = False

    # Both region noises must exist. Their POSITION INPUT CANNOT BE CHECKED HERE and this
    # comment is here so nobody later assumes it is: UMaterialExpressionNoise::Position is
    # a protected UPROPERTY, so get_editor_property refuses it with "is protected and cannot
    # be read", and export_text() is a struct method that expressions do not have. Same
    # family as FLayerBlendInput::layer_input in spec section 11.1.
    #
    # What guards it instead is the RETURN VALUE of connect_material_expressions at the
    # wiring site, which fails loudly - and it has already earned its keep once, catching a
    # connection to "Position" that silently did nothing. The count below is a real check;
    # it is not a check of the wiring, and a screenshot remains the only proof of that.
    noises = [e for e in unreal.MaterialEditingLibrary.get_material_expressions(mat)
              if isinstance(e, unreal.MaterialExpressionNoise)]
    if len(noises) != 2:
        fail("expected 2 region noise nodes, found %d" % len(noises))
        ok = False
    else:
        say("PASS 2 region noise nodes present (their Position is unverifiable - see comment)")

    mi = unreal.EditorAssetLibrary.load_asset("%s/%s" % (OUT_DIR, MI_NAME))
    if mi is None:
        fail("%s does not exist" % MI_NAME)
        return False
    if mi.get_editor_property("parent") == mat:
        say("PASS %s is parented to %s" % (MI_NAME, MAT_NAME))
    else:
        fail("%s is not parented to %s" % (MI_NAME, MAT_NAME))
        ok = False

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.Landscape)]
    if found and found[0].get_editor_property("landscape_material") == mi:
        say("PASS landscape material is %s" % MI_NAME)
    else:
        fail("landscape material is not %s" % MI_NAME)
        ok = False
    return ok


def run():
    mat = build_material()
    if mat is None:
        say("DONE")
        return
    mi = build_instance(mat)
    assign(mi)
    say("ALL VERIFIED" if verify() else "VERIFY FAILED")
    say("DONE")


run()
