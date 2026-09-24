"""Imports the fuel truck from glTF as a SKELETAL mesh. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

SKELETAL, NOT STATIC, and that is the difference from what UAirsideContent expected. Its
VehicleMesh is a UStaticMesh and its comment says "a truck's wheels turn and nothing else
does, and there is no rig yet". That stopped being true on 2026-09-14: the export carries
root, four wheels, two steer bones and a beacon, with the front wheels as steer->roll CHAINS
exactly like plane2's nose gear. A static import would throw that away.

MATERIALS ARE NOT AUTHORED HERE, the same choice import_plane2.py makes: the export carries
fourteen flat-colour materials with no textures and no UVs, so Interchange's glTF material
pipeline reproduces them exactly. Hand-authoring them would be a second transcription of
numbers that already exist, and the two would drift on the next re-export.

COMBINED INTO ONE SKELETAL MESH by skeleton, so thirty nodes become one asset with fourteen
material slots rather than thirty assets an actor would have to keep in step.

THE PIPELINE MUST BE A SAVED ASSET. Setting AssetImportTask.options to a live
UInterchangeGenericAssetsPipeline SUCCEEDS and is then ignored - Interchange takes pipelines
only through FImportAssetParameters::OverridePipelines, an array of SOFT OBJECT PATHS. That
cost import_plane2.py a run; it is written down there and repeated here rather than learned
a second time.
"""
import json
import os
import struct
import sys

import unreal

SOURCE = r"C:\repos\AirportMgr2Models\fueltruck1\export\fueltruck1.glb"

MESH_DIR = "/Game/Vehicles/FuelTruck1"
SKEL_NAME = "SK_FuelTruck1"
PIPELINE_PATH = "/Game/Vehicles/FuelTruck1"
PIPELINE_NAME = "PL_FuelTruck1_Combine"

# Measured off the glTF's own POSITION accessors before importing. Asserted after import so a
# re-export that changes scale is caught HERE rather than as a truck that looks right and
# makes every clearance and footprint figure in the sim quietly false.
#
# 6.200 m AGAIN SINCE 2026-09-24: the 8.5 m below was a uniform enlargement to test crabbing,
# and it made the truck 3.26 m wide - wider than a road lane. Re-imported in place by
# reimport_fueltruck1.py; this first-import script is not re-run. The history:
#
# 8.500 m FROM 2026-09-15, up from 6.200 - which was a Ford Transit, an 11.2 m kerb-to-kerb
# circle parked beside a 737. 8.5 m is the smallest real hydrant dispenser. The resize was
# done in the MODEL (align_and_scale.py's LENGTH_TARGET) and not here; see the note above
# YAW for why an import-time scale cannot do it.
#
# LENGTH is the asserted dimension, not width: the model's README states length as the one
# matched dimension and records the width as deliberately over target. Asserting the width
# would fail a model that is correct by its own convention.
EXPECTED_LENGTH_UU = 850.0
TOLERANCE_UU = 20.0

# DO NOT SCALE HERE. Tried and rejected 2026-09-15, when the truck needed to grow from 6.2 m
# to a real dispenser's 8.5 m: InterchangeGenericAssetsPipeline.import_offset_uniform_scale
# is applied INCONSISTENTLY to a skinned import, and there is no value that gets both halves
# right. Measured, twice, off this script's own assertions and
# Airside.Content.VehicleFootprintMatchesTheMesh:
#
#   requested 1.371  -> mesh 1165.3 uu (620 * 1.371^2), bones 494.5 uu (360.7 * 1.371)
#   requested 1.1709 -> mesh  850.0 uu (620 * 1.1709^2), bones 422.4 uu (360.7 * 1.1709)
#
# The vertices take the global offset TWICE - once baked in, once on the bind pose - and the
# skeleton takes it ONCE. So the body and the rig scale by different factors: the second run
# above produced a correctly sized 8.5 m truck whose wheelbase was 4.22 m instead of 4.95,
# a wheelbase/length ratio of 0.497 against the model's authored 0.582. A wheel whose mesh
# and whose bone disagree about where it is does not stay on its axle when it turns.
#
# RESIZE IN BLENDER AND RE-EXPORT instead. That scales vertices and bones together, which is
# the only place the two are guaranteed to stay in step.

