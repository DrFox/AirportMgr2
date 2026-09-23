"""Imports the chainlink fence kit and points DA_AirsideContent at it. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. Every result line is prefixed MARKER: so it can be grepped out of
Saved/Logs/AirportMgr.log.

WHAT THE KIT IS - see AirportMgr2Models/accessories/chainlink/README.md, which is the asset
contract: two posts from Blender, one texture pair, and a material built here. The fabric
MESH is not content at all; UPlotPresenter generates it from each plot's outline.

NOT import_models.py. That table drives airside_import, a glTF vehicle pipeline - wheel
nodes, rigs, axle checks - and these are two static FBX meshes and two PNGs.

FIRST IMPORT ONLY for the meshes, like import_models.py: an existing asset is kept and
re-measured, never re-imported over (memory: a re-import strands a stray mesh). The
material and the texture settings are rebuilt every run; they are cheap and idempotent.

MEASURED AFTER IMPORT, not trusted: FBX unit handling differs between exporters, and a post
that arrives 2.45 uu tall is a correct-looking import of the wrong size. The height and
radius are checked against the README's figures and the run FAILS if they are off.

DISTANCE FADE (2026-09-23). At the build camera's range (9,200-60,000 uu logged, FOV 75) a
4 mm wire is ~0.05 px and a 50 mm diamond ~0.6 px at 10,000 uu, so the textured fabric was
pure sub-pixel noise at every distance the player builds from, and the 60 mm posts were
flickering one-pixel dashes. No mesh LOD can fix that - the fabric is one quad strip. So:

  - the FABRIC blends from the wire pattern to a flat veil (VeilOpacity, dithered) as a
    diamond shrinks from WireAbovePx to VeilBelowPx on screen. Keyed on the UV's screen
    derivative, not on distance, so it is right at any resolution and at grazing angles.
  - the POSTS dither out between PostFadeStart and PostFadeEnd, where they go sub-pixel.
  - the whole fence then dithers to NOTHING between FadeStart and FadeEnd (Sky Haven does
    the same; the plot's own ground carries its outline from there).

All seven figures live in MPC_FenceFade, one collection both materials read, so they can be
tuned live in PIE. THIS SCRIPT OWNS THEIR DEFAULTS: a rerun writes FADE back over any hand
tuning, so copy a tuned value into FADE before running it again.

STILL MASKED, never translucent (see material()): the partial opacity is a blue-noise dither
that TSR resolves into a veil. The FABRIC casts no shadow at all (AAirsideBuildingsActor's
constructor says why); its material's shadow input is kept only so the fabric shadows
correctly up close if that is ever turned back on. The SHADOW pass keeps the old behaviour -
plain wire alpha, solid posts - because the "camera" in the shadow pass is the LIGHT's view, and a fade keyed
on it would fade shadows by the sun's position rather than the player's.

DISTANCE, NOT DEPTH: the fades measure straight-line distance to the camera, not PixelDepth,
because the posts' instance CULL (UPlotPresenter, at PostFadeEnd) is a distance. Keyed on
depth, a post at the edge of the screen - depth well under its distance - would be culled
while the material still drew it half-faded.

NANITE OFF on both posts: a few hundred triangles in a HISM, where Nanite buys nothing, and
a masked material would put every post on Nanite's programmable raster path.
"""
import os

import unreal

MODELS = r"C:\repos\AirportMgr2Models\accessories"
FENCE_DIR = "/Game/Environment/Fence"
CONTENT = "/Game/DA_AirsideContent"

POSTS = [
    # asset name, fbx, expected radius uu, content property
    ("SM_Fence_Post", os.path.join(MODELS, "chainlink", "export", "SM_Fence_Post.fbx"), 3.0, "fence_line_post"),
    ("SM_Fence_CornerPost", os.path.join(MODELS, "chainlink", "export", "SM_Fence_CornerPost.fbx"), 4.5, "fence_heavy_post"),
]
POST_HEIGHT_UU = 245.0
TOLERANCE_UU = 0.5

TEX_ALBEDO = ("T_Chainlink", os.path.join(MODELS, "textures", "chainlink.png"))
TEX_NORMAL = ("T_Chainlink_N", os.path.join(MODELS, "textures", "chainlink_n.png"))
MAT_NAME = "M_ChainlinkFabric"

