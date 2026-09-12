"""Imports the plane2 airframe from glTF. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

MATERIALS ARE NOT AUTHORED HERE, and that is the difference from import_piper.py. The Piper
ships texture maps that had to be given the right sRGB and compression settings by hand; this
export carries no textures at all - eight materials of flat base colour with metallic and
roughness factors - so Interchange's glTF material pipeline reproduces them exactly from the
file. Hand-authoring them would be a second transcription of numbers that already exist, and
the two would drift the first time the model is re-exported.

COMBINED INTO ONE STATIC MESH, the same choice import_piper.py makes and for the same reason:
seventeen nodes left separate are seventeen assets an actor would have to carry and keep in
step, where combined they are one mesh with eight MATERIAL SLOTS - which is the distinction
that actually matters for how it draws.

THE PIPELINE MUST BE A SAVED ASSET, and this is the trap that cost a run. Setting
`AssetImportTask.options` to a live UInterchangeGenericAssetsPipeline SUCCEEDS - the property
exists and accepts the object - and is then ignored: the import ran with Interchange's
default stack and produced seventeen separate meshes while reporting success. Interchange
takes pipelines only through FImportAssetParameters::OverridePipelines, which is an array of
SOFT OBJECT PATHS, so the configured pipeline has to exist on disk first.

THE COST OF COMBINING, stated because the export went to trouble to avoid it: build_export.py
deliberately splits the props, wheels and nosewheels into left/right objects with their
origins on the rotation axis, so they can spin. Combining fuses them into the airframe and
that is gone. It does not matter yet - the sim spins props through a SKELETAL mesh and an
anim Blueprint (see ARoadAgentActor::Airframe and UAirsideAgentAnim), which this export has
no skeleton for. When plane2 is rigged, the skeletal asset is what the sim will use and this
static mesh stays what the editor and the content browser show.
"""
import json
import os
import struct

import unreal

SOURCE = r"C:\repos\AirportMgr2Models\plane2\export\plane2.glb"

MESH_DIR = "/Game/Aircraft/Plane2"
MESH_NAME = "SM_Plane2"
SKEL_NAME = "SK_Plane2"
PIPELINE_PATH = "/Game/Aircraft/Plane2"
PIPELINE_NAME = "PL_Plane2_Combine"

# Measured off the glTF's own POSITION accessors before importing: span 19.75 m on glTF X,
# length 15.94 m on glTF Z, height 8.41 m on glTF Y. Asserted after import so a re-export
# that changes scale is caught HERE rather than as an aircraft that looks fine and makes
# every clearance number in the sim quietly false.

EXPECTED_SPAN_UU = 1975.0
TOLERANCE_UU = 20.0


def joint_names():
    """The joint names the .glb's skin declares, read from the file itself.

    NOT A CONSTANT IN THIS SCRIPT. It was one - EXPECTED_BONES = 7 - and the very next
    export dropped the split nosewheel for a single one, which would have failed this import
    for a change that was entirely intended. The export is the authority on its own rig, so
    the check becomes "the skeleton has a bone per joint the skin declares" and stops needing
    an edit every time the rig changes.

    A glb is a 12-byte header then a length-prefixed JSON chunk; no dependency needed, and
    the editor's Python has no glTF reader anyway.
    """
    try:
        with open(SOURCE, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            doc = json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        fail("could not read the skin from %s: %s" % (SOURCE, exc))
        return []

    nodes = doc.get("nodes", [])
    names = []
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            if 0 <= index < len(nodes):
                names.append(nodes[index].get("name", "?"))
    return names


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def clear_previous():
    """Remove anything a previous run left, so 'what landed' means this run.

    Safe to delete here and nowhere else: these assets are brand new and nothing references
    them yet. Deleting an asset something else points at breaks the reference rather than
    updating it - see build_airlines.py, which loads-or-creates for exactly that reason.
    """
    if unreal.EditorAssetLibrary.does_directory_exist(MESH_DIR):
        existing = unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True)
        for path in existing:
            if PIPELINE_NAME in path:
                continue
            unreal.EditorAssetLibrary.delete_asset(path)
        say("cleared %d asset(s) from a previous run" % len(existing))


