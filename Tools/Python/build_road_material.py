"""Authors M_RoadSurface, the parent of every paved surface. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because
print() goes to the log rather than stdout under the commandlet.

RESTYLED 2026-09-19 - THE PHOTOREAL ASPHALT SET IS GONE.

This script used to import a four-map PBR set (`pebbled_asphalt` albedo, normal, roughness
and ambient occlusion) from a vendor directory outside the repository, and fed all four
straight into the material. That made the runway the most photoreal surface in the game, sitting
underneath the most stylised markings in it, and it is the single biggest thing the player
looks at. It contradicted the art direction's own words - "flat base colours, no normal
maps" - and the identity claim behind them, which is that this should not look like another
Unreal project assembled from scan libraries.

The normal map was doing most of that work. At the 6 m camera it gave every square metre a
pebble; at 600 m all four maps are mush and cost bandwidth to be mush.

What replaces them is the structure proved on the ground the same day: a flat palette
colour, a second tone in low-frequency patches for wear, and a fine value-only grain for
near-camera break-up. Dialled much further down than the grass - pavement should read as
ONE material with wear in it, not as patches - and at a far smaller size, because a runway
is a 45 m ribbon and its variation has to show several cycles across that width.

The import machinery went with it. Two hard-won facts from it are kept because they apply
to any texture this material ever samples:

  - Sampler type MUST agree with the texture's compression setting. GetSamplerTypeForTexture
    maps TC_Masks to SAMPLERTYPE_Masks and TC_Grayscale-without-sRGB to
    SAMPLERTYPE_LinearGrayscale, and VerifySamplerType rejects any mismatch OUTRIGHT: the
    whole material fails to compile and every surface using it falls back to the engine
    default, while parameter names still enumerate perfectly.
  - A vendor OpenGL-convention normal map needs flip_green_channel, or everything reads as
    lit from the opposite side - wrong in a way that looks entirely plausible.

The textures themselves stay in /Game/Textures, unreferenced. Headless asset deletion
reports success while leaving the asset on disk in this engine, so removing them is a
separate step that has to be verified against the filesystem.

THE INSTANCES ARE WHAT MAKE A SURFACE. M_RunwayTarmac, M_RunwayConcrete and M_RunwayGrass
are instances of this material (build_runway_materials.py), and the band table points slot
0 straight at it (build_road_material_set.py). They set BaseColour, which REPLACED the old
SurfaceTint - a tint multiplied a photo texture, and with no texture underneath there is
nothing for a multiplier to multiply. Both scripts have to be re-run after this one.
"""
import os
import sys
import unreal

# The authoring scripts share a palette and a set of graph recipes. __file__ IS defined
# under -run=pythonscript (verified 2026-09-19), so the sibling directory is reachable.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airside_palette as palette
import airside_matnodes as nodes

TEX_DIR = "/Game/Textures"
MAT_DIR = "/Game/Materials"

# The same tiling noise the ground uses, for the same reason: it is an engine asset, so
# there is no external source directory to go missing, and ground and pavement then break
# up on one shared pattern. NOT T_Default_MacroVariation, which ships on disk but is absent
# from the asset registry, so does_asset_exist and load_asset both deny it.
NOISE_TEXTURE = "/Engine/EngineMaterials/Good64x64TilingNoiseHighFreq"

# --- Tunables, all of them parameters on the material -----------------------------------
#
# Defaults describe TAXIWAY asphalt, because M_RoadSurface is used directly for slot 0 of
# the band table - roads and taxiways - while the runway surfaces are instances that
# override BaseColour. Palette hexes, not floats: see airside_palette.
BASE_COLOUR = palette.TAXIWAY_ASPHALT
# Wear is the SAME family as the base, one step lighter, never a different hue. Asphalt
# weathers paler as the binder oxidises and the aggregate shows through; it does not turn
# brown or green, and a wear tone that shifts hue reads as dirt rather than as age.
# WEAR IS ALMOST INVISIBLE, AND THAT IS THE SETTING. It was 1.49 : 1 against the base in
# linear luminance and read as WET PATCHES - dark blotches on pavement are what damp looks
# like, and the eye names them before it names anything else. A surface that appears to be
# drying in patches is a weather effect nobody asked for, and it is worse than a flat one.
#
# 1.09 : 1 is roughly the threshold of perception across a soft gradient. The wear is meant
# to stop the eye deciding the pavement is a solid fill, NOT to be seen as an area. If it
# can be pointed at, it is too strong.
#
# The window widens with it: a narrow window makes patches WITH EDGES, and an edge is what
# turns a tonal drift into a stain. Four octaves rather than three for the same reason the
# grass needed them - broken-up shapes read as surface, smooth blobs read as objects.
WEAR_COLOUR = "#5D6569"

# 40 m, two orders below the grass's 800 m. A runway is a 45 m ribbon: variation sized for
# the field would put a single tone across the whole width and read as a flat stripe again.
WEAR_SIZE = 40.0
WEAR_LEVELS = 4
# A WIDE window, unlike the grass's 0.46-0.54. Pavement wants a gradient between fresh and
# worn, not patches with edges - a hard edge on tarmac reads as a repair, and a runway
# covered in repairs is a story we are not telling.
WEAR_CONTRAST_LO = 0.18
WEAR_CONTRAST_HI = 0.82

