"""Builds the ground-cover grass content and points DA_AirsideContent at it. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: (grep Saved/Logs/AirportMgr.log). Spec:
docs/superpowers/specs/2026-10-01-ground-cover-grass-design.md, 4.5.

1. M_Grass - VertexColor x Tint x a per-instance brightness jitter, into base colour, and a
   WORLD-SPACE normal of (0,0,1). The normal is the spike's main finding (2026-10-01): lit by
   their own normals, blades rendered olive with near-black sides and a dirty patch at 50 m;
   lit with the ground's own normal they light exactly as the landscape does. The tint is
   MEASURED - the rendered ground against the rendered blade, as a per-channel ratio - not the
   GRASS_MOWN palette row, which came out olive.
2. SM_GrassTuft1/2/3 from AirportMgr2Models/accessories/grass/export (build_tufts.py), imported
   with NANITE OFF: on these meshes Nanite's simplification deletes whole blades, even at 1 m,
   while the asset thumbnail looks fine. verify() fails the run if any comes back with it on.
3. DA_AirsideContent.GroundCoverTufts names the three. The layer densities stay at their C++
   defaults unless set on the asset by hand.

IN PLACE, never delete-and-recreate (build_fence_content.py's reason): the content asset and
the meshes reference the material and would hold a deleted one open.
"""
import os

import unreal

MODELS = r"C:\repos\AirportMgr2Models\accessories\grass\export"
GRASS_DIR = "/Game/Environment/Grass"
CONTENT = "/Game/DA_AirsideContent"
MATERIAL = "M_Grass"
TUFTS = ["SM_GrassTuft1", "SM_GrassTuft2", "SM_GrassTuft3"]

# Linear. Measured 2026-10-01 against M_Ground after #486 (white balance 5600 K): the blade tip
# matched to the rendered ground beside it. Re-measure if the ground or the grade changes.
TINT = (0.215, 0.38, 0.145)
# +-8 % brightness per instance - enough that neighbouring tufts differ.
JITTER = 0.16

FAILED = []


def say(msg):
    unreal.log("MARKER: " + msg)


def fail(msg):
    FAILED.append(msg)
    unreal.log_error("MARKER: FAIL " + msg)


def open_material(lib):
    """build_fence_content.open_material's shape - see its docstring for why not delete_all."""
    path = "%s/%s" % (GRASS_DIR, MATERIAL)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mat = unreal.EditorAssetLibrary.load_asset(path)
        for expression in list(lib.get_material_expressions(mat)):
            lib.delete_material_expression(mat, expression)
        return mat
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        MATERIAL, GRASS_DIR, unreal.Material, unreal.MaterialFactoryNew())


def material():
    lib = unreal.MaterialEditingLibrary
    mat = open_material(lib)
    # INSTANCED: every tuft is an ISM instance; without the flag the renderer draws the default
    # material instead and nothing errors (the skeletal-mesh flag's trap, one family over).
    mat.set_editor_property("used_with_instanced_static_meshes", True)
    mat.set_editor_property("used_with_nanite", False)
    mat.set_editor_property("tangent_space_normal", False)

    colour = lib.create_material_expression(mat, unreal.MaterialExpressionVertexColor, -900, 0)
    tint = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -900, 200)
    tint.set_editor_property("parameter_name", "Tint")
    tint.set_editor_property("default_value", unreal.LinearColor(TINT[0], TINT[1], TINT[2], 1.0))
    tinted = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -650, 100)
    lib.connect_material_expressions(colour, "", tinted, "A")
    lib.connect_material_expressions(tint, "", tinted, "B")

    # 1 + JITTER x (PerInstanceRandom - 0.5): a brightness per tuft, centred on the measured tint.
    rnd = lib.create_material_expression(mat, unreal.MaterialExpressionPerInstanceRandom, -900, 400)
    centred = lib.create_material_expression(mat, unreal.MaterialExpressionSubtract, -700, 400)
    centred.set_editor_property("const_b", 0.5)
    lib.connect_material_expressions(rnd, "", centred, "A")
    scaled = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -550, 400)
    scaled.set_editor_property("const_b", JITTER)
    lib.connect_material_expressions(centred, "", scaled, "A")
    one_plus = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -400, 400)
    one_plus.set_editor_property("const_b", 1.0)
    lib.connect_material_expressions(scaled, "", one_plus, "A")
    base = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -250, 150)
    lib.connect_material_expressions(tinted, "", base, "A")
    lib.connect_material_expressions(one_plus, "", base, "B")
    lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    up = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -250, 550)
    up.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
    lib.connect_material_property(up, "", unreal.MaterialProperty.MP_NORMAL)

    # Matches the landscape after #486: matte, low specular.
    rough = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -250, 700)
    rough.set_editor_property("r", 0.9)
    lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = lib.create_material_expression(mat, unreal.MaterialExpressionConstant, -250, 800)
    spec.set_editor_property("r", 0.2)
    lib.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)

    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset("%s/%s" % (GRASS_DIR, MATERIAL), only_if_is_dirty=False)
    say("material %s/%s" % (GRASS_DIR, MATERIAL))
    return mat


