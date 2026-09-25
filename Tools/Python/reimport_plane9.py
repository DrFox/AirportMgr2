"""Re-imports SK_Plane9 IN PLACE after a re-export. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY NOT import_models.py. It is a first-import tool and SKIPs a model already in Content -
see its header and the memory import-models-is-first-import-only. SK_Plane9 is referenced by
M_ModelYard, DA_Aircraft_Plane9 and ABP_Plane9's skeleton, so a delete-and-recreate would
strand all three. UInterchangeManager::ReimportAsset updates the UObject in place, so the
package path, GUID and every reference survive by construction - reimport_plane3.py's
argument, unchanged.

THE FIRST RE-EXPORT, 2026-09-25, ADDED THE LIVERY AND NOTHING ELSE: plane9_livery on the
fin, wing and fuselage, painted after the first export. That export dropped the material
because no face used it, so the mesh came in with six slots. The bone check still runs
every time, for plane8's reason.

IT SWEEPS ITS OWN ORPHANS, unlike reimport_plane3.py, whose header names the gap: the rebuild
repoints every slot onto the shared M_Fleet instances and orphans the per-asset UMaterials
Interchange just regenerated. This run touched exactly one folder, so it sweeps exactly that.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402
from airside_import import fail, say  # noqa: E402

MESH = "/Game/Aircraft/Plane9/SK_Plane9"
FOLDER = "/Game/Aircraft/Plane9"
SOURCE = r"C:\repos\AirportMgr2Models\plane9\export\plane9.glb"

# The pipeline this script authors, uses and deletes.
PIPELINE_PATH = "/Game/Aircraft/Plane9/PL_Plane9_Reimport"


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

    # THE SKELETON TOO, not the mesh alone: reimport_pipeline updates the Skeleton asset, and a
    # Skeleton left unsaved is the stale bone tree the next session re-merges and asks to save
    # - see airside_import.save_mesh_and_skeleton.
    if not airside_import.save_mesh_and_skeleton(MESH):
        ok = False

    # A reimport regenerates the per-asset materials and reassigns every slot, so the shared
    # set is rebuilt and what it orphans is swept - in that order, or the sweep finds the
    # generated materials still worn and keeps them.
    airside_import.rebuild_fleet_materials()
    say("-" * 70)
    airside_import.sweep_orphan_materials([FOLDER])

    say("plane9: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