# Grain: metres, and small. This is the octave that replaces the pebble normal map. It is
# value-only, so it costs nothing in hue and cannot fight the markings.
GRAIN_SIZE = 2.5
GRAIN_AMOUNT = 0.10

# Roughness constant per surface, wobbled by the grain. High: asphalt is not glossy, and
# high-and-varying roughness - not geometry detail - is what stops a flat plane showing a
# broad specular sheen band under a soft sun with Lumen.
ROUGHNESS_BASE = 0.82


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def build_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary

    # REBUILT IN PLACE. This script used to delete M_RoadSurface and create a new one,
    # which was survivable only because build_runway_materials.py recreated its three
    # instances afterwards - a pipeline ordering constraint standing in for correctness.
    # A material instance holds a pointer to the parent OBJECT, so deleting it strands
    # every child at the destroyed object rather than repointing them at the new one.
    # nodes.clear_graph empties the graph while the asset keeps its identity; see its
    # docstring for why delete_all_material_expressions is not used to do it.
    path = "%s/M_RoadSurface" % MAT_DIR
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        material = unreal.EditorAssetLibrary.load_asset(path)
        nodes.clear_graph(lib, material, fail)
        unreal.log("MARKER: rebuilding %s in place" % path)
    else:
        material = tools.create_asset(
            "M_RoadSurface", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        unreal.log_error("MARKER: no material at %s" % path)
        return None

    noise_texture = unreal.EditorAssetLibrary.load_asset(NOISE_TEXTURE)
    if noise_texture is None:
        fail("grain texture missing: %s" % NOISE_TEXTURE)
        return None

    # --- The pavement itself: colour, wear, grain. No maps. --------------------------
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

    # --- UV1: markings ---------------------------------------------------------------
    # UV1.X is lateral offset in uu, UV1.Y is distance along the centreline in uu.
    uv1 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -1400, 800)
    uv1.set_editor_property("coordinate_index", 1)

    lateral = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -1200, 800)
    lateral.set_editor_property("r", True)
    lateral.set_editor_property("g", False)
    lateral.set_editor_property("b", False)
    lateral.set_editor_property("a", False)
    lib.connect_material_expressions(uv1, "", lateral, "")

    abs_lateral = lib.create_material_expression(
        material, unreal.MaterialExpressionAbs, -1000, 800)
    lib.connect_material_expressions(lateral, "", abs_lateral, "")

    centre_width = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -1000, 950)
    centre_width.set_editor_property("parameter_name", "CentrelineWidth")
    centre_width.set_editor_property("default_value", 15.0)

    sharpness = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -1000, 1050)
    sharpness.set_editor_property("parameter_name", "MarkingSharpness")
    sharpness.set_editor_property("default_value", 0.5)

    # mask = 1 - saturate((|lateral| - CentrelineWidth) * MarkingSharpness)
    #
    # Deliberately not a MaterialExpressionIf: in 5.8 that node's ConstAGreaterThanB and
    # ConstALessThanB are deprecated and its branches are input pins, so a hard threshold
    # would cost three more constant nodes. This form is fewer nodes AND better, because
    # the edge ramps over 1/MarkingSharpness uu instead of aliasing along the road.
    over = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -800, 800)
    lib.connect_material_expressions(abs_lateral, "", over, "A")
    lib.connect_material_expressions(centre_width, "", over, "B")

    scaled = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -650, 800)
    lib.connect_material_expressions(over, "", scaled, "A")
    lib.connect_material_expressions(sharpness, "", scaled, "B")

    clamped = lib.create_material_expression(
        material, unreal.MaterialExpressionSaturate, -520, 800)
    lib.connect_material_expressions(scaled, "", clamped, "")

    centre_mask = lib.create_material_expression(
        material, unreal.MaterialExpressionOneMinus, -400, 800)
    lib.connect_material_expressions(clamped, "", centre_mask, "")

    # --- junction blend fades the markings out ---------------------------------------
    # From UV2.X, NOT vertex colour. A UDynamicMeshComponent only ignores its colour
    # overlay while ColorOverrideMode is Constant, and assigning any material flips that
    # to None - at which point the converter reads the overlay and the surface stops
    # rendering entirely, with any material. A mask is not colour; it belongs in a UV
    # channel, where nothing about the render path depends on it.
    uv2 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -1000, 1100)
    uv2.set_editor_property("coordinate_index", 2)

    junction_blend = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -800, 1100)
    junction_blend.set_editor_property("r", True)
    junction_blend.set_editor_property("g", False)
    junction_blend.set_editor_property("b", False)
    junction_blend.set_editor_property("a", False)
    lib.connect_material_expressions(uv2, "", junction_blend, "")

    fade = lib.create_material_expression(
        material, unreal.MaterialExpressionOneMinus, -600, 1100)
    lib.connect_material_expressions(junction_blend, "", fade, "")

    marking_amount = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -400, 900)
    lib.connect_material_expressions(centre_mask, "", marking_amount, "A")
    lib.connect_material_expressions(fade, "", marking_amount, "B")

    marking_colour = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -400, 1250)
    marking_colour.set_editor_property("parameter_name", "MarkingColor")
    marking_colour.set_editor_property("default_value", unreal.LinearColor(0.85, 0.72, 0.05, 1.0))

    # --- BaseColour REPLACES SurfaceTint -----------------------------------------------
    #
    # SurfaceTint was a multiplier on a photo albedo: white left the asphalt alone, 1.9
    # lifted it to concrete, 0.55/0.80/0.35 pushed it toward grass. With the albedo gone
    # there is nothing to multiply, so the surfaces state their colour outright instead -
    # which is also the only form in which a palette entry can be diffed against the table
    # it came from.
    #
    # The parameter is consumed ABOVE, before the wear lerp, so wear and grain apply to
    # whatever colour an instance chose. The marking lerp stays LAST: paint is never
    # tinted, weathered or grained, because paint is a different material lying on top and
    # the markings have to stay legible at every camera distance.
    #
    # NOTE FOR ANYONE RE-RUNNING THIS: build_runway_materials.py sets this name. Renaming
    # the parameter without renaming it there leaves three instances overriding a parameter
    # that no longer exists, which is silent - they simply render the default.
    painted = lib.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -150, 0)
    lib.connect_material_expressions(surface, "", painted, "A")
    lib.connect_material_expressions(marking_colour, "", painted, "B")
    lib.connect_material_expressions(marking_amount, "", painted, "Alpha")

    # --- Roughness: a constant per surface, wobbled by the same grain -------------------
    # The SAME signal that breaks up the colour breaks up the sheen, so they agree rather
    # than forming two independent patterns. (grain - 1) is +/- GrainAmount/2; a third of
    # that is the wobble that stops the specular band forming.
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

    # --- parameters declared for later slices ----------------------------------------
    # The world-aligned UVs already carry the texel scale, but a material instance is how
    # 2b-ii and later tune these without a rebuild, so they are declared now.
    for name, default, y in (
        ("TexelsPerUnit", 512.0, 1450),
        ("EdgeLineInset", 40.0, 1550),
        ("EdgeLineWidth", 10.0, 1650),
        ("DashLength", 300.0, 1750),
        ("DashGap", 300.0, 1850),
    ):
        node = lib.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -1400, y)
        node.set_editor_property("parameter_name", name)
        node.set_editor_property("default_value", default)

    # The surface is OPAQUE. UV2.Y once drove a dithered opacity mask that faded the
    # shoulder into the terrain; that is gone. An airport's surfaces meet at hard material
    # lines - concrete slab, asphalt run-off, grass - and a road's edge is a kerb, so the
    # fade solved a problem this game does not have and cost a masked material to do it.
    # Edge treatment becomes a per-band material choice; this material covers one band.
    # NO NORMAL AND NO AMBIENT OCCLUSION, and both omissions are deliberate.
    #
    # The normal map was the single biggest contributor to the photoreal read, and at this
    # game's camera range - 6 m to 600 m, looking down - a pebble normal is invisible past
    # about 30 m and mush in between. Roughness, not geometry detail, is what an untextured
    # plane actually needs.
    #
    # AO is left to Lumen, which already occludes contacts. A baked AO map on top of it
    # double-darkens every crevice, and on a flat plane there are no crevices to darken.
    lib.connect_material_property(painted, "", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset("%s/M_RoadSurface" % MAT_DIR)

    # Parameter names prove nothing about whether the shader compiled - they enumerate
    # expressions either way - and a material with a compile error renders as the engine
    # default while every other signal says success. Report the statistics so the run has
    # some evidence, and scan the log for LogMaterial errors after it.
    # Reported for information only. This commandlet runs under the Null RHI (the log
    # shows NullDrv loading), so there is no shader map for the current feature level and
    # every count comes back zero whether the material is sound or broken. Do NOT treat
    # zero here as a compile failure - it says nothing either way.
    #
    # The check that does work is the absence of material errors in the log. A sampler
    # type that disagrees with its texture's compression setting fails VerifySamplerType,
    # and the whole material then falls back to the engine default at render time while
    # parameter names still enumerate perfectly. After running this, scan for:
    #
    #   Select-String -Path Saved/Logs/AirportMgr.log -Pattern "LogMaterial|Sampler type"
    #
    # Anything there means the material is broken no matter how clean these markers look.
    stats = lib.get_statistics(material)
    unreal.log(
        "MARKER: statistics (zero under the Null RHI, informational only) "
        "vertex=%d pixel=%d samplers=%d"
        % (stats.num_vertex_shader_instructions,
           stats.num_pixel_shader_instructions,
           stats.num_samplers))

    unreal.log("MARKER: material saved at %s/M_RoadSurface" % MAT_DIR)
    for info in lib.get_scalar_parameter_names(material):
        unreal.log("MARKER: scalar parameter %s" % info)
    for info in lib.get_vector_parameter_names(material):
        unreal.log("MARKER: vector parameter %s" % info)
    return material


build_material()
unreal.log("MARKER: done")
