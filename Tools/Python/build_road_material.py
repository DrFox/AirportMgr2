"""Imports the asphalt PBR set and authors M_RoadSurface. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because
print() goes to the log rather than stdout under the commandlet.
"""
import os
import unreal

SOURCE = r"C:\repos\models\materials\concrete-bl\pebbled-asphalt1-bl"
# Build only albedo -> BaseColor, nothing else. A baseline that must render before any
# of the rest is worth debugging: ColorOverrideMode was forcing the engine's
# vertex-colour debug material over ours, so M_RoadSurface had never actually been
# drawn and no part of this graph had ever been exercised.
MINIMAL = True

TEX_DIR = "/Game/RoadNet/Textures"
MAT_DIR = "/Game/RoadNet/Materials"

# name -> (file, is_srgb, compression)
MAPS = {
    "T_Asphalt_Albedo": (
        "pebbled_asphalt_albedo.png", True, unreal.TextureCompressionSettings.TC_DEFAULT),
    "T_Asphalt_Normal": (
        "pebbled_asphalt_Normal-ogl.png", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    # TC_GRAYSCALE with sRGB off, NOT TC_MASKS. GetSamplerTypeForTexture maps TC_Masks to
    # SAMPLERTYPE_Masks and TC_Grayscale-without-sRGB to SAMPLERTYPE_LinearGrayscale, and
    # VerifySamplerType rejects any mismatch outright: the whole material then fails to
    # compile and every surface using it silently falls back to the engine default. These
    # are single-channel maps, so grayscale is also what they actually are.
    "T_Asphalt_Roughness": (
        "pebbled_asphalt_Roughness.png", False, unreal.TextureCompressionSettings.TC_GRAYSCALE),
    "T_Asphalt_AO": (
        "pebbled_asphalt_ao.png", False, unreal.TextureCompressionSettings.TC_GRAYSCALE),
}


def import_textures():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    imported = {}
    for name, (filename, srgb, compression) in MAPS.items():
        path = os.path.join(SOURCE, filename)
        if not os.path.isfile(path):
            unreal.log_error("MARKER: missing source texture %s" % path)
            continue

        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = TEX_DIR
        task.destination_name = name
        task.automated = True
        task.replace_existing = True
        task.save = True
        tools.import_asset_tasks([task])

        asset = unreal.load_asset("%s/%s" % (TEX_DIR, name))
        if asset is None:
            unreal.log_error("MARKER: import failed for %s" % name)
            continue

        asset.set_editor_property("srgb", srgb)
        asset.set_editor_property("compression_settings", compression)
        if name == "T_Asphalt_Normal":
            # The vendor ships an OpenGL-convention normal map. UE expects DirectX, so the
            # green channel has to be inverted or every lit surface reads as though it is
            # lit from the opposite side - wrong in a way that looks entirely plausible.
            asset.set_editor_property("flip_green_channel", True)
        unreal.EditorAssetLibrary.save_asset("%s/%s" % (TEX_DIR, name))
        imported[name] = asset
        unreal.log("MARKER: imported %s srgb=%s" % (name, srgb))
    return imported


def build_material(textures):
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    # create_asset returns None rather than raising when the asset already exists, and
    # the very next call then fails with an unrelated AttributeError on NoneType. This
    # script has to be re-runnable - it is the authoring step, and authoring gets
    # iterated - so delete first and rebuild from scratch.
    path = "%s/M_RoadSurface" % MAT_DIR
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    material = tools.create_asset(
        "M_RoadSurface", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        unreal.log_error("MARKER: create_asset returned None for %s" % path)
        return None

    lib = unreal.MaterialEditingLibrary

    # --- UV0: world-aligned asphalt -------------------------------------------------
    uv0 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -1200, 0)
    uv0.set_editor_property("coordinate_index", 0)

    albedo = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, -900, -200)
    albedo.set_editor_property("texture", textures["T_Asphalt_Albedo"])
    lib.connect_material_expressions(uv0, "", albedo, "UVs")

    normal = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, -900, 150)
    normal.set_editor_property("texture", textures["T_Asphalt_Normal"])
    normal.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    lib.connect_material_expressions(uv0, "", normal, "UVs")

    rough = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, -900, 500)
    rough.set_editor_property("texture", textures["T_Asphalt_Roughness"])
    rough.set_editor_property(
        "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    lib.connect_material_expressions(uv0, "", rough, "UVs")

    ao = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureSample, -900, 850)
    ao.set_editor_property("texture", textures["T_Asphalt_AO"])
    ao.set_editor_property(
        "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    lib.connect_material_expressions(uv0, "", ao, "UVs")

    if MINIMAL:
        # Albedo straight into base colour. No UV1, no masks, no parameters - if even
        # this does not render, the fault is not in the marking maths.
        lib.connect_material_property(albedo, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
        lib.recompile_material(material)
        unreal.EditorAssetLibrary.save_asset("%s/M_RoadSurface" % MAT_DIR)
        unreal.log("MARKER: MINIMAL material saved - albedo -> BaseColor only")
        return material

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

    base_colour = lib.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -150, 0)
    lib.connect_material_expressions(albedo, "RGB", base_colour, "A")
    lib.connect_material_expressions(marking_colour, "", base_colour, "B")
    lib.connect_material_expressions(marking_amount, "", base_colour, "Alpha")

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

    lib.connect_material_property(base_colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(normal, "RGB", unreal.MaterialProperty.MP_NORMAL)
    lib.connect_material_property(rough, "R", unreal.MaterialProperty.MP_ROUGHNESS)
    lib.connect_material_property(ao, "R", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)

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


TEXTURES = import_textures()
if len(TEXTURES) == len(MAPS):
    build_material(TEXTURES)
    unreal.log("MARKER: done")
else:
    unreal.log_error(
        "MARKER: aborted, %d of %d textures imported" % (len(TEXTURES), len(MAPS)))