def build_pipeline_asset(skeletal):
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
    # ALL, not VisibleOnly: every node in the export collection is meant to be drawn, and
    # VisibleOnly would silently drop anything the exporter happened to mark hidden.
    mesh_pipeline.set_editor_property(
        "combine_static_meshes_behavior",
        unreal.InterchangeCombineStaticMeshesBehavior.ALL)
    # BY_SKELETON gathers all seventeen skinned meshes into ONE skeletal mesh sharing the
    # one skeleton, which is what the export's single 'plane2_rig' skin describes.
    mesh_pipeline.set_editor_property(
        "combine_skeletal_meshes_behavior",
        unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    mesh_pipeline.set_editor_property("import_static_meshes", not skeletal)
    mesh_pipeline.set_editor_property("import_skeletal_meshes", skeletal)

    # MATERIALS IN THEIR OWN SUBFOLDER, which is the layout Content/Aircraft/PiperMeridian
    # already uses: the mesh at the root with Materials/ (and Textures/, where there are any)
    # beside it. asset_type_sub_folders does that; the mesh is moved up out of StaticMeshes/
    # afterwards, since it is the one asset that belongs at the root.
    #
    # use_source_name_for_asset off so nothing nests under a second folder named after the
    # .glb, which is where the first run put everything.
    pipeline.set_editor_property("use_source_name_for_asset", False)
    pipeline.set_editor_property("scene_name_sub_folder", False)
    pipeline.set_editor_property("asset_type_sub_folders", True)

    # NO ROTATION OFFSET until the bounds say one is needed. The Piper's import records that
    # its axis correction was "got wrong by reasoning and right by measuring"; same here.
    pipeline.set_editor_property("import_offset_rotation", unreal.Rotator(0.0, 0.0, YAW))

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("pipeline ready at %s, %s" % (path, "skeletal" if skeletal else "static"))
    return path


# ZERO, because the orientation is now correct AT SOURCE.
#
# It was not always. The first export put the span on glTF X and the nose on glTF +Z, which
# arrives as span on UE X and nose on UE +Y - ninety degrees from UAircraftType's local
# space of +X forward, +Y starboard. A yaw of -90 fixed that for the STATIC import and did
# nothing useful for the skeletal one: measured A/B, yaw 0 and yaw -90 both produced extents
# of 1975 x 1593.6, the mesh never turning, only the Y range flipping sign. Interchange has
# no skeletal equivalent of import_offset_rotation - the only transform knobs on the
# skeletal side are the convert_statics_* flags.
#
# So build_export.py now rotates the EXPORT COPIES +90 degrees about Z as its last step
# before writing the .glb, leaving the modelling objects alone as that script promises. The
# file arrives nose-forward and this offset has nothing left to correct. The checks in
# report_bounds still measure it rather than trusting it.
YAW = 0.0


def import_mesh(pipeline_path, asset_class, wanted_name):
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

    # WHAT ACTUALLY LANDED, not what was asked for - the failure that made the first run
    # look successful while producing seventeen meshes.
    found = unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True)
    meshes = []
    for path in sorted(found):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        kind = type(asset).__name__ if asset else "LOAD FAILED"
        say("  %-56s %s" % (path.split(".")[0], kind))
        # EXACT class, not isinstance: SkeletalMesh and StaticMesh are unrelated, but the
        # skeleton and physics asset that come with a skeletal import are not, and picking
        # the first "mesh-ish" thing would rename whichever sorted first.
        if type(asset) is asset_class:
            meshes.append(asset)

    if len(meshes) != 1:
        fail("%d %s(es) landed, expected 1 - the combine did not take. A pipeline passed "
             "through AssetImportTask.options is accepted and then ignored; it must be a "
             "saved asset in OverridePipelines." % (len(meshes), asset_class.__name__))
        return None

    # RENAMED AND MOVED UP, because a combine names the asset after whichever node came
    # first - 'fin_001' here, which says nothing about what the asset is - and type
    # subfolders put it under StaticMeshes/ when the Piper's layout wants the mesh at the
    # root with Materials/ beside it. destination_name is not honoured on this path, so both
    # happen after the fact. rename_asset carries the mesh's material references with it.
    mesh = meshes[0]
    current = mesh.get_path_name().split(".")[0]
    wanted = "%s/%s" % (MESH_DIR, wanted_name)
    if current != wanted:
        if unreal.EditorAssetLibrary.does_asset_exist(wanted):
            unreal.EditorAssetLibrary.delete_asset(wanted)
        if unreal.EditorAssetLibrary.rename_asset(current, wanted):
            say("renamed %s -> %s" % (current.split("/")[-1], wanted_name))
            mesh = unreal.EditorAssetLibrary.load_asset(wanted)
        else:
            fail("could not rename %s to %s" % (current, wanted))

    # THE COMPANIONS COME TOO. A skeletal import also produces a Skeleton and a PhysicsAsset,
    # and type subfolders leave them under SkeletalMeshes/ named after whichever node the
    # combine happened to pick. Content/Aircraft/PiperMeridian keeps SK_, its _Skeleton and
    # the mesh together at the root, so they are moved and renamed to match.
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
        target = "%s/%s%s" % (MESH_DIR, wanted_name, suffix)
        if stem == target:
            continue
        if unreal.EditorAssetLibrary.does_asset_exist(target):
            unreal.EditorAssetLibrary.delete_asset(target)
        if unreal.EditorAssetLibrary.rename_asset(stem, target):
            say("renamed %s -> %s%s" % (stem.split("/")[-1], wanted_name, suffix))

    return mesh