# ZERO, for the same reason import_plane2.py's is: the orientation is correct AT SOURCE.
# fueltruck1/scripts/build_export.py yaws the export copies so the truck arrives nose on
# glTF +X, which lands on UE +X - the convention every other asset here follows. The checks
# in report_bounds still MEASURE it rather than trusting it.
YAW = 0.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def read_gltf():
    """The glb's JSON chunk: 12-byte header, then a length-prefixed chunk.

    No dependency needed, and the editor's Python has no glTF reader anyway.
    """
    try:
        with open(SOURCE, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            return json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        fail("could not read %s: %s" % (SOURCE, exc))
        return None


def joint_names(doc):
    """The joint names the .glb's skin declares, read from the file itself.

    NOT A CONSTANT IN THIS SCRIPT, for the reason import_plane2.py records: it had
    EXPECTED_BONES = 7 and the next export changed the rig, which would have failed an
    import for a change that was entirely intended. The export is the authority on its own
    rig, so the check is "the skeleton has a bone per joint the skin declares".
    """
    nodes = doc.get("nodes", [])
    names = []
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            if 0 <= index < len(nodes):
                names.append(nodes[index].get("name", "?"))
    return names


def axle_centres_uu(doc):
    """(front axle, rear axle) X in uu, measured off the .glb's own wheel meshes.

    READ FROM THE FILE, like joint_names above and for the same reason. glTF x is UE X for
    this export and a glTF metre is 100 uu.

    THE WHEEL MESH NODES ARE MATCHED BY EXACT NAME so a renamed rig reports a miss rather
    than quietly measuring the wrong part - 'steer_FL' is a joint with no mesh of its own
    and must not be mistaken for the wheel it steers.
    """
    accessors = doc.get("accessors", [])
    meshes = doc.get("meshes", [])

    def centre_x(mesh_index):
        low = None
        high = None
        for primitive in meshes[mesh_index].get("primitives", []):
            at = primitive.get("attributes", {}).get("POSITION")
            if at is None or at >= len(accessors):
                continue
            accessor = accessors[at]
            low = accessor["min"][0] if low is None else min(low, accessor["min"][0])
            high = accessor["max"][0] if high is None else max(high, accessor["max"][0])
        return None if low is None else (low + high) * 0.5 * 100.0

    front = []
    rear = []
    for node in doc.get("nodes", []):
        index = node.get("mesh")
        if index is None or index >= len(meshes):
            continue
        name = node.get("name", "")
        at = centre_x(index)
        if at is None:
            continue
        if name in ("wheel_FL", "wheel_FR"):
            front.append(at)
        elif name in ("wheel_RL", "wheel_RR"):
            rear.append(at)

    if not front or not rear:
        return None, None
    return sum(front) / float(len(front)), sum(rear) / float(len(rear))


def clear_previous():
    """Remove anything a previous run left, so 'what landed' means this run.

    THE WHOLE DIRECTORY, not asset by asset, and this is worth the paragraph. Deleting them
    one at a time SILENTLY FAILS when they reference each other - the PhysicsAsset and the
    Skeleton both point at the mesh, and the mesh at fourteen materials - so delete_asset
    returns false, nothing says so, and the survivors then collide with this run's rename as
    "the destination path is not valid. An asset already exists at this location". That
    reads as an import bug and is really a stale folder; it cost a run here.

    Safe here and nowhere else: everything under MESH_DIR is this script's own output and
    nothing outside the folder references it. The pipeline goes too and is simply rebuilt.
    """
    if not unreal.EditorAssetLibrary.does_directory_exist(MESH_DIR):
        return True
    existing = unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True)
    unreal.EditorAssetLibrary.delete_directory(MESH_DIR)

    # VERIFIED, because the delete above returns a bool nobody reads and the failure mode is
    # a confusing error three steps later rather than here.
    survivors = unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True) \
        if unreal.EditorAssetLibrary.does_directory_exist(MESH_DIR) else []
    if survivors:
        fail("%d of %d asset(s) survived the clear of %s: %s. Something outside the folder "
             "references them, or the editor has them open - delete them by hand and re-run."
             % (len(survivors), len(existing), MESH_DIR,
                ", ".join(p.split("/")[-1] for p in survivors[:6])))
        return False
    say("cleared %d asset(s) from a previous run" % len(existing))
    return True


