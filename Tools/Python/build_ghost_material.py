"""Authors M_RoadGhost, the translucent preview material. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because
print() goes to the log rather than stdout under the commandlet.

Design spec 6.6 asks for one translucent unlit material driven by parameters, so that
one junction can read as illegal while the rest of a drag stays valid with no mesh
regeneration. Two deliberate departures from what that section specifies:

  - EdgeGlow is NOT a Fresnel rim. Fresnel is dot(normal, view), and the road is a flat
    +Z surface viewed from straight above through the build camera - that dot product is
    1 everywhere, so a Fresnel rim would be uniformly zero across the entire ghost. The
    edge is found in UV1.X instead, which the mesh builder fills with lateral offset
    across the profile in uu: the rim is the band within EdgeWidth of the road's own
    edge, which is the same visual idea by the only means that works top-down.

  - No animated ScanSpeed stripe. It is decoration, and it needs a time-varying pan on
    UV1.Y that would have to be authored against each profile's length parameterisation.
"""
import unreal

MAT_DIR = "/Game/Materials"
MAT_NAME = "M_RoadGhost"


def build_ghost_material():
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, MAT_NAME)

    # Rebuilt IN PLACE rather than deleted and re-created. ARoadNetworkActor resolves this
    # material by path in its constructor, so it is already loaded when this script runs;
    # delete_asset then reports success while the package stays in memory and create_asset
    # refuses with "already exists ... cannot ask the user as the application is running
    # unattended". Re-creating would also strand every existing reference to it.
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

    # Unlit because a preview must read the same regardless of time of day or what it is
    # hovering over, and because the road mesh has no usable tangent frame - see the note
    # in ARoadNetworkActor's constructor about NaN tangents killing every lit material.
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)

    # --- Parameters ------------------------------------------------------------------
    base = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -900, -300)
    base.set_editor_property("parameter_name", "BaseColor")
    base.set_editor_property("default_value", unreal.LinearColor(0.1, 0.75, 1.0, 1.0))

    invalid = lib.create_material_expression(
        material, unreal.MaterialExpressionVectorParameter, -900, -120)
    invalid.set_editor_property("parameter_name", "InvalidColor")
    invalid.set_editor_property("default_value", unreal.LinearColor(1.0, 0.12, 0.1, 1.0))

    validity = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 60)
    validity.set_editor_property("parameter_name", "ValidityBlend")
    validity.set_editor_property("default_value", 0.0)

    glow = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 180)
    glow.set_editor_property("parameter_name", "EdgeGlow")
    glow.set_editor_property("default_value", 1.6)

    # Set from C++ per update: the ghost's own profile decides where its edge is, and a
    # material cannot know that. Left at a default a narrow road would glow edge to edge
    # and a wide one not at all.
    half_width = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 300)
    half_width.set_editor_property("parameter_name", "EdgeHalfWidth")
    half_width.set_editor_property("default_value", 100.0)

    edge_width = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 420)
    edge_width.set_editor_property("parameter_name", "EdgeWidth")
    edge_width.set_editor_property("default_value", 25.0)

    body_opacity = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 700)
    body_opacity.set_editor_property("parameter_name", "BodyOpacity")
    body_opacity.set_editor_property("default_value", 0.42)

    edge_opacity = lib.create_material_expression(
        material, unreal.MaterialExpressionScalarParameter, -900, 820)
    edge_opacity.set_editor_property("parameter_name", "EdgeOpacity")
    edge_opacity.set_editor_property("default_value", 0.95)

    # --- The rim, from UV1.X ---------------------------------------------------------
    # UV1.X is the lateral offset across the profile in uu, signed from the centreline,
    # so abs() of it is the distance out towards either edge.
    uv1 = lib.create_material_expression(
        material, unreal.MaterialExpressionTextureCoordinate, -700, 500)
    uv1.set_editor_property("coordinate_index", 1)

    lateral = lib.create_material_expression(
        material, unreal.MaterialExpressionComponentMask, -560, 500)
    lateral.set_editor_property("r", True)
    lateral.set_editor_property("g", False)
    lateral.set_editor_property("b", False)
    lateral.set_editor_property("a", False)
    lib.connect_material_expressions(uv1, "", lateral, "")

    lateral_abs = lib.create_material_expression(
        material, unreal.MaterialExpressionAbs, -430, 500)
    lib.connect_material_expressions(lateral, "", lateral_abs, "")

    # Where the rim starts: EdgeWidth inboard of the edge.
    inner = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -430, 340)
    lib.connect_material_expressions(half_width, "", inner, "A")
    lib.connect_material_expressions(edge_width, "", inner, "B")

    # saturate((|lateral| - inner) / EdgeWidth), written out of Subtract/Divide/Saturate
    # rather than SmoothStep so it depends only on expression classes that have been
    # stable for many versions. A linear ramp is enough for a 25 uu band.
    over = lib.create_material_expression(
        material, unreal.MaterialExpressionSubtract, -280, 440)
    lib.connect_material_expressions(lateral_abs, "", over, "A")
    lib.connect_material_expressions(inner, "", over, "B")

    # Guarded against a zero EdgeWidth, which would otherwise divide by zero and make the
    # whole ghost NaN - and a NaN vertex is silently discarded by the GPU rather than
    # drawn wrong, so the ghost would simply never appear.
    safe_width = lib.create_material_expression(
        material, unreal.MaterialExpressionMax, -280, 580)
    lib.connect_material_expressions(edge_width, "", safe_width, "A")
    safe_width.set_editor_property("const_b", 1.0)

    ramp = lib.create_material_expression(
        material, unreal.MaterialExpressionDivide, -140, 480)
    lib.connect_material_expressions(over, "", ramp, "A")
    lib.connect_material_expressions(safe_width, "", ramp, "B")

    rim = lib.create_material_expression(
        material, unreal.MaterialExpressionSaturate, -10, 480)
    lib.connect_material_expressions(ramp, "", rim, "")

    # --- Colour ----------------------------------------------------------------------
    # Validity is a parameter rather than a mesh variant, so a drag can be recoloured
    # without regenerating a single triangle.
    tint = lib.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, -560, -200)
    lib.connect_material_expressions(base, "", tint, "A")
    lib.connect_material_expressions(invalid, "", tint, "B")
    lib.connect_material_expressions(validity, "", tint, "Alpha")

    glow_amount = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, 130, 300)
    lib.connect_material_expressions(rim, "", glow_amount, "A")
    lib.connect_material_expressions(glow, "", glow_amount, "B")

    brightness = lib.create_material_expression(
        material, unreal.MaterialExpressionAdd, 270, 200)
    brightness.set_editor_property("const_a", 1.0)
    lib.connect_material_expressions(glow_amount, "", brightness, "B")

    emissive = lib.create_material_expression(
        material, unreal.MaterialExpressionMultiply, 410, -100)
    lib.connect_material_expressions(tint, "", emissive, "A")
    lib.connect_material_expressions(brightness, "", emissive, "B")

    opacity = lib.create_material_expression(
        material, unreal.MaterialExpressionLinearInterpolate, 410, 700)
    lib.connect_material_expressions(body_opacity, "", opacity, "A")
    lib.connect_material_expressions(edge_opacity, "", opacity, "B")
    lib.connect_material_expressions(rim, "", opacity, "Alpha")

    lib.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("MARKER: %s built and saved" % path)
    return material


build_ghost_material()
