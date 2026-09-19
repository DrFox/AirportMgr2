"""Material-graph helpers shared by the authoring scripts.

These were written twice before this module existed - once in build_ground_material.py and
again for the pavement - and the recipes here are the ones that have been measured on
screen. The trap each one avoids is named at the function that avoids it, because every one
of them produces a material that compiles, saves and renders, just wrong.

build_ground_material.py still carries its own copies. It is NOT retrofitted, on the same
ruling that governs bevels on the existing models: it works, it is verified on screen, and
re-proving it buys less than the next script does. It migrates the next time it is opened
for another reason.
"""
import unreal


def scalar(lib, mat, name, default, x, y):
    """A named ScalarParameter - the unit of "tunable on a material instance"."""
    node = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node


def vector(lib, mat, name, default, x, y):
    """A named VectorParameter. `default` is a palette hex string or an unreal.LinearColor."""
    if isinstance(default, str):
        import airside_palette
        default = airside_palette.linear_color(default)
    node = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", default)
    return node


def noise_position(lib, mat, size_param, x, y):
    """World position divided by a size given in METRES - the input every noise here takes.

    Size in metres, not a UV scale, because a parameter reading 0.000029 cannot be judged in
    the editor and a parameter that cannot be judged never gets tuned.

    World position rather than a TextureCoordinate: it survives a resize, and it makes the
    ground and the pavement lying on it break up on the SAME pattern rather than each on one
    of its own."""
    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, x, y)
    cm = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x + 200, y + 120)
    cm.set_editor_property("const_b", 100.0)   # metres -> uu
    lib.connect_material_expressions(size_param, "", cm, "A")
    pos = lib.create_material_expression(mat, unreal.MaterialExpressionDivide, x + 400, y)
    lib.connect_material_expressions(world, "", pos, "A")
    lib.connect_material_expressions(cm, "", pos, "B")
    return pos


def gradient_noise(lib, mat, size_param, levels, x, y, on_fail=None):
    """Low-frequency fractal gradient noise, 0..1, at a size given in metres.

    GradientALU, not GradientTex: the texture-based function reads an 8-bit lookup table,
    and stretching 256 quantised levels over a region hundreds of metres wide lays a net of
    dotted contour lines across the whole surface. Measured on the grass, 2026-09-19.
    Raising Quality does not help - it samples the same quantised table more often.

    THE POSITION PIN IS ADDRESSED AS "", NOT "Position". MaterialEditingLibrary.cpp:46-70
    resolves an input by comparing against GetInputName(index); UMaterialExpressionNoise
    does not override it, so its inputs are nameless and "Position" matches NOTHING. The
    call returns false and the node then evaluates at a constant position, which renders as
    a flat surface rather than as an error. That is why the return value is checked here."""
    pos = noise_position(lib, mat, size_param, x, y)
    noise = lib.create_material_expression(mat, unreal.MaterialExpressionNoise, x + 650, y)
    noise.set_editor_property("noise_function", unreal.NoiseFunction.NOISEFUNCTION_GRADIENT_ALU)
    noise.set_editor_property("scale", 1.0)
    noise.set_editor_property("quality", 2)
    noise.set_editor_property("levels", levels)
    noise.set_editor_property("output_min", 0.0)
    noise.set_editor_property("output_max", 1.0)
    noise.set_editor_property("turbulence", False)
    if not lib.connect_material_expressions(pos, "", noise, "") and on_fail:
        on_fail("could not wire Position on the noise at (%d,%d)" % (x, y))
    return noise


def contrast_window(lib, mat, source, source_pin, lo_param, hi_param, x, y):
    """smoothstep(lo, hi, n) - what turns a smooth noise into PATCHES rather than a wash.

    Without it, a lerp driven by noise puts every intermediate tone everywhere and averages
    back to one colour at any distance. Narrow the window for defined edges, widen it for a
    gentle gradient. A fractal sum concentrates toward its midpoint, and the more octaves
    the tighter, so this window wants narrowing whenever the level count goes up."""
    step = lib.create_material_expression(mat, unreal.MaterialExpressionSmoothStep, x, y)
    lib.connect_material_expressions(lo_param, "", step, "Min")
    lib.connect_material_expressions(hi_param, "", step, "Max")
    lib.connect_material_expressions(source, source_pin, step, "Value")
    return step


def value_grain(lib, mat, texture, size_param, amount_param, x, y):
    """1 +/- amount/2 of fine value-only break-up, from a tiling noise texture.

    A TEXTURE and not another Noise node, because this octave is metres rather than hundreds
    of metres and a texture fetch is the cheaper way to get detail at that rate.

    Value-only on purpose: its job is to stop a flat plane showing a broad specular sheen
    band under a soft sun, which is the actual failure mode of an untextured surface - not
    flatness. It cannot make a colour region and is not asked to."""
    pos = noise_position(lib, mat, size_param, x, y)
    uv = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, x + 600, y)
    uv.set_editor_property("r", True)
    uv.set_editor_property("g", True)
    uv.set_editor_property("b", False)
    uv.set_editor_property("a", False)
    lib.connect_material_expressions(pos, "", uv, "")

    sample = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, x + 780, y)
    sample.set_editor_property("texture", texture)
    # Shared: Wrap, so sampling this texture more than once does not burn a sampler slot
    # each time. Samplers are a hard limit, and a material that exceeds it fails to compile
    # while every authoring signal still reports success.
    sample.set_editor_property(
        "sampler_source", unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
    lib.connect_material_expressions(uv, "", sample, "UVs")

    dev = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, x + 960, y)
    dev.set_editor_property("const_b", 0.5)
    lib.connect_material_expressions(sample, "R", dev, "A")

    amp = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x + 1120, y)
    lib.connect_material_expressions(dev, "", amp, "A")
    lib.connect_material_expressions(amount_param, "", amp, "B")

    out = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, x + 1280, y)
    out.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(amp, "", out, "A")
    return out


def clear_graph(lib, mat, on_fail=None):
    """Empty a material's node graph so it can be rebuilt IN PLACE.

    DO NOT USE delete_all_material_expressions. It is broken in 5.8 and deletes roughly HALF
    the graph: MaterialEditingLibrary.cpp:559-568 range-iterates Material->GetExpressions()
    while DeleteMaterialExpression removes entries from that same array, so every second
    element is stepped over. The result compiles and renders, just wrong.

    In place, and not delete-and-recreate, because every material instance parented to this
    material holds a pointer to the OBJECT. Deleting it strands them: they keep pointing at
    the destroyed object rather than at the new one with the same path. M_RoadSurface has at
    least three such children."""
    for expression in list(lib.get_material_expressions(mat)):
        lib.delete_material_expression(mat, expression)
    left = len(lib.get_material_expressions(mat))
    if left and on_fail:
        on_fail("graph not empty after clear: %d expressions remain" % left)
    return left == 0