def build_pipeline_asset():
    """The configured pipeline, ON DISK, because that is the only kind Interchange takes."""
    path = "%s/%s" % (PIPELINE_PATH, PIPELINE_NAME)
    pipeline = unreal.EditorAssetLibrary.load_asset(path)
    if pipeline is None:
        pipeline = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            PIPELINE_NAME, PIPELINE_PATH, unreal.InterchangeGenericAssetsPipeline, None)
    if pipeline is None:
        fail("could not create the import pipeline at %s" % path)
        return None

    mesh_pipeline = pipeline.get_editor_property("mesh_pipeline")
    # BY_SKELETON gathers every skinned mesh into ONE skeletal mesh sharing the one skeleton,
    # which is what the export's single 'fueltruck1_rig' skin describes.
    mesh_pipeline.set_editor_property(
        "combine_skeletal_meshes_behavior",
        unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    mesh_pipeline.set_editor_property("import_static_meshes", False)
    mesh_pipeline.set_editor_property("import_skeletal_meshes", True)

    # MATERIALS IN THEIR OWN SUBFOLDER, the layout Content/Aircraft/* already uses: the mesh
    # at the root with Materials/ beside it. The mesh is moved up out of SkeletalMeshes/
    # afterwards, since it is the one asset that belongs at the root.
    pipeline.set_editor_property("use_source_name_for_asset", False)
    pipeline.set_editor_property("scene_name_sub_folder", False)
    pipeline.set_editor_property("asset_type_sub_folders", True)
    pipeline.set_editor_property("import_offset_rotation", unreal.Rotator(0.0, 0.0, YAW))

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("pipeline ready at %s, skeletal" % path)
    return path


def import_mesh(pipeline_path):
    if not os.path.isfile(SOURCE):
        fail("missing %s" % SOURCE)
        return None

    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    source_data = unreal.InterchangeManager.create_source_data(SOURCE)

    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    params.override_pipelines = [unreal.SoftObjectPath(pipeline_path)]

    manager.import_asset(MESH_DIR, source_data, params)

    # WHAT ACTUALLY LANDED, not what was asked for - the failure that made import_plane2.py's
    # first run look successful while producing seventeen meshes.
    found = unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True)
    meshes = []
    for path in sorted(found):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        kind = type(asset).__name__ if asset else "LOAD FAILED"
        say("  %-56s %s" % (path.split(".")[0], kind))
        # EXACT class, not isinstance: the Skeleton and PhysicsAsset that come with a
        # skeletal import would otherwise be picked up too.
        if type(asset) is unreal.SkeletalMesh:
            meshes.append(asset)

    if len(meshes) != 1:
        fail("%d SkeletalMesh(es) landed, expected 1 - the combine did not take. A pipeline "
             "passed through AssetImportTask.options is accepted and then ignored; it must "
             "be a saved asset in OverridePipelines." % len(meshes))
        return None

    # RENAMED AND MOVED UP: a combine names the asset after whichever node came first, which
    # says nothing about what the asset is, and type subfolders put it under SkeletalMeshes/.
    # destination_name is not honoured on this path, so both happen after the fact.
    mesh = meshes[0]
    current = mesh.get_path_name().split(".")[0]
    wanted = "%s/%s" % (MESH_DIR, SKEL_NAME)
    if current != wanted:
        if unreal.EditorAssetLibrary.does_asset_exist(wanted):
            unreal.EditorAssetLibrary.delete_asset(wanted)
        if unreal.EditorAssetLibrary.rename_asset(current, wanted):
            say("renamed %s -> %s" % (current.split("/")[-1], SKEL_NAME))
            mesh = unreal.EditorAssetLibrary.load_asset(wanted)
        else:
            fail("could not rename %s to %s" % (current, wanted))

    # THE COMPANIONS COME TOO - the Skeleton and PhysicsAsset a skeletal import produces,
    # left under SkeletalMeshes/ named after whichever node the combine picked.
    for path in unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True):
        stem = path.split(".")[0]
        asset = unreal.EditorAssetLibrary.load_asset(path)
        suffix = None
        if isinstance(asset, unreal.Skeleton):
            suffix = "_Skeleton"
        elif isinstance(asset, unreal.PhysicsAsset):
            suffix = "_PhysicsAsset"
        if suffix is None:
            continue
        target = "%s/%s%s" % (MESH_DIR, SKEL_NAME, suffix)
        if stem == target:
            continue
        if unreal.EditorAssetLibrary.does_asset_exist(target):
            unreal.EditorAssetLibrary.delete_asset(target)
        if unreal.EditorAssetLibrary.rename_asset(stem, target):
            say("renamed %s -> %s%s" % (stem.split("/")[-1], SKEL_NAME, suffix))

    return mesh