# README: "Opacity Mask Clip Value ~ 0.33" and "Alpha Coverage Thresholds ~ 0.33" - the one
# number, on both. Without the texture's, the mip chain averages the wire away and the fence
# dissolves at distance.
CLIP = 0.33

# The posts' own material, built here because the import's chainlink_post is an instance of
# Interchange's FBXLegacyPhongSurfaceMaterial - opaque, and an instance takes no graph nodes.
# chainlink_post stays the COLOUR's source and keeps its README name (see post_material()).
POST_MAT_SOURCE = "chainlink_post"
POST_MAT_NAME = "M_ChainlinkPost"
MPC_NAME = "MPC_FenceFade"

# Diamonds per UV unit: one texture tile is 2.4 m of run AND the full 2.4 m height, 48
# diamonds across (README). The veil blend measures a diamond's on-screen size from this.
DIAMONDS_PER_UV = 48.0

# Defaults, uu and px. Chosen 2026-09-23 from the table in the docstring: posts go below a
# pixel at ~7,500 uu; the player builds from 9,000-14,000 uu; max zoom is 60,000 uu.
FADE = [
    ("VeilBelowPx", 1.0),     # diamond this small or smaller: all veil
    ("WireAbovePx", 2.0),     # diamond this large or larger: all wire
    ("VeilOpacity", 0.3),     # the veil's dithered coverage
    ("PostFadeStart", 5000.0),
    ("PostFadeEnd", 8000.0),
    ("FadeStart", 25000.0),   # whole fence starts to go
    ("FadeEnd", 35000.0),     # ...and is gone
]

# Shared by both Custom nodes. ViewScalarBlueNoise is MaterialTemplate.ush's own blue-noise
# lookup (the one DitherTemporalAA's engine path uses); comparing coverage against it gives
# a mask of exactly 0 or 1, so CLIP never cuts a partial value.
DITHER_HLSL = (
    "float Noise = ViewScalarBlueNoise(uint2(Parameters.SvPosition.xy), View.StateFrameIndex);\n"
    "return Coverage > Noise ? 1.0 : 0.0;\n"
)

FABRIC_HLSL = (
    "float2 UvPerPxX = ddx(UV);\n"
    "float2 UvPerPxY = ddy(UV);\n"
    "float UvPerPx = max(max(length(UvPerPxX), length(UvPerPxY)), 1e-7);\n"
    "float DiamondPx = (1.0 / %.1f) / UvPerPx;\n"
    "float Wire = saturate((DiamondPx - VeilBelowPx) / max(WireAbovePx - VeilBelowPx, 1e-3));\n"
    "float Fade = 1.0 - saturate((Dist - FadeStart) / max(FadeEnd - FadeStart, 1.0));\n"
    "float Coverage = lerp(VeilOpacity, WireA > %.3f ? 1.0 : 0.0, Wire) * Fade;\n"
    % (DIAMONDS_PER_UV, CLIP)
) + DITHER_HLSL

# The post's fade is the product of its own early fade and the whole-fence fade, so a post
# can never outlive the fabric it holds up whatever the tuning.
POST_HLSL = (
    "float Own = 1.0 - saturate((Dist - PostFadeStart) / max(PostFadeEnd - PostFadeStart, 1.0));\n"
    "float Fence = 1.0 - saturate((Dist - FadeStart) / max(FadeEnd - FadeStart, 1.0));\n"
    "float Coverage = Own * Fence;\n"
) + DITHER_HLSL


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def import_file(source, name):
    task = unreal.AssetImportTask()
    task.filename = source
    task.destination_path = FENCE_DIR
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return unreal.EditorAssetLibrary.load_asset("%s/%s" % (FENCE_DIR, name))


