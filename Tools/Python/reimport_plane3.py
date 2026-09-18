"""Re-imports SK_Plane3 IN PLACE, and clears the wreckage of a delete-and-recreate attempt.
Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY THIS EXISTS. plane3's wing2 and wingConnection had NO material assigned in Blender, so the
glTF primitives carried no material reference and Unreal fell back to its default - the wing
drew black. Fixed in the .blend (both now carry plane3_body) and re-exported, but a mesh
SECTION's material index is baked at import, so the asset has to read the source again.

WHY NOT import_models.py, which is where this went wrong. Its own docstring says it imports
"the models that have a current export AND NO ASSETS YET" - it is a first-import tool. Run
against existing assets it calls clear_previous(), which delete_directory()s the whole folder
and then checks for survivors with list_assets. The registry reported "cleared 3 asset(s)" and
no survivors while SK_Plane3.uasset sat on disk untouched, so the rename of the freshly
imported mesh collided with it and every model in the table failed the same way, leaving a
stray mesh in a SkeletalMeshes/ subfolder.

The delete failing was the level protecting itself: M_ModelYard places SK_Plane3, SK_Tug1,
SK_GPU1 and SK_Utility1. clear_previous's docstring says exactly this - "safe here and nowhere
else... once something outside the folder references these - a DA_ entity, a level - this
stops being safe and the assets have to be re-authored in place instead."

So: never delete these four, and note that clear_previous's survivor check asks the ASSET
REGISTRY when the thing that survives is the FILE. See the memory
unreal-headless-delete-reports-success: check os.path.isfile, not list_assets.

REIMPORT IS THE OPERATION THIS ACTUALLY IS, same as reimport_plane2.py: the source changed,
the asset did not move or get renamed and keeps its 16 meshes, 7 joints and 8 material slots.
UInterchangeManager::ReimportAsset updates the UObject in place, so its package path, GUID and
M_ModelYard's reference to it survive by construction.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MESH = "/Game/Aircraft/Plane3/SK_Plane3"
SOURCE = r"C:\repos\AirportMgr2Models\plane3\export\plane3.glb"

# Stray meshes the failed rename left behind, each with the real mesh it duplicates.
#
# The ONLY thing referencing them is the paired Skeleton and PhysicsAsset, which hold an
# editor-only PREVIEW mesh pointer - both the stray and the real mesh use the same skeleton
# and the same bone count, so the real asset is intact and losing a preview pointer costs
# nothing. That pair is therefore allowed as a referencer and NOTHING ELSE is: anything else
# pointing at a stray means it is not a stray.
STRAYS = [
    ("/Game/Aircraft/Plane3/SkeletalMeshes", "/Game/Aircraft/Plane3/SK_Plane3"),
    ("/Game/Vehicles/Tug1/SkeletalMeshes", "/Game/Vehicles/Tug1/SK_Tug1"),
    ("/Game/Vehicles/GPU1/SkeletalMeshes", "/Game/Vehicles/GPU1/SK_GPU1"),
    ("/Game/Vehicles/Utility1/SkeletalMeshes", "/Game/Vehicles/Utility1/SK_Utility1"),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def package_file(path):
    return os.path.join(unreal.Paths.project_content_dir(),
                        path.replace("/Game/", "") + ".uasset")


def clear_strays():
    """Delete the stranded imports - and CHECK THE FILE afterwards, because delete_directory
    returns success while leaving .uasset files on disk. That is the whole reason this
    script exists."""
    for folder, real in STRAYS:
        if not unreal.EditorAssetLibrary.does_directory_exist(folder):
            continue
        allowed = {real + "_Skeleton", real + "_PhysicsAsset"}
        assets = unreal.EditorAssetLibrary.list_assets(folder, recursive=True)
        blocked = False
        for asset in assets:
            clean = asset.split(".")[0]
            refs = [r.split(".")[0] for r in
                    unreal.EditorAssetLibrary.find_package_referencers_for_asset(clean, False)
                    if r.split(".")[0] != clean]
            unexpected = [r for r in refs if r not in allowed]
            if unexpected:
                fail("%s is referenced by %s, which is not its own skeleton or physics asset "
                     "- NOT deleting" % (clean, ", ".join(sorted(unexpected))))
                blocked = True
            elif refs:
                say("%s: only preview pointers from %s - safe to delete"
                    % (clean.split("/")[-1], ", ".join(sorted(r.split("/")[-1] for r in refs))))
        if blocked:
            continue
        unreal.EditorAssetLibrary.delete_directory(folder)
        left = [a for a in assets if os.path.isfile(package_file(a.split(".")[0]))]
        if left:
            for a in left:
                path = package_file(a.split(".")[0])
                try:
                    os.remove(path)
                    say("removed surviving file %s" % os.path.basename(path))
                except OSError as exc:
                    fail("could not remove %s: %s" % (path, exc))
        say("cleared %d stray asset(s) from %s" % (len(assets), folder))


def slot_report(mesh, when):
    slots = mesh.get_editor_property("materials")
    empty = [str(s.material_slot_name) for s in slots if s.material_interface is None]
    say("%-7s %d slot(s): %s" % (when, len(slots),
                                 ", ".join(str(s.material_slot_name) for s in slots)))
    if empty:
        say("%-7s %d slot(s) with NO material: %s" % (when, len(empty), ", ".join(empty)))
    return len(slots), empty


def main():
    say("=" * 78)
    clear_strays()

    if not os.path.isfile(SOURCE):
        fail("missing %s" % SOURCE)
        say("DONE")
        return
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if not isinstance(mesh, unreal.SkeletalMesh):
        fail("%s is not a SkeletalMesh" % MESH)
        say("DONE")
        return

    before_count, before_empty = slot_report(mesh, "before")

    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    params.override_pipelines = []
    try:
        manager.reimport_asset(mesh, params)
    except Exception as exc:
        fail("reimport_asset raised %s - do NOT fall back to deleting, M_ModelYard places "
             "this mesh" % exc)
        say("DONE")
        return

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)      # read the package back
    after_count, after_empty = slot_report(mesh, "after")

    ok = True
    if after_empty:
        fail("%d slot(s) STILL carry no material: %s - the wing would still draw black"
             % (len(after_empty), ", ".join(after_empty)))
        ok = False
    else:
        say("PASS every one of %d slot(s) carries a material" % after_count)

    if unreal.EditorAssetLibrary.load_asset("/Game/Maps/M_ModelYard") is None:
        say("NOTE M_ModelYard not at /Game/Maps; reference survival not checked here")
    else:
        refs = unreal.EditorAssetLibrary.find_package_referencers_for_asset(MESH, False)
        say("PASS %s still referenced by %s"
            % (MESH.split("/")[-1], ", ".join(sorted(r.split("/")[-1] for r in refs)) or "nothing"))

    unreal.EditorAssetLibrary.save_asset(MESH, only_if_is_dirty=False)

    # A reimport regenerates plane3's materials and reassigns its slots, so the shared set
    # has to be rebuilt or the wing comes back wearing a private uber-graph.
    import airside_import
    airside_import.rebuild_fleet_materials()

    say("plane3: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