def extents_of(mesh):
    """(min, max). USkeletalMesh has no get_bounding_box; it gives an FBoxSphereBounds."""
    bounds = mesh.get_bounds()
    origin = bounds.origin
    extent = bounds.box_extent
    return (unreal.Vector(origin.x - extent.x, origin.y - extent.y, origin.z - extent.z),
            unreal.Vector(origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))


def report_bounds(mesh, doc):
    """Measure, then judge. Both, and in that order."""
    low, high = extents_of(mesh)
    x = high.x - low.x
    y = high.y - low.y
    z = high.z - low.z
    say("bounds X[%.1f, %.1f] Y[%.1f, %.1f] Z[%.1f, %.1f]"
        % (low.x, high.x, low.y, high.y, low.z, high.z))
    say("extents X=%.1f Y=%.1f Z=%.1f uu" % (x, y, z))

    if abs(x - EXPECTED_LENGTH_UU) > TOLERANCE_UU:
        fail("length on X is %.1f uu, expected %.1f - the import scaled wrong, or the export "
             "changed. FTrafficRules::VehicleFootprint is set to this figure, so the arbiter "
             "would reserve the wrong amount of road." % (x, EXPECTED_LENGTH_UU))
    else:
        say("PASS length %.1f uu matches the export's own 6.200 m" % x)

    # +X forward, +Y starboard (UAircraftType's local space, which vehicles share), so the
    # LENGTH belongs on X and the width on Y.
    if x > y:
        say("PASS length is on X and width on Y, which is the convention")
    else:
        fail("length is on Y (%.1f) and width on X (%.1f) - the truck is rotated 90 degrees. "
             "Set YAW and re-run." % (y, x))

    # WHICH WAY IT FACES, FROM THE AXLES rather than from the bounds: the bounds measure the
    # ORIGIN at least as much as the facing, and this model's origin is deliberately at one
    # end of the wheelbase.
    front, rear = axle_centres_uu(doc)
    if front is None:
        fail("could not find the wheel meshes in the export, so nothing here knows which way "
             "it faces - check the node names against axle_centres_uu")
    elif front > rear:
        say("PASS front axle %.1f uu is forward of the rear %.1f uu, so it faces +X"
            % (front, rear))
    else:
        fail("front axle is at %.1f uu and the rear at %.1f - the truck is backwards and "
             "would drive in reverse. Check the export's final rotation." % (front, rear))

    # THE ORIGIN IS THE REAR AXLE, which the model's README states as its convention
    # ("Origin: rear axle centre, projected to the ground... because that is what a
    # front-steered truck pivots about"). Asserted because ARoadAgentActor puts this origin
    # on the guideline, so the point the truck keeps on the painted line IS this point.
    if rear is not None and abs(rear) > 10.0:
        fail("the rear axle is %.1f uu from the origin, not on it. The README states the "
             "origin is the rear axle centre; a truck whose origin has moved would swing "
             "wide of every corner it turns." % rear)
    elif rear is not None:
        say("PASS the origin is the rear axle (%.1f uu), as the model's README states" % rear)

    # WHEELS ON THE GROUND, a FAIL for the reason import_plane2.py records: SetMotion applies
    # no lift to a real mesh, so the truck floats or sinks by whatever this is.
    if abs(low.z) > 10.0:
        fail("mesh bottom is at Z=%.1f rather than 0. The agent would float or sink. "
             "Re-export with the wheels on the ground plane." % low.z)
    else:
        say("PASS wheels are on the ground at Z=%.1f, so z=0 is the contact plane" % low.z)