def post(name, source, radius):
    path = "%s/%s" % (FENCE_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mesh = unreal.EditorAssetLibrary.load_asset(path)
        say("kept existing %s" % path)
    else:
        if not os.path.exists(source):
            fail("no export at %s - run build_posts.py in the models repo" % source)
            return None
        mesh = import_file(source, name)
        say("imported %s" % path)
    if not isinstance(mesh, unreal.StaticMesh):
        fail("%s is not a StaticMesh after import (got %r)" % (path, mesh))
        return None

    box = mesh.get_bounding_box()
    height = box.max.z - box.min.z
    half_x = (box.max.x - box.min.x) * 0.5
    say("%s bounds: height %.2f uu, radius %.2f uu, base z %.2f" % (name, height, half_x, box.min.z))
    ok = True
    if abs(height - POST_HEIGHT_UU) > TOLERANCE_UU:
        fail("%s is %.2f uu tall, expected %.1f - an FBX unit mismatch" % (name, height, POST_HEIGHT_UU))
        ok = False
    if abs(half_x - radius) > TOLERANCE_UU:
        fail("%s radius %.2f uu, expected %.1f" % (name, half_x, radius))
        ok = False
    if abs(box.min.z) > TOLERANCE_UU:
        fail("%s base at z %.2f, expected 0 - the presenter stands posts on their origin" % (name, box.min.z))
        ok = False
    return mesh if ok else None


def texture(name, source, normal):
    tex = import_file(source, name)
    if tex is None:
        fail("%s imported nothing" % name)
        return None
    if normal:
        tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property("srgb", False)
    else:
        # UE strips a bool's b prefix on the way to Python: bDoScaleMipsForAlphaCoverage.
        tex.set_editor_property("do_scale_mips_for_alpha_coverage", True)
        tex.set_editor_property("alpha_coverage_thresholds", unreal.Vector4(0.0, 0.0, 0.0, CLIP))
    path = "%s/%s" % (FENCE_DIR, name)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # READ BACK after reload: a headless save that wrote nothing is a known failure here.
    back = unreal.EditorAssetLibrary.load_asset(path)
    if not normal:
        on = back.get_editor_property("do_scale_mips_for_alpha_coverage")
        thr = back.get_editor_property("alpha_coverage_thresholds")
        if not on or abs(thr.w - CLIP) > 1e-4:
            fail("%s alpha coverage did not stick: on=%s w=%.3f" % (name, on, thr.w))
            return None
        say("PASS %s alpha coverage on, threshold %.2f" % (name, thr.w))
    return back


def collection():
    """MPC_FenceFade, with FADE's names and defaults.

    IN PLACE, keeping each existing parameter's Id: a CollectionParameter node resolves its
    parameter by that Guid, and a fresh CollectionScalarParameter struct gets a new one from
    its constructor - rebuilding the array would orphan every node that reads it."""
    path = "%s/%s" % (FENCE_DIR, MPC_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mpc = unreal.EditorAssetLibrary.load_asset(path)
    else:
        mpc = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            MPC_NAME, FENCE_DIR, unreal.MaterialParameterCollection,
            unreal.MaterialParameterCollectionFactoryNew())
    if mpc is None:
        fail("could not load or create %s" % path)
        return None

    params = list(mpc.get_editor_property("scalar_parameters"))
    by_name = {str(p.get_editor_property("parameter_name")): p for p in params}
    for name, value in FADE:
        p = by_name.get(name)
        if p is None:
            p = unreal.CollectionScalarParameter()
            p.set_editor_property("parameter_name", name)
            params.append(p)
        p.set_editor_property("default_value", value)
    mpc.set_editor_property("scalar_parameters", params)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    back = unreal.EditorAssetLibrary.load_asset(path)
    got = {str(p.get_editor_property("parameter_name")): p.get_editor_property("default_value")
           for p in back.get_editor_property("scalar_parameters")}
    wrong = [n for n, v in FADE if n not in got or abs(got[n] - v) > 1e-3]
    if wrong:
        fail("%s did not save %s" % (MPC_NAME, ", ".join(wrong)))
        return None
    say("PASS %s: %s" % (MPC_NAME, ", ".join("%s=%g" % (n, got[n]) for n, _ in FADE)))
    return back


def open_material(lib, name):
    """Load `name` with an EMPTY graph, or create it. In place, never delete-and-recreate:
    DA_AirsideContent and the post meshes reference these and would hold a deleted one open.

    NOT delete_all_material_expressions: in 5.8 it range-iterates the array it deletes from
    and removes about HALF the graph (build_ground_material.py's clear_graph has the engine
    lines). get_material_expressions returns a copy, so this loop is safe."""
    path = "%s/%s" % (FENCE_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        for expression in list(lib.get_material_expressions(mat)):
            lib.delete_material_expression(mat, expression)
        left = len(lib.get_material_expressions(mat))
        if left:
            fail("%s graph not empty after clear: %d expression(s) remain" % (name, left))
            return None
        return mat
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, FENCE_DIR, unreal.Material, unreal.MaterialFactoryNew())


def add_fade(lib, mat, mpc, hlsl, mpc_names, inputs, shadow):
    """Custom node -> ShadowPassSwitch -> Opacity Mask.

    `inputs` maps a Custom input name to (expression, output name) for the non-collection
    ones; Dist is always the pixel's distance to the camera. `shadow` is (expression, output name) feeding the
    SHADOW pass unchanged - see the module docstring for why the fade stays out of it.
    Returns False on the first connection that did not take; connect_material_expressions
    reports a nameless or misnamed pin only through its bool."""
    custom = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, -250, 500)
    custom.set_editor_property("code", hlsl)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    names = list(inputs.keys()) + ["Dist"] + list(mpc_names)
    pins = []
    for n in names:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", n)
        pins.append(pin)
    custom.set_editor_property("inputs", pins)

    ok = True
    for n, (src, out) in inputs.items():
        ok &= lib.connect_material_expressions(src, out, custom, n)
    world = lib.create_material_expression(mat, unreal.MaterialExpressionWorldPosition, -850, 440)
    camera = lib.create_material_expression(mat, unreal.MaterialExpressionCameraPositionWS, -850, 500)
    dist = lib.create_material_expression(mat, unreal.MaterialExpressionDistance, -550, 500)
    ok &= lib.connect_material_expressions(world, "", dist, "A")
    ok &= lib.connect_material_expressions(camera, "", dist, "B")
    ok &= lib.connect_material_expressions(dist, "", custom, "Dist")
    for i, n in enumerate(mpc_names):
        cp = lib.create_material_expression(mat, unreal.MaterialExpressionCollectionParameter, -550, 560 + 60 * i)
        cp.set_editor_property("collection", mpc)
        cp.set_editor_property("parameter_name", n)
        ok &= lib.connect_material_expressions(cp, "", custom, n)

    # The editor calls it ShadowPassSwitch; the class is UMaterialExpressionShadowReplace.
    switch = lib.create_material_expression(mat, unreal.MaterialExpressionShadowReplace, -50, 500)
    ok &= lib.connect_material_expressions(custom, "", switch, "Default")
    ok &= lib.connect_material_expressions(shadow[0], shadow[1], switch, "Shadow")
    ok &= lib.connect_material_property(switch, "", unreal.MaterialProperty.MP_OPACITY_MASK)
    if not ok:
        fail("%s: a fade connection did not take" % mat.get_name())
    return ok


