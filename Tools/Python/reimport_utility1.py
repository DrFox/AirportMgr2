"""Re-imports SK_Utility1 IN PLACE, to pick up the 'hitch' bone utility1/scripts/build_export.py
added on 2026-09-23 (utility1/README.md's own "Tow hitch socket (2026-09-23)" section) after
SK_Utility1 was first imported. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY A REIMPORT AND NOT import_models.py: SK_Utility1 already exists and nothing here changed
its geometry or its other bones - a delete-and-recreate would be exactly the "re-authoring
strands references" trap reimport_fueltruck1.py's own header names, and SK_Utility1 has none
of its own yet (no ABP, no DA_AirsideContent entry before this task) but is about to gain both.
UInterchangeManager::ReimportAsset updates the UObject in place; this is reimport_fueltruck1.py
with utility1's names.

FOUND MISSING BY import_fueltrailer1.py's own report_tow_chain (task 3b, 2026-09-24): it asked
SK_Utility1 for its 'hitch' bone to measure FTowLink::HitchX and got "no bone named hitch in
the imported skeleton" - the .glb itself declares 9 joints (root, beacon, hitch, steer_FL,
wheel_FL, steer_FR, wheel_FR, wheel_RL, wheel_RR, checked directly against the file), so the
asset in Content was imported before the hitch bone existed and needs exactly the
bUpdateSkeletonReferencePose reimport reimport_pipeline exists for - see its own comment for
what leaving that off cost plane2 (a stale Skeleton through three reimports and nothing said so).
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402
from airside_import import fail, say  # noqa: E402

MESH = "/Game/Vehicles/Utility1/SK_Utility1"
FOLDER = "/Game/Vehicles/Utility1"
SOURCE = r"C:\repos\AirportMgr2Models\utility1\export\utility1.glb"

# The pipeline this script authors, uses and deletes.
PIPELINE_PATH = "/Game/Vehicles/Utility1/PL_Utility1_Reimport"


def source_materials(doc):
    return sorted(m.get("name", "") for m in doc.get("materials", []))


def slot_names(mesh):
    return sorted(str(s.material_slot_name) for s in mesh.get_editor_property("materials"))


def bone_report(mesh, declared):
    """The MESH's bones and the SKELETON's, each against the joints the .glb declares - both,
    because they can disagree (reimport_fueltruck1.py's own note, from reimport_plane3.py)."""
    component = unreal.new_object(unreal.SkeletalMeshComponent)
    component.set_skeletal_mesh_asset(mesh)
    on_mesh = [str(component.get_bone_name(i)) for i in range(component.get_num_bones())]
    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is None:
        fail("the mesh has no skeleton at all")
        return False
    on_skeleton = [str(n) for n in
                   unreal.AnimPose.get_bone_names(skeleton.get_reference_pose())]
    say("the .glb declares %d joint(s); the mesh carries %d; the skeleton carries %d"
        % (len(declared), len(on_mesh), len(on_skeleton)))
    ok = True
    for label, have in (("mesh", on_mesh), ("skeleton", on_skeleton)):
        missing = [b for b in declared if b not in have]
        if missing:
            fail("the %s is missing %s - the bone tree changed" % (label, ", ".join(missing)))
            ok = False
    if ok:
        say("PASS the mesh AND the skeleton both carry every joint the export declares, "
            "including 'hitch'")
    return ok


def main():
    say("=" * 78)
    say("re-importing SK_Utility1 to pick up the 'hitch' bone")
    doc = airside_import.read_gltf(SOURCE)
    if not doc:
        fail("could not read %s" % SOURCE)
        say("DONE")
        return
    declared = airside_import.joint_names(doc)
    wanted = source_materials(doc)
    say("the export carries %d material(s): %s" % (len(wanted), ", ".join(wanted)))

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if not isinstance(mesh, unreal.SkeletalMesh):
        fail("%s is not a SkeletalMesh - run import_models.py for a first import" % MESH)
        say("DONE")
        return
    say("before: %d slot(s): %s" % (len(slot_names(mesh)), ", ".join(slot_names(mesh))))

    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    pipeline_path = airside_import.reimport_pipeline(PIPELINE_PATH)
    if pipeline_path is None:
        say("DONE")
        return
    params.override_pipelines = [unreal.SoftObjectPath(pipeline_path)]
    try:
        manager.reimport_asset(mesh, params)
    except Exception as exc:
        airside_import.drop_reimport_pipeline(PIPELINE_PATH)
        fail("reimport_asset raised %s - do NOT fall back to deleting" % exc)
        say("DONE")
        return
    airside_import.drop_reimport_pipeline(PIPELINE_PATH)

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    after = slot_names(mesh)
    say("after:  %d slot(s): %s" % (len(after), ", ".join(after)))

    ok = True
    missing = [m for m in wanted if m not in after]
    if missing:
        fail("the export's %s did not become a slot" % ", ".join(missing))
        ok = False
    else:
        say("PASS every material in the export is a slot on the mesh")
    empty = [str(s.material_slot_name) for s in mesh.get_editor_property("materials")
             if s.material_interface is None]
    if empty:
        fail("%d slot(s) carry no material: %s" % (len(empty), ", ".join(empty)))
        ok = False

    if not bone_report(mesh, declared):
        ok = False

    unreal.EditorAssetLibrary.save_asset(MESH, only_if_is_dirty=False)

    # A reimport regenerates the per-asset materials and reassigns every slot, so the shared
    # set is rebuilt and what it orphans is swept - in that order.
    airside_import.rebuild_fleet_materials()
    say("-" * 70)
    airside_import.sweep_orphan_materials([FOLDER])

    say("utility1: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
