"""The glTF -> skeletal mesh import mechanism, with no knowledge of any particular model.

NOT A FOURTH COPY, and that is the whole reason this file exists. import_piper.py,
import_plane2.py and import_fueltruck.py are ~85% the same script: the same Interchange
pipeline-on-disk dance, the same rename-and-move-up, the same bounds/rig/material checks.
import_fueltruck.py even re-records import_plane2.py's OverridePipelines lesson verbatim,
"written down there and repeated here rather than learned a second time" - which is the
symptom of a mechanism that wanted extracting. Four more copies for plane3, tug1, gpu1 and
utility1 would be seven places that have to agree about what a correct import looks like.

So the MECHANISM lives here and the MODEL FACTS live in import_models.py's table. A spec
declares only what is true of one model: where its .glb is, where its assets go, which mesh
nodes are its front and rear wheels, and where its origin is supposed to sit.

THE EXISTING THREE ARE LEFT ALONE. They are working, heavily annotated, and each records
lessons at the site that learned them; rewriting them onto this module is a refactor nobody
asked for, and the refactor contract in CLAUDE.md would demand a test per seam to prove it
changed nothing. If one of them is next re-imported and the divergence bites, that is the
moment to fold it in.

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.
"""
import json
import os
import re
import struct

import unreal


# Blender's duplicate-name suffix. plane3's export carries 'gearFront.001' where plane2's
# carries 'nosewheel', so a spec names the STEM and matching strips the suffix - otherwise
# every spec has to know whether the exporter happened to collide names that day.
_DUP_SUFFIX = re.compile(r"\.\d+$")


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


class Spec(object):
    """What is true of ONE model. Everything else is mechanism.

    front_nodes / rear_nodes are MESH node name stems, not joint names, and the distinction
    has already bitten: plane2's rig has a joint 'nosewheel_steer' that owns no geometry, and
    plane3's wheel MESHES are named gearFront/gearRear_L while its JOINTS are named
    nosewheel/wheel_L. Measuring a facing off a jointless node, or off a joint with no mesh,
    silently measures the wrong thing.

    origin_on is 'rear' or 'front' or None - which reference the model's own convention puts
    the origin on. It is REPORTED rather than asserted for a model nothing drives yet: the
    cost of a wrong origin is that the agent keeps the wrong point on the painted guideline,
    which is a real defect but not a reason to refuse the asset.
    """

    def __init__(self, key, source, mesh_dir, skel_name, front_nodes, rear_nodes,
                 front_label, rear_label, origin_on, note=""):
        self.key = key
        self.source = source
        self.mesh_dir = mesh_dir
        self.skel_name = skel_name
        self.front_nodes = front_nodes
        self.rear_nodes = rear_nodes
        self.front_label = front_label
        self.rear_label = rear_label
        self.origin_on = origin_on
        self.note = note

    @property
    def pipeline_path(self):
        return "%s/PL_%s_Combine" % (self.mesh_dir, self.skel_name[3:])


