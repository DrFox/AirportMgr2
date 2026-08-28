"""Imports the asphalt PBR set and authors M_RoadSurface. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because
print() goes to the log rather than stdout under the commandlet.
"""
import os
import unreal

SOURCE = r"C:\repos\models\materials\concrete-bl\pebbled-asphalt1-bl"
TEX_DIR = "/Game/RoadNet/Textures"
MAT_DIR = "/Game/RoadNet/Materials"

# name -> (file, is_srgb, compression)
MAPS = {
    "T_Asphalt_Albedo": (
        "pebbled_asphalt_albedo.png", True, unreal.TextureCompressionSettings.TC_DEFAULT),
    "T_Asphalt_Normal": (
        "pebbled_asphalt_Normal-ogl.png", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    "T_Asphalt_Roughness": (
        "pebbled_asphalt_Roughness.png", False, unreal.TextureCompressionSettings.TC_MASKS),
    "T_Asphalt_AO": (
        "pebbled_asphalt_ao.png", False, unreal.TextureCompressionSettings.TC_MASKS),
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
    material = tools.create_asset(
        "M_RoadSurface", MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
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
    vertex_colour = lib.create_material_expression(
        material, unreal.MaterialExpressionVertexColor, -800, 1100)

    fade = lib.create_material_expression(
        material, unreal.MaterialExpressionOneMinus, -600, 1100)
    lib.connect_material_expressions(vertex_colour, "G", fade, "")

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

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset("%s/M_RoadSurface" % MAT_DIR)

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
