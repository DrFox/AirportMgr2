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


# THE GRASS SPECKLE, ONE SET OF NUMBERS FOR EVERY GRASS SURFACE (2026-09-30). The grass
# strips (M_RunwayGrass, build_runway_materials.py) and the field (MI_Ground's parent,
# build_ground_material.py) both read this. Two copies drifted would put a visible seam at
# every strip edge; one dict cannot. close_speckle samples world position, so equal numbers
# also mean the SAME pattern either side of the edge. Tuning history: build_runway_materials.
GRASS_SPECKLE = {
    "SpeckleAmount": 0.4,
    "SpeckleSize": 3.5,
    "SpeckleFadeStart": 30.0,
    "SpeckleFadeEnd": 100.0,
}

# Grass specular. A lawn has no sheen, and a sheen band across a flat green plane under a
# low sun is the strongest "plastic" cue there is. Engine default is 0.5.
GRASS_SPECULAR = 0.2


def close_speckle(lib, mat, texture, size_param, amount_param, fade_start_param, fade_end_param,
                  x, y):
    """A fine two-octave value dappling that fades out with camera depth. A MULTIPLIER, 1 at
    and beyond fade end - and everywhere when amount is 0 - so it is safe to multiply into
    any colour.

        speckle = lerp(grainA * grainB, 1, saturate((depth_m - start) / max(end - start, 0.01)))

    Faded by depth because a centimetre pattern seen from hundreds of metres is not detail
    but shimmer, and the top-down look was approved without it. The second octave is 0.37x
    the first - a non-integer ratio, so the two tilings never line up and the 64 px tile's
    repeat stays hidden near the ground."""
    speckle_a = value_grain(lib, mat, texture, size_param, amount_param, x, y)
    fine_size = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x - 100, y + 160)
    fine_size.set_editor_property("const_b", 0.37)
    lib.connect_material_expressions(size_param, "", fine_size, "A")
    speckle_b = value_grain(lib, mat, texture, fine_size, amount_param, x, y + 160)
    both = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, x + 1400, y + 50)
    lib.connect_material_expressions(speckle_a, "", both, "A")
    lib.connect_material_expressions(speckle_b, "", both, "B")

    depth = lib.create_material_expression(mat, unreal.MaterialExpressionPixelDepth, x + 900, y - 280)
    depth_m = lib.create_material_expression(mat, unreal.MaterialExpressionDivide, x + 1050, y - 280)
    depth_m.set_editor_property("const_b", 100.0)   # uu -> metres
    lib.connect_material_expressions(depth, "", depth_m, "A")
    past_start = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, x + 1200, y - 250)
    lib.connect_material_expressions(depth_m, "", past_start, "A")
    lib.connect_material_expressions(fade_start_param, "", past_start, "B")
    span = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, x + 1050, y - 150)
    lib.connect_material_expressions(fade_end_param, "", span, "A")
    lib.connect_material_expressions(fade_start_param, "", span, "B")
    # max(span, 0.01): an end at or before the start is a step, not a division by zero.
    span_safe = lib.create_material_expression(mat, unreal.MaterialExpressionMax, x + 1200, y - 150)
    span_safe.set_editor_property("const_b", 0.01)
    lib.connect_material_expressions(span, "", span_safe, "A")
    ramp = lib.create_material_expression(mat, unreal.MaterialExpressionDivide, x + 1350, y - 250)
    lib.connect_material_expressions(past_start, "", ramp, "A")
    lib.connect_material_expressions(span_safe, "", ramp, "B")
    far = lib.create_material_expression(mat, unreal.MaterialExpressionSaturate, x + 1450, y - 250)
    lib.connect_material_expressions(ramp, "", far, "")
    # lerp(speckle, 1, far) is lerp(1, speckle, near) without a OneMinus node.
    out = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, x + 1550, y)
    lib.connect_material_expressions(both, "", out, "A")
    out.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(far, "", out, "Alpha")
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



# Owned land in MPC_OwnedLand (land purchase spec 2026-10-02, section 4). THE NAMES ARE
# AAirsideOwnedLandActor::CollectionParams', which writes them: the airport's FLandGrid - whether it is valid, its
# origin, tile size and size, and its 64-bit owned mask as four 16-bit words (a float holds integers exactly to 2^24).
# LandValid defaults to 0, which clips NOTHING - a map whose airport owns no grid, or a frame before any edge actor
# has written the collection, shows all its ground.
OWNED_LAND_MPC = "/Game/Environment/MPC_OwnedLand"
OWNED_LAND_PARAMS = (("LandValid", 0.0), ("LandOriginX", 0.0), ("LandOriginY", 0.0), ("LandTileSize", 60000.0),
                     ("LandColumns", 8.0), ("LandRows", 8.0),
                     ("LandMask0", 0.0), ("LandMask1", 0.0), ("LandMask2", 0.0), ("LandMask3", 0.0))
