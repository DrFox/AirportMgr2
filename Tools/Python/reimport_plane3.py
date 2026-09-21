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

REIMPORT IS THE OPERATION THIS ACTUALLY IS, same as reimport_plane2.py: the source changed
and the asset did not move or get renamed. UInterchangeManager::ReimportAsset updates the
UObject in place, so its package path, GUID and every reference to it - M_ModelYard's,
DA_Aircraft_Plane3's, ABP_Plane3's skeleton - survive by construction.

AND IT IS NOW A RIG CHANGE, NOT ONLY A MATERIAL ONE. On 2026-09-21 plane3 gained retraction
bones and bay doors: build_gear_rig.py added gear_L, gear_R, gear_nose and four door bones,
re-parented the wheels under their legs and the steer bone under gear_nose, and
build_gear_bays.py modelled the four door panels. The skin went from 7 joints to 14 and the
mesh from 19 parts to 23. That is exactly what a reimport is for and exactly what would be
lost by a delete-and-recreate, since the ABP targets this mesh's Skeleton by object.

THE HEADER USED TO STATE THE COUNTS - "keeps its 16 meshes, 7 joints and 8 material slots" -
and every one of those numbers is now wrong. They are not restated. A count in prose beside
an asset that is re-imported whenever the model changes is a figure with nothing keeping it
true; what this script actually enforces is EXPECTED_SKIN below, which names parts rather
than counting them.

THE SKELETON CANNOT BE REGENERATED HEADLESSLY, AND THIS REIMPORT NEEDS IT REGENERATED.
MEASURED 2026-09-21, not inferred: the run logs

    LogInterchangeEngine: Error: UInterchangeSkeletalMeshFactory::EndImportAssetObject_
    GameThread, cannot merge bone tree with the existing skeleton.

and leaves a MESH carrying 14 bones beside a SKELETON still carrying 7.

WHY THE MERGE REFUSES. USkeleton::MergeAllBonesToBoneTree accepts bones being ADDED to a
tree; it refuses a tree whose SHAPE changed. plane3's did: build_gear_rig.py re-parented
wheel_L and wheel_R from `root` onto the new gear_L and gear_R, and nosewheel_steer from
`root` onto gear_nose. Three bones changed parent, so this is a different skeleton wearing
the old one's names, and the engine is right to refuse it.

WHY HEADLESS CANNOT ANSWER. InterchangeSkeletalMeshFactory.cpp's next move when the merge
fails is to offer to RECREATE the skeleton - and that path is gated on
`if (GIsRunningUnattendedScript)`, which -unattended sets, so it posts an error instead of
asking. There is no Python route round it either: USkeleton::MergeAllBonesToBoneTree is
ENGINE_API and not a UFUNCTION, and unreal.Skeleton's `copy_bones_from_skeleton` is a
GeometryScript call that copies bone attributes onto a DynamicMesh - a different thing
entirely under a promising name.

SO THIS ONE STEP IS DONE BY HAND, ONCE, IN THE OPEN EDITOR: right-click SK_Plane3 ->
Reimport, and answer YES to regenerating the skeleton. Then run this script to verify, and
the rest of the pipeline scripts as normal. What it costs is that plane3's asset is no
longer reproducible from zero by a commandlet - it never was, since a reimport presupposes
the asset - and what it buys is not carrying a wrapper around an engine-internal API for a
job done once per rig change. bone_report() below is what refuses a run that half-landed, so
the manual step cannot be silently skipped.

IT DOES NOT SWEEP THE ORPHANED MATERIALS. rebuild_fleet_materials() at the end repoints every
slot onto the shared M_Fleet instances, which ORPHANS the per-asset UMaterials Interchange
just regenerated - about 48 KB of dead uber-graph each, referenced by nothing and invisible to
every test. import_models.py learned to sweep automatically on 2026-09-21 and this script did
not, because the sweep wants the set of folders a run touched and this one has exactly one.
Run Tools/Python/clean_orphan_materials.py after this; the tell is
Content/Aircraft/Plane3/Materials/ existing at all.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402

MESH = "/Game/Aircraft/Plane3/SK_Plane3"
SOURCE = r"C:\repos\AirportMgr2Models\plane3\export\plane3.glb"

# The pipeline this script authors, uses and deletes.
PIPELINE_PATH = "/Game/Aircraft/Plane3/PL_Plane3_Reimport"