def report_rig(mesh, doc):
    """A bone per joint the skin declares, and the steer->roll chains named.

    THE CHAINS ARE THE POINT of importing this skeletal at all: each front wheel's roll bone
    is a CHILD of its steer bone, so the roll axis turns with the steering. The parentage is
    reported from the .glb because UE 5.8 exposes no Python reader for a Skeleton's bone
    tree - the editor-side check here is that every declared joint arrived.
    """
    declared = joint_names(doc)
    if mesh.skeleton is None:
        fail("the skeletal mesh has no skeleton")
        return
    say("skin declares %d joints: %s" % (len(declared), ", ".join(declared)))

    # THROUGH A COMPONENT, because USkeletalMesh exposes no bone lookup to Python in 5.8:
    # find_bone_index is not on the asset (it reads as though it should be, and is not), and
    # a Skeleton's ReferenceSkeleton is not a UPROPERTY so the names are not reachable from
    # the asset either. A transient SkeletalMeshComponent has get_num_bones/get_bone_name,
    # and needs no world to answer them.
    bones = []
    try:
        component = unreal.new_object(unreal.SkeletalMeshComponent)
        component.set_skeletal_mesh_asset(mesh)
        bones = [str(component.get_bone_name(i)) for i in range(component.get_num_bones())]
    except Exception as exc:
        say("NOTE could not read the skeleton's bones (%s) - the rig is reported from the "
            "export below and the editor-side check is skipped, NOT passed" % exc)

    if bones:
        missing = [want for want in declared if want not in bones]
        if missing:
            fail("the skeleton is missing %d declared joint(s): %s"
                 % (len(missing), ", ".join(missing)))
        else:
            say("PASS all %d declared joints have bones (skeleton has %d)"
                % (len(declared), len(bones)))

    nodes = doc.get("nodes", [])
    parent_of = {}
    for i, node in enumerate(nodes):
        for child in node.get("children", []):
            parent_of[child] = i
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            name = nodes[index].get("name", "?")
            if not name.startswith("wheel_F"):
                continue
            if index not in parent_of:
                fail("%s has no parent in the export - the steer chain is broken, and the "
                     "wheel would spin about a fixed axis while the truck turns" % name)
                continue
            parent = nodes[parent_of[index]].get("name", "?")
            if parent.startswith("steer_"):
                say("PASS chain %s <- %s" % (name, parent))
            else:
                fail("%s hangs off %s, not a steer bone - the steer chain is broken"
                     % (name, parent))


def main():
    say("=" * 70)
    say("importing the fuel truck from %s" % SOURCE)
    doc = read_gltf()
    if doc is None:
        return
    if not clear_previous():
        return
    pipeline_path = build_pipeline_asset()
    if pipeline_path is None:
        return
    mesh = import_mesh(pipeline_path)
    if mesh is None:
        return
    say("materials: %d slot(s)" % len(mesh.materials))
    report_bounds(mesh, doc)
    report_rig(mesh, doc)

    # THE SKELETAL USAGE FLAG, WITHOUT WHICH EVERY ONE OF THESE RENDERS AS GREY CLAY.
    #
    # THIS SCRIPT DID NOT DO IT UNTIL 2026-09-18, and the truck's fourteen materials shipped
    # without the flag as a result. A UMaterial with bUsedWithSkeletalMesh false cannot
    # compile for the skeletal vertex factory, so the renderer silently substitutes the
    # DEFAULT material - the component holds correct materials, logs correct names, and draws
    # a grey truck. It is the same defect plane2 was reported for on 2026-09-12 ("when the
    # twotter spawned none of its materials were set"); import_plane2.py learned it and this
    # script, written afterwards, did not.
    #
    # Shared with airside_import.py rather than copied, so the next importer cannot miss it
    # the way this one did. fix_fueltruck_materials.py repairs an ALREADY-imported truck
    # without the re-import this function's clear_previous() would force.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from airside_import import flag_for_skeletal
    flag_for_skeletal(MESH_DIR)

    unreal.EditorAssetLibrary.save_directory(MESH_DIR, only_if_is_dirty=False, recursive=True)
    say("saved %s" % MESH_DIR)
    say("=" * 70)


main()
