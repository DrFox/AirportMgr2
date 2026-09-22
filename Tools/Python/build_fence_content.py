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


def material(albedo, normal):
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (FENCE_DIR, MAT_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        lib.delete_all_material_expressions(mat)
    else:
        mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            MAT_NAME, FENCE_DIR, unreal.Material, unreal.MaterialFactoryNew())

    # MASKED, NOT TRANSLUCENT (README): it sorts correctly and costs a fraction as much.
    # TWO-SIDED: the player sees both faces of every bay.
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("opacity_mask_clip_value", CLIP)

    base = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -500, 0)
    base.set_editor_property("texture", albedo)
    lib.connect_material_property(base, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.connect_material_property(base, "A", unreal.MaterialProperty.MP_OPACITY_MASK)

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
    mat = material(albedo, normal)
    if mat is None:
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
    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)

    back = unreal.EditorAssetLibrary.load_asset(CONTENT)
    missing = [p for p in ("fence_line_post", "fence_heavy_post", "fence_fabric_material")
               if back.get_editor_property(p) is None]
    if missing:
        fail("DA_AirsideContent lost %s on save" % ", ".join(missing))
    else:
        say("PASS DA_AirsideContent names the fence kit")
        say("ALL VERIFIED")
    say("DONE")


run()