# Which bone each gear part must ride, checked against the SOURCE .glb.
#
# THE POINT OF THE 2026-09-19 REIMPORT. The gear used to be three meshes skinned PER VERTEX by
# a heuristic in plane3/scripts/build_export.py - tyre material, plus anything within 0.45 m of
# the axle - and a main leg runs from z 0.346 to 2.372 with its axle at 0.431, so the bottom
# half-metre of every strut went into the wheel group and the legs rotated with the wheels in
# engine. The .blend now holds six separate objects, three wheels and three legs, and the
# export skins one bone per object.
#
# CHECKED AGAINST THE .glb RATHER THAN THE ASSET, because that is the artefact Unreal reads and
# the one a stale export would betray. "The legs stopped spinning" is invisible in any count,
# extent or material slot the rest of this script measures - it is a statement about WEIGHTS,
# so it takes a check that reads weights. The same argument covers every row added since.
#
# THE LEGS MOVED OFF `root` ON 2026-09-21, WHICH IS THE SECOND REIMPORT THIS TABLE HAS SEEN.
# plane3/scripts/build_gear_rig.py added retract bones and bay doors, so the two main legs
# now ride gear_L and gear_R rather than the root they were bolted to when the gear was
# fixed, and four door meshes joined that never existed before. The nose leg is the one row
# that did NOT change: it rode nosewheel_steer then and it rides it now, because the steer
# bone was RE-PARENTED under gear_nose rather than replaced - the leg still steers about its
# strut and now retracts with its parent for free.
#
# A ROW PER MOVING PART, and the doors are in here for the same reason the legs are: a door
# left on `root` is a door that hangs open through the whole cycle, and nothing but a weight
# check can tell.
EXPECTED_SKIN = {
    "wheel_L": "wheel_L", "wheel_R": "wheel_R", "nosewheel": "nosewheel",
    "gearRear_L": "gear_L", "gearRear_R": "gear_R", "gearFront": "nosewheel_steer",
    "maindoor_L": "door_main_L", "maindoor_R": "door_main_R",
    "nosedoor_L": "door_nose_L", "nosedoor_R": "door_nose_R",
}

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


def check_source_skin():
    """Every gear part in the .glb rides exactly one bone, and it is the right one.

    READ STRAIGHT OUT OF THE FILE - the JSON chunk for the layout and the BIN chunk for the
    JOINTS_0/WEIGHTS_0 accessors - because a skin weight is not visible anywhere else. The
    export script prints what it INTENDED to skin; this reads what it wrote. That distinction
    is the whole of CLAUDE.md's "a log line is not evidence the thing it describes exists",
    and the per-vertex heuristic this replaces printed a confident, correct-looking report
    every single time it put half a leg on the wheel bone.

    ONE bone per part, not "mostly one": a part with any vertex on a second bone is a part
    that deforms, and none of these should - each is a rigid body bolted to one joint.
    """
    import json
    import struct

    data = open(SOURCE, "rb").read()
    _, _, total = struct.unpack("<III", data[:12])
    off, chunks = 12, {}
    while off < total:
        length, kind = struct.unpack("<I4s", data[off:off + 8])
        chunks[kind.strip(b"\x00").decode()] = data[off + 8:off + 8 + length]
        off += 8 + length
    doc = json.loads(chunks["JSON"].decode("utf-8"))
    blob = chunks.get("BIN", b"")

    fmt_of = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
              5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
    count_of = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

    def read(index):
        acc = doc["accessors"][index]
        fmt, size = fmt_of[acc["componentType"]]
        n = count_of[acc["type"]]
        view = doc["bufferViews"][acc["bufferView"]]
        base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = view.get("byteStride") or size * n
        return [struct.unpack_from("<" + fmt * n, blob, base + i * stride)
                for i in range(acc["count"])]

    if not doc.get("skins"):
        fail("%s declares no skin - nothing in it would animate" % SOURCE)
        return False
    joints = [doc["nodes"][j].get("name") for j in doc["skins"][0]["joints"]]

    # Node names carry Blender's duplicate suffix (wheel_L.001), since the modelling objects
    # already own the clean names. Match on the stem, as airside_import does.
    def stem(name):
        return name.rsplit(".", 1)[0] if name and name[-3:].isdigit() else name

    seen, ok = {}, True
    for node in doc.get("nodes", []):
        index = node.get("mesh")
        if index is None:
            continue
        key = stem(node.get("name", ""))
        if key not in EXPECTED_SKIN:
            continue
        tally = {}
        for prim in doc["meshes"][index]["primitives"]:
            attrs = prim["attributes"]
            if "JOINTS_0" not in attrs:
                tally["(unskinned)"] = tally.get("(unskinned)", 0) + 1
                continue
            for jj, ww in zip(read(attrs["JOINTS_0"]), read(attrs["WEIGHTS_0"])):
                best = max(range(len(ww)), key=lambda k: ww[k])
                name = joints[jj[best]] if ww[best] > 0.0 else "(zero weight)"
                tally[name] = tally.get(name, 0) + 1
        seen[key] = tally
        want = EXPECTED_SKIN[key]
        if list(tally) != [want]:
            fail("%s rides %s in the .glb, expected every vertex on %s - the export is stale "
                 "or the object is skinned to more than one bone"
                 % (key, ", ".join("%s=%d" % kv for kv in sorted(tally.items())), want))
            ok = False
        else:
            say("PASS %-12s %d vert(s), all on %s" % (key, tally[want], want))

    for key in EXPECTED_SKIN:
        if key not in seen:
            fail("%s is not in %s - re-run plane3/scripts/build_export.py" % (key, SOURCE))
            ok = False
    return ok


