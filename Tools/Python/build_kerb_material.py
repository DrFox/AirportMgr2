"""Authors M_RoadKerb. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

The third slot of the per-band material set. A kerb is cast concrete: it reuses the
textures build_apron_material.py already imported rather than importing its own, because
they are the same material in the world and a second copy would be two assets that must be
kept in step by hand.

It is NOT M_ApronConcrete with a tint, and deliberately not a material instance of it: the
apron material's parameters are tuned for a large slab viewed from above, and a kerb is a
narrow strip seen edge-on at the road's rim. Sharing the asset would make every future
apron tweak a kerb tweak.

NO MARKINGS, and that is the point of the slot existing. M_RoadSurface paints its
centreline and edge lines from UV1, which is defined across the whole profile - so a kerb
band skinned with the road material inherits an edge line running down a surface that
should only ever be plain concrete.
"""
import unreal

TEX_DIR = "/Game/Textures"
MAT_DIR = "/Game/Materials"
MAT_NAME = "M_RoadKerb"

# Reused, not re-imported. build_apron_material.py owns the import and its settings - the
# sRGB and compression choices there fail silently when wrong, so there must be one place
# that gets them right rather than two that agree today.
TEXTURES = {
    "albedo": "T_Concrete_Albedo",
    "normal": "T_Concrete_Normal",
    "rough": "T_Concrete_Roughness",
    "ao": "T_Concrete_AO",
}


def load_textures():
    loaded = {}
    for key, name in TEXTURES.items():
        asset = unreal.load_asset("%s/%s" % (TEX_DIR, name))
        if asset is None:
            unreal.log_warning(
                "MARKER: %s not found - run build_apron_material.py first" % name)
            continue
        loaded[key] = asset
    return loaded


def build_kerb_material(textures):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, MAT_NAME)

    # Rebuilt in place, deleted through delete_loaded_asset. See build_apron_material.py:
    # a material referenced from C++ is already in memory when this runs, and delete_asset
    # reports success while the package stays loaded, after which create_asset refuses.
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

    # World-aligned UV0 over a texel scale, exactly as the road and apron do it, so a kerb
    # is continuous with the surfaces either side of it rather than restarting its tiling
    # at every band boundary. Design spec 6.3.
    uv0 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -1300, 0)
    uv0.set_editor_property("coordinate_index", 0)

    # Tighter than the apron's 0.5: a kerb is a narrow band, and slab-sized aggregate on it
    # reads as a blur rather than as concrete.
    tiling = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -1300, 200)
    tiling.set_editor_property("parameter_name", "TileScale")
    tiling.set_editor_property("default_value", 2.0)

    scaled = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -1120, 60)
    lib.connect_material_expressions(uv0, "", scaled, "A")
    lib.connect_material_expressions(tiling, "", scaled, "B")

    def sample(key, y, sampler=None):
        texture = textures.get(key)
        if texture is None:
            return None
        node = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureSample, -900, y)
        node.set_editor_property("texture", texture)
        if sampler is not None:
            node.set_editor_property("sampler_type", sampler)
        lib.connect_material_expressions(scaled, "", node, "UVs")
        return node

    albedo = sample("albedo", -300)
    normal = sample("normal", 0, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    rough = sample("rough", 320, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
    ao = sample("ao", 640, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)

    # --- Base colour -----------------------------------------------------------------
    #
    # Darker than the apron's 1.0 tint. A kerb has to read as a distinct band against the
    # lane beside it from a normal camera height; at the same value as the apron the band
    # subdivision is invisible and this whole slice looks like it did nothing.
    tint = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -600, -560)
    tint.set_editor_property("parameter_name", "KerbTint")
    tint.set_editor_property("default_value", unreal.LinearColor(0.55, 0.55, 0.57, 1.0))

    tinted = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -380, -420)
    lib.connect_material_expressions(tint, "", tinted, "A")
    if albedo is not None:
        lib.connect_material_expressions(albedo, "RGB", tinted, "B")
    else:
        # A missing texture must still leave a material that renders. An invisible kerb is
        # indistinguishable from the band subdivision not working at all.
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
    #
    # Rougher than the apron. Cast kerb concrete is unpolished and never gets the tyre
    # burnishing that flattens an apron's roughness where aircraft actually roll.
    roughness_scale = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -600, 460)
    roughness_scale.set_editor_property("parameter_name", "RoughnessScale")
    roughness_scale.set_editor_property("default_value", 1.15)

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
    # Safe for the same reason as the apron's: the component leaves tangents
    # ExternallyProvided and falls back to a frame derived from the normal, which on a flat
    # +Z surface is a constant valid basis.
    if normal is not None:
        lib.connect_material_property(normal, "", unreal.MaterialProperty.MP_NORMAL)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("MARKER: %s built and saved" % path)
    return material


build_kerb_material(load_textures())