def material(albedo, normal, mpc):
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (FENCE_DIR, MAT_NAME)
    mat = open_material(lib, MAT_NAME)
    if mat is None:
        return None

    # MASKED, NOT TRANSLUCENT (README): it sorts correctly and costs a fraction as much.
    # TWO-SIDED: the player sees both faces of every bay.
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("opacity_mask_clip_value", CLIP)

    base = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -500, 0)
    base.set_editor_property("texture", albedo)
    lib.connect_material_property(base, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    # The base colour needs no veil branch: the mip chain the veil distances sample IS the
    # averaged wire colour.
    uv = lib.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -750, 500)
    if not add_fade(lib, mat, mpc, FABRIC_HLSL,
                    ["VeilBelowPx", "WireAbovePx", "VeilOpacity", "FadeStart", "FadeEnd"],
                    {"UV": (uv, ""), "WireA": (base, "A")}, shadow=(base, "A")):
        return None

    if normal is not None:
        nrm = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -500, 300)
        nrm.set_editor_property("texture", normal)
        nrm.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
        lib.connect_material_property(nrm, "RGB", unreal.MaterialProperty.MP_NORMAL)

    rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 200)
    rough.set_editor_property("r", 0.45)
    lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    metal = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 260)
    metal.set_editor_property("r", 1.0)
    lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)

    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    back = unreal.EditorAssetLibrary.load_asset(path)
    if back.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_MASKED or not back.get_editor_property("two_sided"):
        fail("%s did not save masked and two-sided" % path)
        return None
    say("PASS %s masked, two-sided, clip %.2f" % (MAT_NAME, back.get_editor_property("opacity_mask_clip_value")))
    return back