def extents_of(mesh):
    """(min, max) for either mesh kind.

    UStaticMesh exposes get_bounding_box; USkeletalMesh does not, and gives an FBoxSphere
    Bounds through get_bounds instead. Converted here rather than branching at every caller.
    """
    if hasattr(mesh, "get_bounding_box"):
        box = mesh.get_bounding_box()
        return box.min, box.max
    bounds = mesh.get_bounds()
    origin = bounds.origin
    extent = bounds.box_extent
    return (unreal.Vector(origin.x - extent.x, origin.y - extent.y, origin.z - extent.z),
            unreal.Vector(origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))


def report_bounds(mesh):
    """Measure, then judge. Both, and in that order."""
    low, high = extents_of(mesh)
    bounds = type("B", (), {"min": low, "max": high})
    x = bounds.max.x - bounds.min.x
    y = bounds.max.y - bounds.min.y
    z = bounds.max.z - bounds.min.z
    say("bounds X[%.1f, %.1f] Y[%.1f, %.1f] Z[%.1f, %.1f]"
        % (bounds.min.x, bounds.max.x, bounds.min.y, bounds.max.y, bounds.min.z, bounds.max.z))
    say("extents X=%.1f Y=%.1f Z=%.1f uu" % (x, y, z))

    span = max(x, y)
    if abs(span - EXPECTED_SPAN_UU) > TOLERANCE_UU:
        fail("span is %.1f uu, expected %.1f - the import scaled wrong, or the export "
             "changed. Every clearance check in the sim is measured against this."
             % (span, EXPECTED_SPAN_UU))
    else:
        say("PASS span %.1f uu matches the export's own 19.75 m" % span)

    # The project's local space is +X forward, +Y starboard (see UAircraftType), so the SPAN
    # belongs on Y. Reported as a measurement, not asserted, until a run confirms it.
    if y > x:
        say("PASS span is on Y and length on X, which is the convention")
    else:
        say("NOTE span is on X (%.1f) and length on Y (%.1f) - set YAW to 90 and re-run"
            % (x, y))

    # WHICH WAY IT FACES, not merely which axis it lies on. A 180-degree error keeps the
    # span on Y and the length on X and passes every check above - it just taxis backwards.
    # The nose is the +X reach: the nosewheel is the forward wheel in the export, and the
    # derivation at YAW traces it from there to this axis.
    nose = bounds.max.x
    tail = -bounds.min.x
    if nose > tail:
        say("PASS nose reach %.1f exceeds tail reach %.1f, so it faces +X" % (nose, tail))
    else:
        fail("+X reach is %.1f and -X reach is %.1f - the airframe is backwards and would "
             "taxi tail-first. Check YAW." % (nose, tail))

    # Wheels on the ground is what lets a taxiing agent sit at Z = SurfaceZ with no lift.
    if abs(bounds.min.z) > 10.0:
        say("NOTE mesh bottom is at Z=%.1f rather than 0 - an agent would float or sink"
            % bounds.min.z)
    else:
        say("PASS wheels are on the ground at Z=%.1f" % bounds.min.z)


