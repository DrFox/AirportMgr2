"""Re-imports SK_Plane2 IN PLACE after the airframe was re-proportioned. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY NOT JUST RE-RUN import_plane2.py. Because it would destroy work it does not own. Its
clear_previous() deletes EVERY asset under /Game/Aircraft/Plane2, and that folder no longer
holds only the import's output - ABP_Plane2, the hand-authored animation Blueprint that turns
the props and wheels, lives there too. It would go, silently, and the run would report
success.

The references are the second half of it. ABP_Plane2, DA_Aircraft_Plane2 and M_ModelYard all
name SK_Plane2. Deleting an asset something points at breaks the reference rather than
updating it - the lesson Tools/Python/build_airlines.py records as "re-author IN PLACE" - so
even a delete that spared the Blueprint would leave it bound to nothing.

REIMPORT IS THE OPERATION THIS ACTUALLY IS. The source .glb changed; the asset did not move,
did not get renamed, and kept every one of its 16 meshes, 7 joints and 8 material slots.
UInterchangeManager::ReimportAsset updates the existing UObject in place, so its package path,
its GUID and every reference to it survive by construction rather than by luck.

WHAT CHANGED IN THE SOURCE, and why a reimport is safe rather than merely convenient:

    height       6.67 -> 5.94 m      fuselage   2.286 x 2.703 -> 1.750 x 1.900
    wing chord   2.477 -> 1.980      main wheel 1.076 -> 0.780   track 5.10 -> 3.72
    prop disc    3.091 -> 2.590

    span 19.75 and length 15.94 UNCHANGED, which is what import_plane2.py asserts and what
    every clearance figure in the sim is measured against.

The rig is untouched: same seven joints, same nosewheel_steer -> nosewheel chain. Only the
bone POSITIONS moved, which is a reimport's business and not a re-rig.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402

MESH = "/Game/Aircraft/Plane2/SK_Plane2"
SOURCE = r"C:\repos\AirportMgr2Models\plane2\export\plane2.glb"

# Assets that must still resolve afterwards. Named rather than discovered, because the point
# of the check is that THESE survived - a search that found nothing would pass silently.
DEPENDENTS = [
    "/Game/Aircraft/Plane2/ABP_Plane2",
    "/Game/Entities/DA_Aircraft_Plane2",
]

# Measured off the .glb before importing, in uu. The span is the one import_plane2.py
# asserts; the height is the one that should have CHANGED, and is here so that a reimport
# which silently did nothing is caught. A no-op reimport is the failure mode this whole
# script risks - it would report success and leave the old airframe in place.
EXPECTED_SPAN_UU = 1975.0
EXPECTED_HEIGHT_UU = 594.0
TOLERANCE_UU = 20.0

# The pipeline this script authors, uses and deletes - see reference_pose_pipeline().
PIPELINE_PATH = "/Game/Aircraft/Plane2/PL_Plane2_Reimport"

# WHERE THE BONES MUST LAND, uu, read off plane2.glb's own joints rather than believed.
#
# THE SKELETON IS A SEPARATE ASSET FROM THE MESH and does not follow it. Interchange's
# bUpdateSkeletonReferencePose defaults to FALSE - "the reference pose of the mesh is always
# updated", says the engine's own tooltip, and the SKELETON's is not - so every reimport this
# project has run updated the geometry and left the joints where they were. That is why
# SK_Plane2 carried a main-gear hub at Z 68.60 against a tyre measuring 39.05 in radius, and
# why nothing caught it: the mesh looked right, the old check measured the mesh's height, and
# the height had already been correct for a day.
EXPECTED_BONES = {
    "wheel_L": (-454.28, 186.00, 39.00),
    "wheel_R": (-454.28, -186.00, 39.00),
    "prop_L": (-103.94, 305.00, 274.89),
    "prop_R": (-103.94, -305.00, 274.89),
    "nosewheel": (0.00, 0.00, 26.50),
    "nosewheel_steer": (-1.29, 0.00, 26.50),
}
BONE_TOLERANCE_UU = 1.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def extents_of(mesh):
    bounds = mesh.get_bounds()
    origin = bounds.origin
    extent = bounds.box_extent
    return ((origin.x - extent.x, origin.y - extent.y, origin.z - extent.z),
            (origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))


def measure(mesh, when):
    low, high = extents_of(mesh)
    size = [high[i] - low[i] for i in range(3)]
    say("%s: extents X=%.1f Y=%.1f Z=%.1f uu, bottom Z=%.1f"
        % (when, size[0], size[1], size[2], low[2]))
    return size, low


def bones_of(mesh):
    """Every bone's WORLD position in the SKELETON's reference pose, uu.

    OFF THE SKELETON, not off the mesh, because they are two assets and this script exists to
    keep them in step. UAirsideAgentAnim rotates bones by name and ARoadAgentActor pitches
    about the main gear, so the skeleton's idea of where the gear is IS the aeroplane as far
    as the game is concerned - a mesh that has been updated under a stale skeleton draws in
    the right place and animates about the wrong one.
    """
    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is None:
        return {}
    pose = skeleton.get_reference_pose()
    out = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        t = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD).translation
        out[str(name)] = (t.x, t.y, t.z)
    return out


def reference_pose_pipeline():
    """The shared reimport pipeline, at this script's own path.

    ITS OWN ASSET rather than import_plane2.py's PL_Plane2_Combine, which that script deletes
    at the end of every run as "tooling, not content". Depending on a package another script
    exists to remove would work until the day somebody ran it.
    """
    return airside_import.reimport_pipeline(PIPELINE_PATH)


def drop_pipeline():
    airside_import.drop_reimport_pipeline(PIPELINE_PATH)


def check_bones(mesh, before):
    """The joints landed where the .glb puts them, and they MOVED to get there.

    BOTH HALVES MATTER. "They are where they should be" passes on an asset nothing touched if
    it was already right, and "they moved" passes on a reimport that moved them somewhere
    else. Asserting the pair is what makes this the no-op guard the height check used to be -
    and the height check is no longer one, because the height has been correct since the
    geometry was last reimported while the joints were not.
    """
    after = bones_of(mesh)
    if not after:
        fail("the reimported mesh has no skeleton to read a reference pose from")
        return False

    ok = True
    moved = 0
    for name, want in sorted(EXPECTED_BONES.items()):
        got = after.get(name)
        if got is None:
            fail("the skeleton has no %s bone - ABP_Plane2 drives it by name" % name)
            ok = False
            continue
        off = max(abs(got[i] - want[i]) for i in range(3))
        was = before.get(name)
        if was is not None and max(abs(got[i] - was[i]) for i in range(3)) > BONE_TOLERANCE_UU:
            moved += 1
        if off > BONE_TOLERANCE_UU:
            fail("bone %s is at (%.2f, %.2f, %.2f), expected (%.2f, %.2f, %.2f) from the "
                 ".glb - the skeleton's reference pose did not follow the mesh"
                 % ((name,) + got + want))
            ok = False
        else:
            say("PASS bone %-16s (%9.2f, %9.2f, %8.2f)" % ((name,) + got))

    if ok and moved == 0 and before:
        say("NOTE no bone moved - the skeleton already matched the .glb, so this run "
            "confirmed it rather than fixing it")
    elif moved:
        say("PASS %d bone(s) moved onto the .glb's joints" % moved)
    return ok


def main():
    say("=" * 78)
    if not os.path.isfile(SOURCE):
        fail("missing %s" % SOURCE)
        say("DONE")
        return

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        fail("%s does not exist - nothing to reimport" % MESH)
        say("DONE")
        return
    if not isinstance(mesh, unreal.SkeletalMesh):
        fail("%s is a %s, not a SkeletalMesh" % (MESH, type(mesh).__name__))
        say("DONE")
        return

    before, _ = measure(mesh, "before")
    bones_before = bones_of(mesh)
    for name in sorted(bones_before):
        say("  before: bone %-16s (%9.2f, %9.2f, %8.2f)" % ((name,) + bones_before[name]))

    pipeline_path = reference_pose_pipeline()
    if pipeline_path is None:
        say("DONE")
        return

    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True

    # THE SOURCE IS RESTATED rather than left to the asset's stored import data. That data
    # remembers the pipeline the first import used - PL_Plane2_Combine - which import_plane2.py
    # deliberately DELETES at the end of every run as "tooling, not content". A reimport that
    # tried to resolve it would be chasing a package that is not there.
    source_data = unreal.InterchangeManager.create_source_data(SOURCE)

    # AND THE PIPELINE IS OVERRIDDEN, which it was not before. Left to the default stack this
    # reimports the geometry and leaves the SKELETON's reference pose alone - see
    # reference_pose_pipeline and EXPECTED_BONES for what that cost.
    params.override_pipelines = [unreal.SoftObjectPath(pipeline_path)]
    try:
        manager.reimport_asset(mesh, params)
    except Exception as exc:
        drop_pipeline()
        fail("reimport_asset raised %s - falling back is NOT automatic here, because the "
             "alternative (delete and re-import) would take ABP_Plane2 with it. Re-run "
             "import_plane2.py by hand only after moving the Blueprint out." % exc)
        say("DONE")
        return

    drop_pipeline()

    # RELOAD BEFORE MEASURING. The in-memory object may be the pre-reimport one; reading the
    # package back is the only way to know what a fresh editor would see.
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    after, low = measure(mesh, "after")

    # THE AXES, STATED ONCE. A UE skeletal mesh's extents come back (X, Y, Z) = (length,
    # span, height): glTF is Y-up and UE is Z-up, so the export's height lands on Z and its
    # span on Y. Getting this backwards compared the span against the expected height and
    # declared a reimport that had plainly worked - Z went 740.0 to 594.0 in the same log -
    # to have done nothing.
    span_uu, height_uu = after[1], after[2]

    # DID IT ACTUALLY CHANGE. A reimport that quietly no-ops is this script's real risk: the
    # asset is untouched, every reference still resolves, nothing errors, and the aeroplane in
    # the game is the old fat one.
    #
    # MEASURED ON THE BONES SINCE 2026-09-19, not on the height. The height was the right
    # probe exactly once - on the run that re-proportioned the airframe - and it has been a
    # green light that measures nothing ever since: it compares this run's mesh against the
    # last run's mesh, so the second time you reimport the same .glb it reports "the reimport
    # did nothing" whether or not anything needed doing. check_bones asks a question with a
    # right answer instead: the joints are where plane2.glb puts them.
    ok = check_bones(mesh, bones_before)
    if abs(span_uu - EXPECTED_SPAN_UU) > TOLERANCE_UU:
        fail("span is %.1f uu, expected %.1f - every clearance figure in the sim is measured "
             "against this" % (span_uu, EXPECTED_SPAN_UU))
        ok = False
    else:
        say("PASS span %.1f uu is unchanged, as the re-proportion intended" % span_uu)

    if abs(height_uu - EXPECTED_HEIGHT_UU) > TOLERANCE_UU:
        fail("height is %.1f uu, expected %.1f" % (height_uu, EXPECTED_HEIGHT_UU))
        ok = False
    else:
        say("PASS height %.1f uu matches the DHC-6-300's real 5.94 m" % height_uu)

    if abs(low[2]) > 10.0:
        fail("mesh bottom is at Z=%.1f rather than 0 - the aircraft would float or sink, and "
             "the pitch pivot would be that far underground" % low[2])
        ok = False
    else:
        say("PASS wheels are on the ground at Z=%.1f" % low[2])

    # THE RIG, because a reimport can silently drop a skin and leave a one-bone skeleton that
    # looks perfect standing still and animates nothing.
    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is None:
        fail("the reimported mesh has no skeleton")
        ok = False
    else:
        try:
            component = unreal.new_object(unreal.SkeletalMeshComponent)
            component.set_skeletal_mesh_asset(mesh)
            bones = [str(component.get_bone_name(i)) for i in range(component.get_num_bones())]
        except Exception as exc:
            say("NOTE could not read the bones (%s) - the rig check is SKIPPED, not passed"
                % exc)
            bones = []
        if bones:
            want = ["root", "nosewheel_steer", "nosewheel", "prop_L", "prop_R",
                    "wheel_L", "wheel_R"]
            missing = [b for b in want if b not in bones]
            if missing:
                fail("the skeleton lost %s - ABP_Plane2 drives these by name"
                     % ", ".join(missing))
                ok = False
            else:
                say("PASS all 7 bones survived: %s" % ", ".join(bones))

    materials = mesh.get_editor_property("materials")
    empty = [str(s.material_slot_name) for s in materials if s.material_interface is None]
    if empty:
        fail("%d slot(s) lost their material: %s" % (len(empty), ", ".join(empty)))
        ok = False
    else:
        say("PASS %d material slots, all filled" % len(materials))

    # THE WHOLE POINT OF REIMPORTING RATHER THAN REPLACING: the things that pointed at this
    # asset must still point at it. Checked by loading them, because a broken reference is
    # not visible on the mesh itself.
    for path in DEPENDENTS:
        dependent = unreal.EditorAssetLibrary.load_asset(path)
        if dependent is None:
            fail("%s no longer loads - the reimport broke it" % path)
            ok = False
        else:
            say("PASS %s still loads (%s)" % (path.split("/")[-1], type(dependent).__name__))

    # SKELETAL USAGE FLAG re-asserted: a reimport can regenerate materials, and a material
    # without it renders as grey clay on a skeletal mesh while logging the correct name.
    from airside_import import flag_for_skeletal
    flag_for_skeletal("/Game/Aircraft/Plane2")

    saved = 0
    for path in unreal.EditorAssetLibrary.list_assets("/Game/Aircraft/Plane2", recursive=True):
        if unreal.EditorAssetLibrary.save_asset(path.split(".")[0], only_if_is_dirty=False):
            saved += 1
    say("saved %d asset(s)" % saved)

    say("plane2: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("=" * 78)
    # A reimport regenerates plane2's materials and reassigns its slots; without this
    # the shared M_Fleet set is silently undone for plane2 alone.
    airside_import.rebuild_fleet_materials()
    say("DONE")


main()
