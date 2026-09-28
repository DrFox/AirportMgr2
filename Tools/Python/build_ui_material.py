"""Authors M_UI_Rounded, the one UI material every rounded, gradient-shaded control draws with.
Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

WHY A MATERIAL AND NOT FSlateRoundedBoxBrush: the rounded box shader draws one flat colour.
The gradient needs a material, and once a material draws the fill it must draw the corners too,
because Slate does not clip a material to a rounded shape.

WHY IT NEEDS NO SIZE PARAMETER: Slate hands every material two things per vertex
(Engine/Shaders/Private/SlateVertexShader.usf, UVArrays): uv channel 3 is the element's size
in SCREEN PIXELS, uv channel 4 is the vertex's position across the element, 0..1. Their
product is the pixel position, so one material draws exact-radius corners at any size. The
9-slice alternative (DrawAs Box, margin 0.5) keeps corners but squeezes the gradient into the
corner bands.

TINT IS APPLIED BY SLATE, NOT HERE: SlateElementPixelShader.usf GetColor does
`FinalColor = BaseColor * InVertex.Color` for every material element, so this outputs only a
grey SHADE and a MASK. Multiplying by VertexColor here too squared the tint - F4BA38 drew as
orange and every grey a step darker (spike PIE 2026-09-28: blue 0.0395 squared encodes to 5,
exactly what the capture read).
"""
import unreal

MAT_DIR = "/Game/UI"
MAT_NAME = "M_UI_Rounded"
EXPECTED_EXPRESSIONS = 7

lib = unreal.MaterialEditingLibrary


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def connect(src, src_pin, dst, dst_pin):
    # connect_material_expressions returns False on a pin name that matches nothing, and the
    # material still compiles - wrong. Honoured at every call.
    if not lib.connect_material_expressions(src, src_pin, dst, dst_pin):
        fail("could not connect %s.%s -> %s.%s" % (src.get_name(), src_pin, dst.get_name(), dst_pin))


def custom(material, x, y, name, code, inputs, output_type):
    node = lib.create_material_expression(material, unreal.MaterialExpressionCustom, x, y)
    node.set_editor_property("description", name)
    node.set_editor_property("code", code)
    node.set_editor_property("output_type", output_type)
    # unreal.CustomInput(input_name=...) raises "call() takes at most 0 arguments".
    pins = []
    for n in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", n)
        pins.append(pin)
    node.set_editor_property("inputs", pins)
    return node


def scalar(material, x, y, name, value):
    node = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", value)
    return node


def run():
    path = "%s/%s" % (MAT_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        say("replaced existing %s" % path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(MAT_NAME, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        fail("create_asset returned None for %s" % path)
        say("DONE")
        return

    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_UI)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    uv_norm = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, -100)
    uv_norm.set_editor_property("coordinate_index", 4)
    uv_px = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -800, 20)
    uv_px.set_editor_property("coordinate_index", 3)
    radius = scalar(material, -800, 300, "RadiusPx", 5.0)
    top = scalar(material, -800, 400, "TopShade", 1.0)
    bottom = scalar(material, -800, 500, "BottomShade", 0.92)

    shade = custom(material, -400, -100, "Shade",
                   "return lerp(TopShade, BottomShade, saturate(UVn.y)).xxx;",
                   ["UVn", "TopShade", "BottomShade"],
                   unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    connect(uv_norm, "", shade, "UVn")
    connect(top, "", shade, "TopShade")
    connect(bottom, "", shade, "BottomShade")

    # Rounded-rectangle signed distance in pixels; 0.5 - d is a one-pixel antialiased edge.
    # Radius clamped to half the short side, so a small chip becomes a pill, not garbage.
    mask = custom(material, -400, 200, "RoundedMask",
                  "float2 h = Px * 0.5;\n"
                  "float r = min(Radius, min(h.x, h.y));\n"
                  "float2 q = abs(UVn * Px - h) - (h - r);\n"
                  "float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
                  "return saturate(0.5 - d);",
                  ["UVn", "Px", "Radius"],
                  unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    connect(uv_norm, "", mask, "UVn")
    connect(uv_px, "", mask, "Px")
    connect(radius, "", mask, "Radius")

    if not lib.connect_material_property(shade, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
        fail("final colour not connected")
    if not lib.connect_material_property(mask, "", unreal.MaterialProperty.MP_OPACITY):
        fail("opacity not connected")

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    n = lib.get_num_material_expressions(reloaded)
    if n != EXPECTED_EXPRESSIONS:
        fail("expected %d expressions after reload, found %d" % (EXPECTED_EXPRESSIONS, n))
    else:
        say("PASS %s saved with %d expressions" % (path, n))
    if reloaded.get_editor_property("material_domain") != unreal.MaterialDomain.MD_UI:
        fail("domain is not UI after reload")
    else:
        say("PASS domain is UI")
    say("DONE")


run()
