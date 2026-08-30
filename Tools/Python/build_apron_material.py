"""Authors M_ApronConcrete, the apron surface material. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

An apron is concrete, not asphalt, and until now it borrowed M_RoadSurface - which made a
newly drawn apron very hard to tell from the taxiway lying on it and from the ground under
it. Distinctness is the point of this material as much as realism is.

Two deliberate departures from M_RoadSurface:

  NO normal map. The road's works only because its component supplies tangents externally
  and the surface is flat +Z; an apron needs no relief and skipping it removes the whole
  tangent-frame question rather than relying on it staying answered.

  NO UV1. M_RoadSurface reads UV1 for its centreline, and an apron's UV1 is zero at every
  vertex because lateral offset and distance-along-a-centreline mean nothing for a polygon.
  A material sampling that would paint the ENTIRE apron as centreline, which is not a
  marking - it is a bug that happens to be visible.
"""
import unreal

TEX_DIR = "/Game/RoadNet/Textures"
MAT_DIR = "/Game/RoadNet/Materials"
MAT_NAME = "M_ApronConcrete"


def build_apron_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    # create_asset returns None rather than raising when the asset already exists, and the
    # next call then fails with an unrelated AttributeError on NoneType. Authoring gets
    # iterated, so delete and rebuild.
    path = "%s/%s" % (MAT_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    material = tools.create_asset(
        MAT_NAME, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        unreal.log_error("MARKER: create_asset returned None for %s" % path)
        return None

    lib = unreal.MaterialEditingLibrary

    albedo_texture = unreal.EditorAssetLibrary.load_asset("%s/T_Asphalt_Albedo" % TEX_DIR)
    rough_texture = unreal.EditorAssetLibrary.load_asset("%s/T_Asphalt_Roughness" % TEX_DIR)

    # UV0 is world-aligned XY over the texel scale - a pure function of position - so the
    # concrete is continuous across the join where a taxiway runs onto the apron, for the
    # same reason the asphalt is.
    uv0 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -900, 0)
    uv0.set_editor_property("coordinate_index", 0)

    # Slabs are large. Tiled coarser than the road so the two surfaces do not read as the
    # same material at a different brightness.
    scale = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -740, 0)
    lib.connect_material_expressions(uv0, "", scale, "A")
    scale.set_editor_property("const_b", 0.35)

    tint = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -560, -260)
    tint.set_editor_property("parameter_name", "ConcreteTint")
    tint.set_editor_property("default_value", unreal.LinearColor(0.62, 0.62, 0.60, 1.0))

    base = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, -300, -140)
    lib.connect_material_expressions(tint, "", base, "A")

    if albedo_texture is not None:
        albedo = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureSample, -560, -60)
        albedo.set_editor_property("texture", albedo_texture)
        lib.connect_material_expressions(scale, "", albedo, "UVs")
        lib.connect_material_expressions(albedo, "RGB", base, "B")
    else:
        # The grain is a nicety; the colour is not. A missing texture must still leave a
        # material that renders, or a missing asset becomes an invisible apron - which is
        # the exact failure this material was written to rule out.
        unreal.log_warning("MARKER: no asphalt albedo found, concrete will be flat colour")
        base.set_editor_property("const_b", 1.0)

    lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    roughness = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -300, 260)
    roughness.set_editor_property("parameter_name", "Roughness")
    roughness.set_editor_property("default_value", 0.88)

    if rough_texture is not None:
        rough = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureSample, -560, 200)
        rough.set_editor_property("texture", rough_texture)
        rough.set_editor_property(
            "sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_GRAYSCALE)
        lib.connect_material_expressions(scale, "", rough, "UVs")

        rough_mix = lib.create_material_expression(
            material, unreal.MaterialExpressionMultiply, -140, 220)
        lib.connect_material_expressions(rough, "R", rough_mix, "A")
        lib.connect_material_expressions(roughness, "", rough_mix, "B")
        lib.connect_material_property(rough_mix, "", unreal.MaterialProperty.MP_ROUGHNESS)
    else:
        lib.connect_material_property(roughness, "", unreal.MaterialProperty.MP_ROUGHNESS)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("MARKER: %s built and saved" % path)
    return material


build_apron_material()