def read_gltf(source):
    """The .glb's JSON chunk: a 12-byte header, then a length-prefixed chunk.

    No dependency needed, and the editor's Python has no glTF reader anyway. The export is
    read rather than described because the export is the authority on its own geometry and
    rig - a constant in this file would need re-typing every time a model moved, which is
    exactly the edit that gets missed.
    """
    try:
        with open(source, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            return json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        fail("could not read %s: %s" % (source, exc))
        return None


def joint_names(doc):
    """The joint names the .glb's skin declares.

    NOT A CONSTANT, for the reason import_plane2.py records: it had EXPECTED_BONES = 7 and
    the next export changed the rig, which would have failed an import for a change that was
    entirely intended. The check is "the skeleton has a bone per joint the skin declares".
    """
    nodes = doc.get("nodes", [])
    names = []
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            if 0 <= index < len(nodes):
                names.append(nodes[index].get("name", "?"))
    return names


def _stem(name):
    return _DUP_SUFFIX.sub("", name)


def axle_centres_uu(doc, front_nodes, rear_nodes):
    """(front, rear) X in uu, measured off the .glb's own wheel meshes.

    glTF x is UE X for every export here, and a glTF metre is 100 uu. Nodes are matched by
    name stem so a renamed rig reports a MISS rather than quietly measuring the wrong part.
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
        stem = _stem(node.get("name", ""))
        at = centre_x(index)
        if at is None:
            continue
        if stem in front_nodes:
            front.append(at)
        elif stem in rear_nodes:
            rear.append(at)

    if not front or not rear:
        return None, None
    return sum(front) / float(len(front)), sum(rear) / float(len(rear))


def source_extents_m(doc):
    """(size, min) in glTF metres, from the POSITION accessors alone.

    THE IMPORT IS CHECKED AGAINST THE FILE, not against a number typed into this script.
    import_plane2.py and import_fueltruck.py both hardcode an expected dimension, and they
    are right to: those two are wired into the sim, where FTrafficRules::VehicleFootprint and
    every clearance figure are set to a specific size, so a re-export that changes scale must
    fail loudly. Nothing drives these four yet, so there is no second figure to disagree with,
    and the invariant that actually matters here is narrower: THE IMPORT DID NOT RESCALE THE
    MODEL. That is the check Interchange's offset-scale bug (see import_fueltruck.py, where a
    skinned import scaled vertices twice and bones once) would break.

    When one of these does get wired up, the sim's figure becomes the authority and the spec
    grows a hardcoded expectation the way the other two have.
    """
    accessors = doc.get("accessors", [])
    low = [None, None, None]
    high = [None, None, None]
    for mesh in doc.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            at = primitive.get("attributes", {}).get("POSITION")
            if at is None or at >= len(accessors):
                continue
            accessor = accessors[at]
            for axis in range(3):
                mn = accessor["min"][axis]
                mx = accessor["max"][axis]
                low[axis] = mn if low[axis] is None else min(low[axis], mn)
                high[axis] = mx if high[axis] is None else max(high[axis], mx)
    if low[0] is None:
        return None, None
    return [high[i] - low[i] for i in range(3)], low


def clear_previous(mesh_dir):
    """Remove anything a previous run left, so 'what landed' means this run.

    THE WHOLE DIRECTORY, not asset by asset, which import_fueltruck.py learned the hard way:
    deleting them one at a time SILENTLY FAILS when they reference each other - the
    PhysicsAsset and the Skeleton both point at the mesh, and the mesh at its materials - so
    delete_asset returns false, nothing says so, and the survivors collide with this run's
    rename as "an asset already exists at this location". That reads as an import bug and is
    really a stale folder.

    Safe here and nowhere else: everything under mesh_dir is this script's own output. Once
    something outside the folder references these - a DA_ entity, a level - this stops being
    safe and the assets have to be re-authored in place instead. See
    Tools/Python/build_airlines.py, which load-or-creates for exactly that reason.
    """
    if not unreal.EditorAssetLibrary.does_directory_exist(mesh_dir):
        return True
    existing = unreal.EditorAssetLibrary.list_assets(mesh_dir, recursive=True)
    unreal.EditorAssetLibrary.delete_directory(mesh_dir)

    survivors = unreal.EditorAssetLibrary.list_assets(mesh_dir, recursive=True) \
        if unreal.EditorAssetLibrary.does_directory_exist(mesh_dir) else []
    if survivors:
        fail("%d of %d asset(s) survived the clear of %s: %s. Something outside the folder "
             "references them, or the editor has them open - delete them by hand and re-run."
             % (len(survivors), len(existing), mesh_dir,
                ", ".join(p.split("/")[-1] for p in survivors[:6])))
        return False
    if existing:
        say("cleared %d asset(s) from a previous run" % len(existing))
    return True


def build_pipeline_asset(spec):
    """The configured pipeline, ON DISK, because that is the only kind Interchange takes.

    THIS IS THE TRAP THAT COST import_plane2.py A RUN, repeated here because the failure is
    silent: setting AssetImportTask.options to a live UInterchangeGenericAssetsPipeline
    SUCCEEDS - the property exists and accepts the object - and is then ignored. The import
    runs with Interchange's default stack, produces one asset per node, and reports success.
    Interchange takes pipelines only through FImportAssetParameters::OverridePipelines, which
    is an array of SOFT OBJECT PATHS, so the configured pipeline must exist on disk first.
    """
    directory, name = spec.pipeline_path.rsplit("/", 1)
    # ASKED BEFORE IT IS LOADED. load_asset on a path that does not exist logs
    # "LogEditorAssetSubsystem: Error: LoadAsset failed" and returns None - which is the
    # ANSWER here, not a fault, since drop_pipeline deletes this asset at the end of every
    # run so it is absent by design. The stray Error is not harmless: the commandlet's exit
    # code reports any logged error, so four of these turned a fully passing run into exit 1
    # and made the exit code useless for telling a real failure from a normal one.
    pipeline = None
    if unreal.EditorAssetLibrary.does_asset_exist(spec.pipeline_path):
        pipeline = unreal.EditorAssetLibrary.load_asset(spec.pipeline_path)
    if pipeline is None:
        pipeline = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, directory, unreal.InterchangeGenericAssetsPipeline, None)
    if pipeline is None:
        fail("could not create the import pipeline at %s" % spec.pipeline_path)
        return None

    mesh_pipeline = pipeline.get_editor_property("mesh_pipeline")
    # BY_SKELETON gathers every skinned mesh into ONE skeletal mesh sharing the one skeleton,
    # which is what each of these exports' single rig describes. Left separate they would be
    # dozens of assets an actor has to carry and keep in step, where combined they are one
    # mesh with a MATERIAL SLOT per material - the distinction that decides how it draws.
    mesh_pipeline.set_editor_property(
        "combine_skeletal_meshes_behavior",
        unreal.InterchangeCombineSkeletalMeshesBehavior.BY_SKELETON)
    mesh_pipeline.set_editor_property("import_static_meshes", False)
    mesh_pipeline.set_editor_property("import_skeletal_meshes", True)

    # MATERIALS IN THEIR OWN SUBFOLDER, the layout Content/Aircraft/* and
    # Content/Vehicles/FuelTruck1 already use: the mesh at the root with Materials/ beside it.
    # The mesh is moved up out of SkeletalMeshes/ afterwards, since it is the one asset that
    # belongs at the root. use_source_name_for_asset off so nothing nests under a second
    # folder named after the .glb.
    pipeline.set_editor_property("use_source_name_for_asset", False)
    pipeline.set_editor_property("scene_name_sub_folder", False)
    pipeline.set_editor_property("asset_type_sub_folders", True)

    # NO ROTATION OFFSET. Every export in this batch already arrives nose on glTF +X, which
    # lands on UE +X - the convention UAircraftType documents and vehicles share. report_bounds
    # MEASURES that rather than trusting it. Note also that Interchange has no skeletal
    # equivalent of import_offset_rotation anyway: measured A/B in import_plane2.py, yaw 0 and
    # yaw -90 produced identical extents on the skeletal path, the mesh never turning.
    pipeline.set_editor_property("import_offset_rotation", unreal.Rotator(0.0, 0.0, 0.0))

    # NO SCALE OFFSET, EVER, and this is not a preference. import_fueltruck.py measured it
    # twice: import_offset_uniform_scale is applied INCONSISTENTLY to a skinned import -
    # vertices take it TWICE (once baked, once on the bind pose) and the skeleton takes it
    # ONCE - so there is no value that gets both halves right. A truck scaled here came out
    # correctly sized with a wheelbase 15% short, and a wheel whose mesh and bone disagree
    # about where it is does not stay on its axle when it turns. Resize in Blender.

    unreal.EditorAssetLibrary.save_asset(spec.pipeline_path, only_if_is_dirty=False)
    say("pipeline ready at %s" % spec.pipeline_path)
    return spec.pipeline_path


def import_mesh(spec, pipeline_path):
    if not os.path.isfile(spec.source):
        fail("missing %s" % spec.source)
        return None

    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    source_data = unreal.InterchangeManager.create_source_data(spec.source)

    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    params.override_pipelines = [unreal.SoftObjectPath(pipeline_path)]

    manager.import_asset(spec.mesh_dir, source_data, params)

    # WHAT ACTUALLY LANDED, not what was asked for - the failure that made import_plane2.py's
    # first run look successful while producing seventeen meshes.
    found = unreal.EditorAssetLibrary.list_assets(spec.mesh_dir, recursive=True)
    meshes = []
    for path in sorted(found):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        kind = type(asset).__name__ if asset else "LOAD FAILED"
        say("  %-56s %s" % (path.split(".")[0], kind))
        # EXACT class, not isinstance: the Skeleton and PhysicsAsset that come with a skeletal
        # import would otherwise be picked up too, and the first one sorted would be renamed.
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
    # rename_asset carries the mesh's material references with it.
    mesh = meshes[0]
    current = mesh.get_path_name().split(".")[0]
    wanted = "%s/%s" % (spec.mesh_dir, spec.skel_name)
    if current != wanted:
        if unreal.EditorAssetLibrary.does_asset_exist(wanted):
            unreal.EditorAssetLibrary.delete_asset(wanted)
        if unreal.EditorAssetLibrary.rename_asset(current, wanted):
            say("renamed %s -> %s" % (current.split("/")[-1], spec.skel_name))
            mesh = unreal.EditorAssetLibrary.load_asset(wanted)
        else:
            fail("could not rename %s to %s" % (current, wanted))

    # THE COMPANIONS COME TOO - the Skeleton and PhysicsAsset a skeletal import produces,
    # otherwise left under SkeletalMeshes/ named after whichever node the combine picked.
    for path in unreal.EditorAssetLibrary.list_assets(spec.mesh_dir, recursive=True):
        stem = path.split(".")[0]
        asset = unreal.EditorAssetLibrary.load_asset(path)
        suffix = None
        if isinstance(asset, unreal.Skeleton):
            suffix = "_Skeleton"
        elif isinstance(asset, unreal.PhysicsAsset):
            suffix = "_PhysicsAsset"
        if suffix is None:
            continue
        target = "%s/%s%s" % (spec.mesh_dir, spec.skel_name, suffix)
        if stem == target:
            continue
        if unreal.EditorAssetLibrary.does_asset_exist(target):
            unreal.EditorAssetLibrary.delete_asset(target)
        if unreal.EditorAssetLibrary.rename_asset(stem, target):
            say("renamed %s -> %s%s" % (stem.split("/")[-1], spec.skel_name, suffix))

    return mesh


def extents_of(mesh):
    """(min, max). USkeletalMesh has no get_bounding_box; it gives an FBoxSphereBounds."""
    bounds = mesh.get_bounds()
    origin = bounds.origin
    extent = bounds.box_extent
    return (unreal.Vector(origin.x - extent.x, origin.y - extent.y, origin.z - extent.z),
            unreal.Vector(origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))


def report_bounds(spec, mesh, doc):
    """Measure, then judge. Both, and in that order.

    Returns True when nothing FAILED, so the caller can report a per-model verdict rather
    than leaving a reader to scan for the word.
    """
    ok = True
    low, high = extents_of(mesh)
    size = [high.x - low.x, high.y - low.y, high.z - low.z]
    say("bounds X[%.1f, %.1f] Y[%.1f, %.1f] Z[%.1f, %.1f]"
        % (low.x, high.x, low.y, high.y, low.z, high.z))
    say("extents X=%.1f Y=%.1f Z=%.1f uu" % tuple(size))

    # THE IMPORT AGAINST THE FILE. glTF is Y-up and UE is Z-up, so glTF (x, y, z) arrives as
    # UE (x, z, y): the height swaps with the depth. Comparing the SORTED triples sidesteps
    # any argument about which axis went where and still catches the thing that matters - a
    # uniform or non-uniform rescale - because a rescale changes the numbers themselves.
    source_size, source_low = source_extents_m(doc)
    if source_size is None:
        fail("the .glb declares no POSITION data, so nothing here knows what size it should be")
        ok = False
    else:
        want = sorted(v * 100.0 for v in source_size)
        got = sorted(size)
        worst = max(abs(a - b) for a, b in zip(want, got))
        if worst > 5.0:
            fail("the import rescaled the model: source %s uu against imported %s uu. "
                 "Interchange's offset scale is applied twice to a skinned mesh's vertices "
                 "and once to its bones - check nothing set import_offset_uniform_scale."
                 % (["%.1f" % v for v in want], ["%.1f" % v for v in got]))
            ok = False
        else:
            say("PASS imported at the export's own size (%.2f x %.2f x %.2f m, worst axis "
                "off by %.1f uu)" % (source_size[0], source_size[1], source_size[2], worst))

    # THE PROPORTIONS, REPORTED AND NOT JUDGED. This said "the aircraft convention" when Y
    # exceeded X and "the vehicle convention" otherwise, and that was a misleading line of
    # exactly the kind this project keeps paying for: whether an airframe is longer than its
    # span is a fact about the AEROPLANE, not about how it was exported. A Dash 8-Q400 is
    # 32.5 m long and 28.2 m across, so a perfectly correct import was announced as following
    # "the vehicle convention".
    #
    # There is nothing to judge here anyway. The convention is +X forward, +Y starboard
    # (UAircraftType's documented local space, which the vehicles share), and the wheel-based
    # facing check below is what actually tests it - off the gear, which knows which end is
    # the front, rather than off a bounding box, which does not.
    say("NOTE %.1f uu along X, %.1f uu across Y" % (size[0], size[1]))

    # WHICH WAY IT FACES, FROM THE WHEELS rather than from the bounds. The bounds measure the
    # ORIGIN at least as much as the facing, and several of these models deliberately put the
    # origin at one end - import_plane2.py's old bounds-based test called a CORRECT model
    # backwards for exactly that reason. Which end is the front is a fact about the gear.
    front, rear = axle_centres_uu(doc, spec.front_nodes, spec.rear_nodes)
    if front is None:
        fail("could not find the %s / %s meshes in the export, so nothing here knows which "
             "way it faces - check the node names against the spec's front_nodes/rear_nodes"
             % (spec.front_label, spec.rear_label))
        ok = False
    elif front > rear:
        say("PASS %s %.1f uu is forward of the %s %.1f uu, so it faces +X"
            % (spec.front_label, front, spec.rear_label, rear))
    else:
        fail("%s is at %.1f uu and the %s at %.1f - the model is backwards and would travel "
             "tail-first. Check the export's final rotation."
             % (spec.front_label, front, spec.rear_label, rear))
        ok = False

    # WHERE THE ORIGIN SITS, reported rather than asserted - see Spec.origin_on. This is the
    # point ARoadAgentActor::SetPose puts on the guideline, so it IS the point the model
    # keeps on the painted line. A deviation nobody declared is a defect; a deviation stated
    # here is a number whoever wires the model up has to carry, the way UAircraftType's
    # SteerAxleX carries the Piper's.
    if front is not None and spec.origin_on:
        at = front if spec.origin_on == "front" else rear
        label = spec.front_label if spec.origin_on == "front" else spec.rear_label
        if abs(at) > 10.0:
            say("NOTE the %s is %.1f uu from the origin, not on it - whatever drives this "
                "model has to carry that offset or it will keep the wrong point on the "
                "guideline" % (label, at))
        else:
            say("PASS the origin is the %s (%.1f uu), as this model's convention states"
                % (label, at))

    # WHEELS ON THE GROUND, and a FAIL rather than a note. Two things rest on z = 0 being the
    # ground plane and both break silently otherwise: ARoadAgentActor::SetMotion applies no
    # lift, so the model floats or sinks by whatever this is; and an aircraft's pitch pivot is
    # taken at z = 0 as the main gear's CONTACT PATCH, so a model exported a metre up pitches
    # about a point a metre underground.
    if abs(low.z) > 10.0:
        fail("mesh bottom is at Z=%.1f rather than 0. The agent would float or sink. "
             "Re-export with the wheels on the ground plane." % low.z)
        ok = False
    else:
        say("PASS wheels are on the ground at Z=%.1f, so z=0 is the contact plane" % low.z)

    return ok


def report_rig(mesh, doc):
    """A bone per joint the skin declares, and the steer->roll chains named.

    COUNTING BONES IS THE CHECK, not merely finding a skeleton: a skinless import coerced
    into a skeletal mesh produces ONE root bone, binds every vertex to it, looks completely
    correct standing still, and animates nothing.
    """
    ok = True
    declared = joint_names(doc)
    if mesh.skeleton is None:
        fail("the skeletal mesh has no skeleton")
        return False
    say("skin declares %d joint(s): %s" % (len(declared), ", ".join(declared)))

    if not declared:
        fail("the .glb declares no skin - a skinless glTF yields no skeletal mesh at all, "
             "and Interchange will not invent a rig from a flat node hierarchy")
        return False

    # THROUGH A COMPONENT, because USkeletalMesh exposes no bone lookup to Python in 5.8:
    # find_bone_index is not on the asset (it reads as though it should be, and is not), and
    # a Skeleton's ReferenceSkeleton is not a UPROPERTY so the names are not reachable from
    # the asset either. A transient SkeletalMeshComponent has get_num_bones/get_bone_name and
    # needs no world to answer them.
    bones = []
    try:
        component = unreal.new_object(unreal.SkeletalMeshComponent)
        component.set_skeletal_mesh_asset(mesh)
        bones = [str(component.get_bone_name(i)) for i in range(component.get_num_bones())]
    except Exception as exc:
        say("NOTE could not read the skeleton's bones (%s) - the rig is reported from the "
            "export below and the editor-side check is SKIPPED, not passed" % exc)

    if bones:
        missing = [want for want in declared if want not in bones]
        if missing:
            fail("the skeleton is missing %d declared joint(s): %s"
                 % (len(missing), ", ".join(missing)))
            ok = False
        else:
            say("PASS all %d declared joints have bones (skeleton has %d)"
                % (len(declared), len(bones)))

    # THE STEER CHAINS ARE THE POINT of importing these skeletal at all: a steered wheel's
    # roll bone must be a CHILD of its steer bone, so the roll axis turns with the steering.
    # The parentage is read from the .glb because UE 5.8 exposes no Python reader for a
    # Skeleton's bone tree.
    nodes = doc.get("nodes", [])
    parent_of = {}
    for i, node in enumerate(nodes):
        for child in node.get("children", []):
            parent_of[child] = i
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            name = nodes[index].get("name", "?")
            # The steered wheels, by the naming both the vehicle and aircraft rigs use.
            if not (name.startswith("wheel_F") or name == "nosewheel"):
                continue
            if index not in parent_of:
                fail("%s has no parent in the export - the steer chain is broken, and the "
                     "wheel would spin about a fixed axis while the model turns" % name)
                ok = False
                continue
            parent = nodes[parent_of[index]].get("name", "?")
            if parent.startswith("steer_") or parent.endswith("_steer"):
                say("PASS chain %s <- %s" % (name, parent))
            else:
                fail("%s hangs off %s, not a steer bone - the steer chain is broken"
                     % (name, parent))
                ok = False
    return ok


def report_materials(mesh, doc):
    """The slots, whether anything is in them, and one per material the export declares."""
    ok = True
    materials = mesh.get_editor_property("materials")
    declared = [m.get("name", "?") for m in doc.get("materials", [])]
    say("material slots: %d (the export declares %d)" % (len(materials), len(declared)))
    missing = 0
    for slot in materials:
        interface = slot.material_interface
        say("  %-26s -> %s" % (slot.material_slot_name,
                               interface.get_path_name().split(".")[0] if interface else "NONE"))
        if interface is None:
            missing += 1
    if missing:
        fail("%d slot(s) have no material - the glTF material pipeline did not run" % missing)
        ok = False
    elif len(materials) < len(declared):
        fail("%d slots against %d materials in the export - a combine that merged slots "
             "would lose the livery" % (len(materials), len(declared)))
        ok = False
    else:
        say("PASS %d slots, every one carrying a material from the glTF" % len(materials))
    return ok


def flag_for_skeletal(mesh_dir):
    """bUsedWithSkeletalMesh on every generated material, SET AND SAVED HERE.

    WITHOUT THIS EVERY ONE OF THEM RENDERS AS GREY CLAY. A UMaterial with the flag false
    cannot compile for the skeletal vertex factory, and the renderer silently substitutes the
    DEFAULT material - so the component holds correct materials, logs correct names, and
    draws a grey vehicle. That is exactly how it was reported for plane2: "when the twotter
    spawned none of its materials were set".

    It hides particularly well: the editor sets the flag ITSELF on first use and recompiles,
    so the mesh looks right in the skeletal mesh editor and wrong in PIE - but that only marks
    the material DIRTY, so the fix dies with the session unless it is saved. Saved here, where
    the materials are made.

    NOTE import_fueltruck.py does NOT do this and Content/Vehicles/FuelTruck1 may carry the
    same latent defect; it is not touched from here because that would be editing an asset
    this run does not own. Worth a look the next time the truck is re-imported.
    """
    flagged = 0
    for path in unreal.EditorAssetLibrary.list_assets("%s/Materials" % mesh_dir, recursive=True):
        material = unreal.EditorAssetLibrary.load_asset(path)
        if not isinstance(material, unreal.Material):
            continue
        material.set_editor_property("used_with_skeletal_mesh", True)
        flagged += 1
    say("set the skeletal usage flag on %d material(s)" % flagged)

    # READ IT BACK, because it is the flag that turns a correct material into a grey one and
    # nothing on screen distinguishes the two until the model spawns in PIE.
    unflagged = []
    for path in unreal.EditorAssetLibrary.list_assets("%s/Materials" % mesh_dir, recursive=True):
        material = unreal.EditorAssetLibrary.load_asset(path)
        if isinstance(material, unreal.Material) and not material.get_editor_property(
                "used_with_skeletal_mesh"):
            unflagged.append(path.split("/")[-1].split(".")[0])
    if unflagged:
        fail("these would render as grey clay on a skeletal mesh: %s" % ", ".join(unflagged))
        return False
    say("PASS every material is flagged for skeletal meshes")
    return True


def save_everything(mesh_dir):
    """Forced, because save_asset does nothing for a package the editor does not think dirty.

    SAVE EVERYTHING THE IMPORT MADE, not just the mesh. Saving only the mesh left plane2's
    eight materials alive in the commandlet's memory and absent from disk: the mesh loaded
    fine in the next session, its slots pointed at packages that did not exist, and the
    aircraft rendered default grey - which looks like a material authoring problem rather
    than a missing save.
    """
    saved = 0
    for path in unreal.EditorAssetLibrary.list_assets(mesh_dir, recursive=True):
        if unreal.EditorAssetLibrary.save_asset(path.split(".")[0], only_if_is_dirty=False):
            saved += 1
    say("saved %d asset(s) under %s" % (saved, mesh_dir))
    return saved


def drop_pipeline(spec):
    """THE PIPELINE ASSET IS TOOLING, not content.

    Deleted rather than left sitting in a folder that otherwise holds only the mesh and its
    materials. build_pipeline_asset authors it deterministically, so the next run makes it
    again. (Content/Vehicles/FuelTruck1 still has its PL_ asset checked in, because
    import_fueltruck.py never removed one; not tidied from here.)
    """
    if unreal.EditorAssetLibrary.does_asset_exist(spec.pipeline_path):
        unreal.EditorAssetLibrary.delete_asset(spec.pipeline_path)
        say("removed the import pipeline asset; it is rebuilt on each run")


def import_one(spec):
    """The whole sequence for one model. Returns True when every check passed."""
    say("=" * 78)
    say("%s: importing from %s" % (spec.key, spec.source))
    if spec.note:
        say("%s: %s" % (spec.key, spec.note))

    doc = read_gltf(spec.source)
    if doc is None:
        return False
    if not clear_previous(spec.mesh_dir):
        return False
    pipeline_path = build_pipeline_asset(spec)
    if pipeline_path is None:
        return False
    mesh = import_mesh(spec, pipeline_path)
    if mesh is None:
        return False

    ok = report_bounds(spec, mesh, doc)
    ok = report_rig(mesh, doc) and ok
    ok = report_materials(mesh, doc) and ok
    ok = flag_for_skeletal(spec.mesh_dir) and ok
    save_everything(spec.mesh_dir)
    drop_pipeline(spec)

    say("%s: %s" % (spec.key, "ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    return ok