# #529's rectangle, retired with it: removed from the collection on the next run so nothing can read a stale one.
RETIRED_OWNED_PARAMS = ("OwnedMinX", "OwnedMinY", "OwnedMaxX", "OwnedMaxY")


def owned_land_collection(on_fail=None):
    """MPC_OwnedLand, created or updated IN PLACE keeping each surviving parameter's Id: a CollectionParameter node
    resolves its parameter by that Guid, and rebuilding the array would orphan every node that reads it
    (build_fence_content.collection's rule). Retired names are dropped."""
    folder, name = OWNED_LAND_MPC.rsplit("/", 1)
    if unreal.EditorAssetLibrary.does_asset_exist(OWNED_LAND_MPC):
        mpc = unreal.EditorAssetLibrary.load_asset(OWNED_LAND_MPC)
    else:
        mpc = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.MaterialParameterCollection, unreal.MaterialParameterCollectionFactoryNew())
    if mpc is None:
        if on_fail:
            on_fail("could not load or create %s" % OWNED_LAND_MPC)
        return None
    params = [p for p in mpc.get_editor_property("scalar_parameters")
              if str(p.get_editor_property("parameter_name")) not in RETIRED_OWNED_PARAMS]
    by_name = {str(p.get_editor_property("parameter_name")): p for p in params}
    for pname, value in OWNED_LAND_PARAMS:
        p = by_name.get(pname)
        if p is None:
            p = unreal.CollectionScalarParameter()
            p.set_editor_property("parameter_name", pname)
            params.append(p)
        p.set_editor_property("default_value", value)
    mpc.set_editor_property("scalar_parameters", params)
    unreal.EditorAssetLibrary.save_asset(OWNED_LAND_MPC, only_if_is_dirty=False)
    return mpc


# The tile under the pixel, then its bit. Outside the grid is not owned. Bit = Row * Columns + Column, the
# FLandGrid::BitOf rule; word = bit / 16 picks the float, fmod(floor(m / 2^b), 2) reads the bit.
OWNED_LAND_HLSL = """if (Valid < 0.5) return 1.0;
float2 t = floor((WP.xy - float2(OX, OY)) / Size);
if (t.x < 0 || t.y < 0 || t.x >= Cols || t.y >= Rows) return 0.0;
float bit = t.y * Cols + t.x;
float word = floor(bit / 16.0);
float m = word < 0.5 ? M0 : (word < 1.5 ? M1 : (word < 2.5 ? M2 : M3));
return fmod(floor(m / exp2(bit - word * 16.0)), 2.0) >= 0.5 ? 1.0 : 0.0;"""
OWNED_LAND_PINS = ("Valid", "OX", "OY", "Size", "Cols", "Rows", "M0", "M1", "M2", "M3")


def owned_land_clip(lib, mat, mpc, x, y):
    """1 on owned land, 0 off it - for Opacity Mask. The diorama edge (2026-10-02).

    Wired into an OPAQUE material it does nothing: Opacity Mask is ignored until the blend mode is Masked. That is
    deliberate. Only an instance that overrides the blend mode to Masked pays for the clip, so every map that does not
    opt in renders exactly as before. The land comes from `mpc` (MPC_OwnedLand), written at runtime by
    AAirsideOwnedLandActor from the airport's FLandGrid - one source of truth with the walls, camera, grass and every
    build refusal. Returns the Custom node, or None if a pin did not connect (connect_material_expressions reports a
    misnamed pin only through its bool, and the material still compiles)."""
    custom = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
    custom.set_editor_property("description", "OwnedLandClip")
    custom.set_editor_property("code", OWNED_LAND_HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    pins = []
    for n in ("WP",) + OWNED_LAND_PINS:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", n)
        pins.append(pin)
    custom.set_editor_property("inputs", pins)

    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, x - 300, y)
    ok = lib.connect_material_expressions(world, "", custom, "WP")
    for i, ((param, _), pin) in enumerate(zip(OWNED_LAND_PARAMS, OWNED_LAND_PINS)):
        node = lib.create_material_expression(
            mat, unreal.MaterialExpressionCollectionParameter, x - 300, y + 60 * (i + 1))
        node.set_editor_property("collection", mpc)
        node.set_editor_property("parameter_name", param)
        ok &= lib.connect_material_expressions(node, "", custom, pin)
    return custom if ok else None
