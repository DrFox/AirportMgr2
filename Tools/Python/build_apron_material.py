"""Imports the degraded-concrete PBR set and authors M_ApronConcrete. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

An apron is concrete, not asphalt. Beyond realism, distinctness is the point: while the
aprons borrowed M_RoadSurface a new one was very hard to tell from the taxiway lying on it
and from the ground under it, and "hard to tell from the ground" is indistinguishable from
not rendering at all.

One deliberate departure from M_RoadSurface: NO UV1. That material reads UV1 for its
centreline, and an apron's UV1 is zero at every vertex because lateral offset and distance
along a centreline mean nothing for a polygon. Sampling it would paint the whole apron as
one enormous centre marking - not a marking, a bug that happens to be visible.
"""
import os
import unreal

SOURCE = r"C:\repos\models\materials\concrete-bl\degraded-concrete-bl"

TEX_DIR = "/Game/RoadNet/Textures"
MAT_DIR = "/Game/RoadNet/Materials"
MAT_NAME = "M_ApronConcrete"

# name -> (file, is_srgb, compression)
#
# Height and metallic are deliberately not imported. The surface is flat and viewed from
# above so there is no parallax worth paying for, and concrete is a dielectric - a metallic
# map of solid black is a texture sample that can only ever return the constant below it.
MAPS = {
    "T_Concrete_Albedo": (
        "degraded-concrete_albedo.png", True, unreal.TextureCompressionSettings.TC_DEFAULT),
    "T_Concrete_Normal": (
        "degraded-concrete_normal-ogl.png", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    # TC_GRAYSCALE with sRGB off, NOT TC_MASKS. GetSamplerTypeForTexture maps TC_Masks to
    # SAMPLERTYPE_Masks and TC_Grayscale-without-sRGB to SAMPLERTYPE_LinearGrayscale, and
    # VerifySamplerType rejects any mismatch outright: the material then fails to compile
    # and every surface using it silently falls back to the engine default.
    "T_Concrete_Roughness": (
        "degraded-concrete_roughness.png", False, unreal.TextureCompressionSettings.TC_GRAYSCALE),
    "T_Concrete_AO": (
        "degraded-concrete_ao.png", False, unreal.TextureCompressionSettings.TC_GRAYSCALE),
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
        if name == "T_Concrete_Normal":
            # The vendor ships an OpenGL-convention normal map. UE expects DirectX, so the
            # green channel has to be inverted or every lit surface reads as though it is
            # lit from the opposite side - wrong in a way that looks entirely plausible.
            asset.set_editor_property("flip_green_channel", True)

        unreal.EditorAssetLibrary.save_asset("%s/%s" % (TEX_DIR, name))
        imported[name] = asset
        unreal.log("MARKER: imported %s srgb=%s" % (name, srgb))
    return imported


def build_apron_material(textures):
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, MAT_NAME)

    # Rebuilt IN PLACE rather than deleted and re-created.
    #
    # ARoadNetworkActor resolves this material by path in its constructor, so it is loaded
    # before this script runs. delete_asset then reports success while the package stays in
    # memory, and create_asset refuses with "already exists ... cannot ask the user as the
    # application is running unattended" - failing the whole script after the textures have
    # already been imported.
    #
    # Re-creating it would be wrong even if it worked: a new asset is a new object, and
    # every reference to the old one - the actor's included - would be left pointing at a
    # deleted package.
    # Deleted through delete_loaded_asset, not delete_asset.
    #
    # ARoadNetworkActor resolves this material by path in its constructor, so it is already
    # in memory when this script runs. delete_asset then reports success while the package
    # stays loaded and create_asset refuses with "already exists ... cannot ask the user as
    # the application is running unattended". Clearing the graph in place is not the answer
    # either: delete_all_material_expressions asserts !IsRooted() on a material the CDO is
    # holding, and takes the whole commandlet down with it.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    material = tools.create_asset(
        MAT_NAME, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        unreal.log_error(
            "MARKER: create_asset returned None for %s - the asset is still loaded. "
            "Delete the .uasset from disk and re-run." % path)
        return None

    # UV0 is world-aligned XY over the texel scale - a pure function of position - so the
    # concrete is continuous across the join where a taxiway runs onto the apron, for the
    # same reason the asphalt is. Design spec 6.3.
    uv0 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -1300, 0)
    uv0.set_editor_property("coordinate_index", 0)

    # A parameter, so the tiling can be tuned in a material instance without re-running
    # this script or rebuilding a single triangle. Below one means larger slabs.
    tiling = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -1300, 200)
    tiling.set_editor_property("parameter_name", "TileScale")
    tiling.set_editor_property("default_value", 0.5)

    scaled = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -1120, 60)
    lib.connect_material_expressions(uv0, "", scaled, "A")
    lib.connect_material_expressions(tiling, "", scaled, "B")

    def sample(name, y, sampler=None):
        texture = textures.get(name)
        if texture is None:
            return None
        node = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureSample, -900, y)
        node.set_editor_property("texture", texture)
        if sampler is not None:
            node.set_editor_property("sampler_type", sampler)
        lib.connect_material_expressions(scaled, "", node, "UVs")
        return node

    albedo = sample("T_Concrete_Albedo", -300)
    normal = sample("T_Concrete_Normal", 0, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    rough = sample("T_Concrete_Roughness", 320,
                   unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    ao = sample("T_Concrete_AO", 640, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)

    # --- Base colour -----------------------------------------------------------------
    tint = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -600, -560)
    tint.set_editor_property("parameter_name", "ConcreteTint")
    tint.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 1.0, 1.0))

    tinted = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -380, -420)
    lib.connect_material_expressions(tint, "", tinted, "A")
    if albedo is not None:
        lib.connect_material_expressions(albedo, "RGB", tinted, "B")
    else:
        # The grain is a nicety; the colour is not. A missing texture must still leave a
        # material that renders, or a missing asset becomes an invisible apron - the exact
        # failure this material exists to rule out.
        unreal.log_warning("MARKER: no concrete albedo, falling back to flat colour")
        tinted.set_editor_property("const_b", 0.62)

    base = tinted
    if ao is not None:
        occluded = lib.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -180, -380)
        lib.connect_material_expressions(tinted, "", occluded, "A")
        lib.connect_material_expressions(ao, "R", occluded, "B")
        base = occluded

    lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # --- Roughness --------------------------------------------------------------------
    roughness_scale = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -600, 460)
    roughness_scale.set_editor_property("parameter_name", "RoughnessScale")
    roughness_scale.set_editor_property("default_value", 1.0)

    if rough is not None:
        rough_mix = lib.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -300, 380)
        lib.connect_material_expressions(rough, "R", rough_mix, "A")
        lib.connect_material_expressions(roughness_scale, "", rough_mix, "B")
        lib.connect_material_property(rough_mix, "", unreal.MaterialProperty.MP_ROUGHNESS)
    else:
        lib.connect_material_property(
            roughness_scale, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # --- Normal -----------------------------------------------------------------------
    #
    # Safe on this mesh for the same reason it is on the road: the component leaves tangents
    # ExternallyProvided, finds no tangent space, and falls back to a frame derived from the
    # normal alone - which on a flat +Z surface is a constant, valid basis. It would NOT be
    # safe under AutoCalculated, which derives the frame from the UV layers and hits this
    # mesh's degenerate UV1, producing NaN tangents the GPU silently discards.
    if normal is not None:
        lib.connect_material_property(normal, "", unreal.MaterialProperty.MP_NORMAL)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("MARKER: %s built and saved" % path)
    return material


build_apron_material(import_textures())