def report_skeleton(skeletal_mesh):
    """The bone COUNT, which is the thing that discriminates.

    The rig exists so the two props and the four wheels can turn independently. A skinless
    import coerced into a skeletal mesh produces ONE root bone, binds every vertex to it,
    looks completely correct standing still, and animates nothing - so counting bones is the
    check, not merely finding a skeleton.
    """
    skeleton = skeletal_mesh.get_editor_property("skeleton")
    if skeleton is None:
        fail("the skeletal mesh has no skeleton")
        return
    say("skeleton: %s" % skeleton.get_path_name().split(".")[0])

    bones = skeleton.get_editor_property("bone_tree")
    count = len(bones)
    joints = joint_names()
    say("bones: %d" % count)
    say("the export's skin declares %d joint(s): %s" % (len(joints), ", ".join(joints)))

    if not joints:
        fail("the .glb declares no skin - a skinless glTF yields no skeletal mesh at all, "
             "and Interchange will not invent a rig from a flat node hierarchy")
    elif count == len(joints):
        say("PASS a bone per joint the export declares")
    elif count <= 1:
        fail("%d bone(s) - the mesh bound to a single root and nothing can turn "
             "independently. The export lost its skin." % count)
    else:
        fail("%d bones against %d joints in the export's skin - the import dropped or "
             "invented bones" % (count, len(joints)))


def report_materials(mesh):
    """The slots, and whether anything is actually in them."""
    # UStaticMesh calls them static_materials and USkeletalMesh just materials; the slot
    # struct differs too, so the name is read through whichever field each one has.
    if isinstance(mesh, unreal.SkeletalMesh):
        materials = mesh.get_editor_property("materials")
        slot_name = lambda slot: slot.material_slot_name
    else:
        materials = mesh.get_editor_property("static_materials")
        slot_name = lambda slot: slot.material_slot_name

    say("material slots: %d" % len(materials))
    missing = 0
    for slot in materials:
        interface = slot.material_interface
        say("  %-22s -> %s" % (slot_name(slot),
                               interface.get_path_name().split(".")[0] if interface else "NONE"))
        if interface is None:
            missing += 1
    if missing:
        fail("%d slot(s) have no material - the glTF material pipeline did not run" % missing)
    elif len(materials) < 8:
        fail("%d slots, expected 8 - the glTF declares eight materials and a combine that "
             "merged slots would lose the livery" % len(materials))
    else:
        say("PASS %d slots, every one carrying a material from the glTF" % len(materials))


