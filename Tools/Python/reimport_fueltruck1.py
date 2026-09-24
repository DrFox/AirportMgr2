"""Re-imports SK_FuelTruck1 IN PLACE after a re-export. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY NOT import_models.py / import_fueltruck.py: both are FIRST-import tools, and SK_FuelTruck1
is referenced by ABP_FuelTruck1, M_ModelYard and the fleet materials, so a delete-and-recreate
would strand them. UInterchangeManager::ReimportAsset updates the UObject in place -
reimport_plane8.py's argument, and this is that script with the truck's names.

2026-09-24: THE TRUCK WENT BACK FROM 8.5 m TO ITS MODELLED 6.2 m (uniformly, in Blender -
align_and_scale.py's LENGTH_TARGET), because at 8.5 m it was 3.26 m over the tyres and wider
than a road lane. The bones MOVE on this reimport (steer_F* from x 494.5 to 360.7 uu), which is
exactly the case reimport_pipeline's bUpdateSkeletonReferencePose exists for.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402
from airside_import import fail, say  # noqa: E402

MESH = "/Game/Vehicles/FuelTruck1/SK_FuelTruck1"
FOLDER = "/Game/Vehicles/FuelTruck1"
SOURCE = r"C:\repos\AirportMgr2Models\fueltruck1\export\fueltruck1.glb"

# The pipeline this script authors, uses and deletes.
PIPELINE_PATH = "/Game/Vehicles/FuelTruck1/PL_FuelTruck1_Reimport"


def source_materials(doc):
    return sorted(m.get("name", "") for m in doc.get("materials", []))


def slot_names(mesh):
    return sorted(str(s.material_slot_name) for s in mesh.get_editor_property("materials"))


def bone_report(mesh, declared):
    """The MESH's bones and the SKELETON's, each against the joints the .glb declares.

    Both, because they can disagree: reimport_plane3.py's header has the measurement of a
    reimport that updated the mesh to 14 bones and left the Skeleton on 7, with every other
    check green.
    """
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
            fail("the %s is missing %s - the bone tree changed; see reimport_plane3.py's "
                 "THE SKELETON CANNOT BE REGENERATED HEADLESSLY" % (label, ", ".join(missing)))
            ok = False
    if ok:
        say("PASS the mesh AND the skeleton both carry every joint the export declares")
    return ok


def main():
    say("=" * 78)
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
    # OVERRIDDEN, not left to the default stack - see airside_import.reimport_pipeline for
    # what bUpdateSkeletonReferencePose defaulting to False cost plane2.
    pipeline_path = airside_import.reimport_pipeline(PIPELINE_PATH)
    if pipeline_path is None:
        say("DONE")
        return
    params.override_pipelines = [unreal.SoftObjectPath(pipeline_path)]
    try:
        manager.reimport_asset(mesh, params)
    except Exception as exc:
        airside_import.drop_reimport_pipeline(PIPELINE_PATH)
        fail("reimport_asset raised %s - do NOT fall back to deleting, M_ModelYard places "
             "this mesh" % exc)
        say("DONE")
        return
    airside_import.drop_reimport_pipeline(PIPELINE_PATH)

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    after = slot_names(mesh)
    say("after:  %d slot(s): %s" % (len(after), ", ".join(after)))

    ok = True
    # SLOT NAMES AGAINST THE EXPORT'S MATERIALS, not a count: the point of this re-export is
    # a material that was not there before, and a count cannot say which one is missing.
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
    # set is rebuilt and what it orphans is swept - in that order, or the sweep finds the
    # generated materials still worn and keeps them.
    airside_import.rebuild_fleet_materials()
    say("-" * 70)
    airside_import.sweep_orphan_materials([FOLDER])

    say("fueltruck1: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