def tuft(name, mat):
    source = os.path.join(MODELS, name + ".fbx")
    if not os.path.exists(source):
        fail("no export at %s - run accessories/grass/scripts/build_tufts.py" % source)
        return None
    task = unreal.AssetImportTask()
    task.filename = source
    task.destination_path = GRASS_DIR
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    path = "%s/%s" % (GRASS_DIR, name)
    mesh = unreal.EditorAssetLibrary.load_asset(path)
    if not isinstance(mesh, unreal.StaticMesh):
        fail("%s is not a StaticMesh after import (got %r)" % (path, mesh))
        return None
    nanite = mesh.get_editor_property("nanite_settings")
    nanite.set_editor_property("enabled", False)
    mesh.set_editor_property("nanite_settings", nanite)
    mesh.set_material(0, mat)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    box = mesh.get_bounding_box()
    say("%s tris=%d height=%.1f uu" % (name, mesh.get_num_triangles(0), box.max.z - box.min.z))
    return mesh


def verify(meshes):
    mat = unreal.EditorAssetLibrary.load_asset("%s/%s" % (GRASS_DIR, MATERIAL))
    if mat is None or mat.get_editor_property("tangent_space_normal"):
        fail("M_Grass missing or its normal is tangent-space - blades would self-shade dark (spike finding)")
    if mat is not None and not mat.get_editor_property("used_with_instanced_static_meshes"):
        fail("M_Grass lacks the instanced-static-mesh usage flag - tufts would draw the default material")
    if len(meshes) < 3:
        fail("only %d of 3 tufts imported" % len(meshes))
    for mesh in meshes:
        if mesh.get_editor_property("nanite_settings").get_editor_property("enabled"):
            fail("%s has Nanite ON - its simplification deletes whole blades" % mesh.get_name())
        height = mesh.get_bounding_box().max.z - mesh.get_bounding_box().min.z
        # 14-24 cm blades: shorter cannot hide the 10 cm lip, taller reads as a shrub.
        if not 12.0 <= height <= 26.0:
            fail("%s is %.1f uu tall, outside 12-26 - check the FBX scale" % (mesh.get_name(), height))
    back = unreal.EditorAssetLibrary.load_asset(CONTENT)
    named = [m for m in back.get_editor_property("ground_cover_tufts") if m is not None] if back else []
    if len(named) != len(TUFTS):
        fail("DA_AirsideContent names %d tuft(s), expected %d" % (len(named), len(TUFTS)))


def main():
    mat = material()
    meshes = [m for m in (tuft(name, mat) for name in TUFTS) if m is not None]
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - content set not updated" % CONTENT)
    else:
        content.set_editor_property("ground_cover_tufts", meshes)
        unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)
        say("DA_AirsideContent.GroundCoverTufts = %s" % [m.get_name() for m in meshes])
    verify(meshes)
    say("DONE - %s" % ("FAILED: " + "; ".join(FAILED) if FAILED else "all checks passed"))


main()