def run():
    clear_previous()

    # SKELETAL FIRST, because it is the asset the sim uses: ARoadAgentActor::Airframe is a
    # USkeletalMeshComponent and UAirsideAgentAnim turns the prop and wheel bones. The static
    # mesh is kept beside it the way Content/Aircraft/PiperMeridian keeps both.
    pipeline_path = build_pipeline_asset(skeletal=True)
    if pipeline_path is None:
        say("DONE")
        return

    skeletal = import_mesh(pipeline_path, unreal.SkeletalMesh, SKEL_NAME)
    if skeletal is None:
        say("DONE")
        return
    report_bounds(skeletal)
    report_skeleton(skeletal)
    report_materials(skeletal)

    # NO STATIC MESH, and this is a consequence of the rig rather than a choice.
    #
    # Every mesh in the .glb is now SKINNED to plane2_rig, so the static pipeline has nothing
    # to take and lands zero assets - which it duly reported the first time this ran after
    # the re-export. An SM_ could still be forced out with force_all_mesh_as_type, but there
    # is nothing to use it for: ARoadAgentActor::Airframe is a USkeletalMeshComponent and
    # UAirsideAgentAnim turns the prop and wheel bones, so the skeletal mesh IS the asset.
    # Two meshes of one aircraft would be two things that have to agree, for no gain.
    #
    # Content/Aircraft/PiperMeridian holds both an SM_ and an SK_; that is history, not a
    # convention worth copying into an airframe that was rigged before it was ever imported.

    # THE SKELETAL USAGE FLAG, without which every one of these renders as grey clay.
    #
    # Interchange generated these materials during an import that produced a STATIC mesh, so
    # bUsedWithSkeletalMesh is false on all of them. A material without that flag cannot
    # compile for the skeletal vertex factory, and the renderer silently substitutes the
    # DEFAULT material - so the component holds eight correct materials, logs eight correct
    # names, and draws a grey aeroplane. That is exactly how it was reported: "when the
    # twotter spawned none of its materials were set".
    #
    # The editor sets the flag itself on first use and recompiles - which is why the mesh
    # looks right in the skeletal mesh editor and wrong in PIE - but that marks the material
    # DIRTY rather than saving it, so the fix evaporates with the session. Set and saved here
    # instead, where the materials are made.
    flagged = 0
    for path in unreal.EditorAssetLibrary.list_assets("%s/Materials" % MESH_DIR, recursive=True):
        material = unreal.EditorAssetLibrary.load_asset(path)
        if not isinstance(material, unreal.Material):
            continue
        material.set_editor_property("used_with_skeletal_mesh", True)
        flagged += 1
    say("set the skeletal usage flag on %d material(s)" % flagged)

    # SAVE EVERYTHING THE IMPORT MADE, not just the mesh.
    #
    # This is the failure CLAUDE.md names and it cost a full round trip: saving only the
    # static mesh left its eight materials alive in the commandlet's memory and absent from
    # disk. The mesh loaded fine in the next editor session, its slots pointed at packages
    # that did not exist, and the aircraft rendered in default grey - which looks like a
    # material authoring problem rather than a missing save. Forced, because save_asset does
    # nothing for a package the editor does not think is dirty.
    saved = 0
    for path in unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True):
        if unreal.EditorAssetLibrary.save_asset(path.split(".")[0], only_if_is_dirty=False):
            saved += 1
    say("saved %d asset(s) under %s" % (saved, MESH_DIR))

    # Read one back the way a FRESH editor would, since that is where the absence showed up.
    probe = "%s/Materials/plane2_livery" % MESH_DIR
    if unreal.EditorAssetLibrary.does_asset_exist(probe):
        say("PASS %s exists on disk" % probe)

        # READ THE FLAG BACK, because it is the one that turns a correct material into a
        # grey one and nothing on screen distinguishes the two until an aircraft spawns.
        unflagged = []
        for path in unreal.EditorAssetLibrary.list_assets("%s/Materials" % MESH_DIR, recursive=True):
            material = unreal.EditorAssetLibrary.load_asset(path)
            if isinstance(material, unreal.Material) and not material.get_editor_property(
                    "used_with_skeletal_mesh"):
                unflagged.append(path.split("/")[-1].split(".")[0])
        if unflagged:
            fail("these would render as grey clay on a skeletal mesh: %s" % ", ".join(unflagged))
        else:
            say("PASS every material is flagged for skeletal meshes")
    else:
        fail("%s is not on disk - the mesh's slots will resolve to nothing and the aircraft "
             "will render in default grey" % probe)
    # THE PIPELINE ASSET IS TOOLING, not content, and it is deleted rather than left sitting
    # in an aircraft folder that otherwise holds only the mesh and its materials - the layout
    # Content/Aircraft/PiperMeridian keeps. build_pipeline_asset authors it deterministically,
    # so the next run simply makes it again.
    pipeline_asset = "%s/%s" % (PIPELINE_PATH, PIPELINE_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(pipeline_asset):
        unreal.EditorAssetLibrary.delete_asset(pipeline_asset)
        say("removed the import pipeline asset; it is rebuilt on each run")

    say("DONE")


run()