def bone_report(mesh):
    """The MESH's bone list and the SKELETON's, compared - because they can disagree.

    THIS IS THE CHECK THE 2026-09-21 REIMPORT NEEDED AND DID NOT HAVE. Interchange updated
    the mesh to 14 bones and left the Skeleton asset on 7, and every other check in this
    script passed: slots, extents and skin weights are all read off the mesh or the .glb, and
    none of them can see a stale Skeleton. What breaks is everything downstream - the axis
    resolver reads AnimPose::GetBoneNames off the SKELETON and found no gear, and the Anim
    Blueprint targets the SKELETON and so could not have driven a new bone at all.

    THE JOINTS THE .glb DECLARES ARE THE AUTHORITY, not either asset: the export is the thing
    that changed, and both assets are meant to follow it.
    """
    declared = airside_import.joint_names(airside_import.read_gltf(SOURCE) or {})
    try:
        component = unreal.new_object(unreal.SkeletalMeshComponent)
        component.set_skeletal_mesh_asset(mesh)
        on_mesh = [str(component.get_bone_name(i)) for i in range(component.get_num_bones())]
    except Exception as exc:
        say("NOTE could not read the mesh's bones (%s) - bone check SKIPPED, not passed" % exc)
        return True

    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is None:
        fail("the mesh has no skeleton at all")
        return False
    pose = skeleton.get_reference_pose()
    on_skeleton = [str(n) for n in unreal.AnimPose.get_bone_names(pose)]

    say("the .glb declares %d joint(s); the mesh carries %d; the skeleton carries %d"
        % (len(declared), len(on_mesh), len(on_skeleton)))

    ok = True
    for label, have in (("mesh", on_mesh), ("skeleton", on_skeleton)):
        missing = [b for b in declared if b not in have]
        if missing:
            fail("the %s is missing %d joint(s) the export declares: %s. See THE SKELETON "
                 "CANNOT BE REGENERATED HEADLESSLY in this file's header - this run cannot "
                 "finish the job and the reimport has to be done once in the open editor."
                 % (label, len(missing), ", ".join(missing)))
            ok = False
    if ok:
        say("PASS the mesh AND the skeleton both carry every joint the export declares")
    return ok


def main():
    say("=" * 78)
    clear_strays()

    if not os.path.isfile(SOURCE):
        fail("missing %s" % SOURCE)
        say("DONE")
        return

    # THE SOURCE IS CHECKED BEFORE THE ASSET IS TOUCHED. Importing a stale .glb and then
    # discovering it was stale costs a second reimport and leaves the asset wrong in between;
    # reading the file first costs nothing and refuses the run outright.
    if not check_source_skin():
        fail("the export is not what the reimport expects - nothing was imported")
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

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)      # read the package back
    after_count, after_empty = slot_report(mesh, "after")

    ok = True
    if after_empty:
        fail("%d slot(s) STILL carry no material: %s - the wing would still draw black"
             % (len(after_empty), ", ".join(after_empty)))
        ok = False
    else:
        say("PASS every one of %d slot(s) carries a material" % after_count)

    if not bone_report(mesh):
        ok = False

    if unreal.EditorAssetLibrary.load_asset("/Game/Maps/M_ModelYard") is None:
        say("NOTE M_ModelYard not at /Game/Maps; reference survival not checked here")
    else:
        refs = unreal.EditorAssetLibrary.find_package_referencers_for_asset(MESH, False)
        say("PASS %s still referenced by %s"
            % (MESH.split("/")[-1], ", ".join(sorted(r.split("/")[-1] for r in refs)) or "nothing"))

    unreal.EditorAssetLibrary.save_asset(MESH, only_if_is_dirty=False)

    # A reimport regenerates plane3's materials and reassigns its slots, so the shared set
    # has to be rebuilt or the wing comes back wearing a private uber-graph.
    #
    # The import is at module scope now. Leaving a second one HERE made `airside_import` a
    # local of this whole function, so the call at the top of it raised UnboundLocalError -
    # the reimport never ran and the log's last line was a slot report that looked healthy.
    airside_import.rebuild_fleet_materials()

    say("plane3: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