def post_material(mpc, meshes):
    """M_ChainlinkPost: the import's look, masked, with the fade; on slot 0 of both posts.

    The LOOK is chainlink_post's: its DiffuseColor is read here every run, so the import stays
    the one source of the colour. Phong's other figures do not carry over to a PBR graph by
    any exact rule; non-metallic at 0.5 roughness is the nearest match to a Shininess-20
    Phong grey, and a post this thin shows no highlight at build distance anyway."""
    lib = unreal.MaterialEditingLibrary
    source = unreal.EditorAssetLibrary.load_asset("%s/%s" % (FENCE_DIR, POST_MAT_SOURCE))
    colour = None
    for p in source.get_editor_property("vector_parameter_values") if source else []:
        if str(p.parameter_info.name) == "DiffuseColor":
            colour = p.parameter_value
    if colour is None:
        fail("%s has no DiffuseColor - the post colour has no source" % POST_MAT_SOURCE)
        return None

    path = "%s/%s" % (FENCE_DIR, POST_MAT_NAME)
    mat = open_material(lib, POST_MAT_NAME)
    if mat is None:
        return None
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("opacity_mask_clip_value", CLIP)
    # USAGE FLAG, set and saved here: the posts are HISM instances, and the editor sets a
    # missing flag in memory on first use and never saves it, so a packaged build would draw
    # them with the default material (MapCheck "missing the usage flag"). Nanite is set FALSE,
    # stated because the field is ours: the posts are not Nanite (see the docstring), and the
    # flag would only compile shader permutations nothing draws with.
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mat.set_editor_property("used_with_nanite", False)

    ok = True
    base = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -300, 0)
    base.set_editor_property("constant", colour)
    ok &= lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 200)
    rough.set_editor_property("r", 0.5)
    ok &= lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    # The SHADOW pass casts a solid post, as it did when the post was opaque.
    solid = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -250, 700)
    solid.set_editor_property("r", 1.0)
    if not ok or not add_fade(lib, mat, mpc, POST_HLSL,
                              ["PostFadeStart", "PostFadeEnd", "FadeStart", "FadeEnd"],
                              {}, shadow=(solid, "")):
        fail("%s: a connection did not take" % POST_MAT_NAME)
        return None
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    for mesh in meshes:
        mesh.set_material(0, mat)
        settings = mesh.get_editor_property("nanite_settings")
        settings.enabled = False
        mesh.set_editor_property("nanite_settings", settings)
        unreal.EditorAssetLibrary.save_asset(mesh.get_path_name().split(".")[0], only_if_is_dirty=False)

    back = unreal.EditorAssetLibrary.load_asset(path)
    if back.get_editor_property("blend_mode") != unreal.BlendMode.BLEND_MASKED:
        fail("%s did not save masked" % path)
        return None
    if not back.get_editor_property("used_with_instanced_static_meshes"):
        fail("%s did not save used_with_instanced_static_meshes" % path)
        return None
    for mesh in meshes:
        again = unreal.EditorAssetLibrary.load_asset(mesh.get_path_name().split(".")[0])
        if again.get_material(0) != back:
            fail("%s slot 0 is %r, not %s" % (again.get_name(), again.get_material(0), POST_MAT_NAME))
            return None
        if again.get_editor_property("nanite_settings").enabled:
            fail("%s is still Nanite after save" % again.get_name())
            return None
    say("PASS %s masked, colour %s, on slot 0 of %s, Nanite off" % (
        POST_MAT_NAME, colour, ", ".join(m.get_name() for m in meshes)))
    return back


def run():
    meshes = {}
    for name, source, radius, prop in POSTS:
        mesh = post(name, source, radius)
        if mesh is None:
            say("DONE")
            return
        meshes[prop] = mesh

    albedo = texture(TEX_ALBEDO[0], TEX_ALBEDO[1], normal=False)
    normal = texture(TEX_NORMAL[0], TEX_NORMAL[1], normal=True)
    if albedo is None:
        say("DONE")
        return
    mpc = collection()
    if mpc is None:
        say("DONE")
        return
    mat = material(albedo, normal, mpc)
    if mat is None or post_material(mpc, list(meshes.values())) is None:
        say("DONE")
        return

    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - content set not updated" % CONTENT)
        say("DONE")
        return
    for prop, mesh in meshes.items():
        content.set_editor_property(prop, mesh)
    content.set_editor_property("fence_fabric_material", mat)
    content.set_editor_property("fence_fade_collection", mpc)
    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)

    back = unreal.EditorAssetLibrary.load_asset(CONTENT)
    missing = [p for p in ("fence_line_post", "fence_heavy_post", "fence_fabric_material",
                           "fence_fade_collection")
               if back.get_editor_property(p) is None]
    if missing:
        fail("DA_AirsideContent lost %s on save" % ", ".join(missing))
    else:
        say("PASS DA_AirsideContent names the fence kit")
        say("ALL VERIFIED")
    say("DONE")


run()
