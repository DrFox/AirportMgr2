"""Creates ABP_Plane5, parented to UAirsideAgentAnim and targeting SK_Plane5's skeleton, and
resolves the rotation axis its AnimGraph must drive each bone about. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
NOTE those lines land in the log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH, and unlike build_plane1_anim.py and
build_plane2_anim.py that is no longer where the story ends. Tools/wire_plane5_anim.py
authors the whole graph over MCP against the RUNNING editor, then reads it back bone by bone.
The two halves want the editor in opposite states, which is one restart:

  1. editor CLOSED: this script, to create the asset and measure the axes
  2. editor UP:     Tools/wire_plane5_anim.py, to wire it

See docs/2026-09-20-animgraph-authoring.md for what is and is not reachable that way. State
machines remain hand work; nothing here needs one.

THE BONE PLAN IS COPIED A FIFTH TIME AND THAT IS A DELIBERATE, RECORDED DEBT.
build_plane1_anim.py's header says in as many words: "the fourth model is the one that
extracts rather than copies". This is the fifth, and the extraction was declined AGAIN - on
purpose, and the reason is worth stating so the next reader is not deciding it blind.

  * The rule this copy breaks is CLAUDE.md's "lists that must agree are ONE list", and it is
    the same list four times: build_plane1_anim.py, build_plane2_anim.py,
    build_fueltruck_anim.py and this.
  * The prediction in plane1's header HAS ALREADY COME TRUE ONCE. The gear and door rules
    landed in build_plane2_anim.py on 2026-09-19 and were typed into plane1's copy
    separately.
  * What makes the copy survivable today is that NOTHING IN THIS PLAN IS THE AUTHORITY ANY
    MORE. Tools/wire_plane5_anim.py wires the graph and then READS IT BACK off the compiled
    asset; a plan that disagreed with UAirsideAgentAnim would fail there rather than print a
    wrong instruction for a human to follow. The plan below is a REPORT, and a wrong report
    is cheaper than a wrong graph.
  * THE SIXTH MODEL SHOULD EXTRACT. Said here rather than in a commit message nobody greps:
    one bone_plan() in airside_import.py, four callers.

FOURTEEN JOINTS, THE MOST OF ANY AEROPLANE IN THE FLEET - thirteen of them driven. plane4 has
twelve and plane1 has six. What plane5 adds over plane4 is MAIN-GEAR DOORS: the 737's mains
sit in a well behind a fixed fairing, so only its nose bay is modelled, where a King Air's
mains retract forward into each engine nacelle behind doors of their own. All four doors take
the same BayDoorAngleDegrees, which is what the model's sequencing already expects.
"""
import json
import os
import struct

import unreal

SOURCE = r"C:\repos\AirportMgr2Models\plane5\export\plane5.glb"

SKELETON = "/Game/Aircraft/Plane5/SK_Plane5_Skeleton"
MESH = "/Game/Aircraft/Plane5/SK_Plane5"
ABP_PATH = "/Game/Aircraft/Plane5"
ABP_NAME = "ABP_Plane5"


def bone_plan():
    """(bone, variable) per joint the export declares, root excluded.

    The NAMES come from the .glb; the MAPPING is the decision this script owns. A joint that
    matches neither rule is reported rather than skipped silently - an unrecognised bone is
    either a rig the sim does not know how to drive yet, or a typo, and both want saying.

    THIS LIST MUST AGREE WITH UAirsideAgentAnim'S PROPERTY NAMES and there is no compiler to
    check it - see CLAUDE.md, "check where a list is CONSUMED". Since 2026-09-21 there IS a
    checker one step down the pipeline: Tools/wire_plane5_anim.py --verify reads the compiled
    graph back and fails on any bone whose driver disagrees.

    EVERY plane5 JOINT MATCHES A RULE, which is the first time that has been true of a new rig
    on its first run. plane4's five retract and hinge bones were correctly reported
    UNRECOGNISED when it was imported, because gear/door did not exist yet.
    """
    plan = []
    for name in joint_names():
        lowered = name.lower()
        if lowered == "root":
            continue
        if "prop" in lowered:
            plan.append((name, "PropAngleDegrees"))
        elif "steer" in lowered:
            # BEFORE the wheel rule, because 'nosewheel_steer' matches both and the wheel rule
            # would tell someone to wire a STEERING bone to the ROLL angle - which spins the
            # nose gear about the strut and looks like a broken castor.
            plan.append((name, "SteerAngleDegrees"))
        elif "wheel" in lowered:
            plan.append((name, "WheelAngleDegrees"))
        elif "gear" in lowered:
            # AFTER the wheel rule. On THIS rig the ordering finally does work rather than
            # merely being disciplined: plane5 is the first rig where the retract bones
            # (gear_L, gear_R, gear_nose) are the PARENTS of the rolling ones, so a wrong
            # winner here retracts the aeroplane every time it rolls forward.
            plan.append((name, "GearAngleDegrees"))
        elif "door" in lowered:
            # ALL FOUR, and they are not a copy of plane4's two. door_main_L/_R are new in this
            # rig; they close OUTBOARD where the nose pair close INBOARD, and the mirroring
            # lives in each bone's rest direction rather than in the number - see
            # plane5/scripts/build_rig.py. So one variable drives four doors correctly.
            plan.append((name, "BayDoorAngleDegrees"))
        else:
            plan.append((name, "?  UNRECOGNISED - nothing in UAirsideAgentAnim drives it"))
    return plan


# THE BONES THIS GRAPH DRIVES, and every one of them turns about ITS OWN LENGTH.
#
# That is not a transcription of a comment, it is what plane5/scripts/build_rig.py DOES. Its
# pose() is three lines:
#
#     for name in ("gear_L", "gear_R", "gear_nose"):
#         rig.pose.bones[name].rotation_euler = (0, a, 0)
#     for name in ("door_main_L", "door_main_R", "door_nose_L", "door_nose_R"):
#         rig.pose.bones[name].rotation_euler = (0, d, 0)
#
# ONE COMPONENT, THE MIDDLE ONE, POSITIVE, FOR ALL SEVEN. In Blender a pose bone's local Y IS
# the bone's length, so "every bone's LENGTH is its rotation axis" is enforced by the code
# that keys the cycle rather than merely asserted by the header above it - and pose_check()
# then folds the gear and reports where each wheel and each door edge lands, which is how the
# SIGNS were settled. The mirroring lives in the rest direction: gear_L and gear_R fold
# FORWARD and gear_nose folds AFT on the same positive value, because their bones point
# opposite ways.
#
# SO THE RIG IS UNIFORM, and the only open question in UE is which rotator component Blender's
# bone-Y became on import, and with what sign. That is measured by resolve_axis() rather than
# assumed. The whole fleet's shipped graphs say Yaw; this checks it rather than inheriting it.
DRIVEN_BONES = [
    "gear_L", "gear_R", "gear_nose",
    "wheel_L", "wheel_R", "nosewheel", "nosewheel_steer",
    "door_main_L", "door_main_R", "door_nose_L", "door_nose_R",
    "prop_L", "prop_R",
]

# A Transform (Modify) Bone applied in Bone Space takes an FRotator whose three components
# turn about the bone's own local axes: Roll is local X, Pitch local Y, Yaw local Z.
COMPONENT_OF_LOCAL_AXIS = {0: "Roll", 1: "Pitch", 2: "Yaw"}

# The rig this one is checked against. SK_Plane2 is a SHIPPED, HAND-WIRED, KNOWN-GOOD asset -
# ABP_Plane2 has driven the Twin Otter in game for weeks - and it carries the three bone
# classes plane5 shares with it: a propeller, a rolling wheel and a steering bone.
#
# NOT SK_Plane4, although plane4 is the rig with gear and doors. ABP_Plane4 was duplicated
# from ABP_Plane2 in the editor and retargeted by hand, so it is not independent evidence; and
# plane4's own bind orientations are not uniform the way plane5's are - measured 2026-09-21,
# its two main gear bones resolve onto OPPOSITE local axes. Checking against it would be
# checking against the less trustworthy of the two.
REFERENCE_MESH = "/Game/Aircraft/Plane2/SK_Plane2"

# The axle runs ACROSS the aeroplane. This is the anchor the whole resolution hangs on and the
# one rotation axis in the rig that is knowable without reading anybody's comment: wheel_L and
# wheel_R sit at y -2.6 m and +2.6 m, so whatever they turn about lies along UE's Y.
AXLE_UE = (0.0, 1.0, 0.0)


def _local_axes(quat):
    """The bone's own X, Y and Z, in the space the quaternion is expressed in.

    BY HAND rather than through MathLibrary, because this is nine lines of arithmetic against
    an API whose exact Python spelling would have to be looked up, and a wrong guess here
    fails at the one point where a wrong answer is indistinguishable from a right one.
    """
    x, y, z, w = quat.x, quat.y, quat.z, quat.w
    return (
        (1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)),
        (2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)),
        (2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)),
    )


def _closest_axis(frame, want):
    """(index, dot) of the frame axis most nearly parallel to `want`."""
    best, best_dot = 0, 0.0
    for index, axis in enumerate(frame):
        dot = sum(a * b for a, b in zip(axis, want))
        if abs(dot) > abs(best_dot):
            best, best_dot = index, dot
    return best, best_dot


def bone_frames(mesh_path):
    """{bone: (localX, localY, localZ)} in COMPONENT space, off a skeletal mesh's reference
    pose. {} with a logged failure if the asset is not there."""
    mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    if mesh is None:
        fail("no %s" % mesh_path)
        return {}
    pose = mesh.get_editor_property("skeleton").get_reference_pose()
    frames = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        transform = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        frames[str(name)] = _local_axes(transform.rotation)
    return frames


def resolve_axis():
    """(bone, rotator component, sign) for every driven bone, MEASURED - or [] on failure.

    WHY THIS IS MEASURED AND NOT TYPED. Every wiring script in this project so far takes
    "Yaw, in Bone Space" on faith, on the strength of wire_fueltruck_anim.py's note that "the
    rigger orients each bone so that its own Z is the axis it is meant to turn about". That
    note is TRUE OF THE RIGS IT WAS WRITTEN AGAINST and it is a property of those assets, not
    a law. It is exactly the shape of claim CLAUDE.md says to distrust: a statement ABOUT a
    mechanism standing in for the mechanism. plane5 is also the first rig where a single wrong
    axis would be easy to miss - four doors and three legs, most of them small and most of the
    time stowed.

    THE ANCHOR IS THE AXLE - see AXLE_UE. Whichever of wheel_L's own local axes lies along
    UE Y is the local axis this rig rotates about, and since build_rig.py poses every bone
    about the same one, it is the local axis for all thirteen.

    THE UNIFORMITY IS CHECKED RATHER THAN TRUSTED: every driven bone's chosen axis must lie
    along one of UE's own axes to within about a degree. A bone whose rotation axis points
    somewhere diagonal is a bone the rigger did not align, and on a rig built from one table
    and posed by one line that is a change to look at rather than a rounding.

    THE SIGN COMES FROM A SHIPPED GRAPH. ABP_Plane2 drives nosewheel_steer with a plain
    GetSteerAngleDegrees() on Yaw and no negation, so a plane5 steer bone whose local axis
    points the same way as plane2's takes the same plain positive. This is the one thing that
    cannot be derived from plane5 alone: a sign is only meaningful against a convention, and
    the convention lives in the assets that already work.
    """
    frames = bone_frames(MESH)
    if not frames:
        return []

    missing = [bone for bone in DRIVEN_BONES if bone not in frames]
    if missing:
        fail("SK_Plane5 has no %s - the rig was renamed. Bones: %s"
             % (", ".join(missing), ", ".join(sorted(frames))))
        return []

    index, along = _closest_axis(frames["wheel_L"], AXLE_UE)
    if abs(along) < 0.99:
        fail("wheel_L's axle is not along UE Y - its closest local axis is %s at %.2f. Either "
             "the bone is not on the axle, or the import reoriented the rig."
             % ("XYZ"[index], along))
        return []
    component = COMPONENT_OF_LOCAL_AXIS[index]
    say("    the rig turns about each bone's local %s (%s), anchored on wheel_L's axle lying "
        "along UE Y at %.3f" % ("XYZ"[index], component, along))

    sign = _sign_against_reference(frames, index)
    if sign is None:
        return []

    rows = []
    for bone in DRIVEN_BONES:
        axis = frames[bone][index]
        if max(abs(value) for value in axis) < 0.99:
            fail("%s's local %s points (%.2f, %.2f, %.2f), which is along none of UE's axes. "
                 "A driven bone on this rig should be square to the airframe."
                 % (bone, "XYZ"[index], axis[0], axis[1], axis[2]))
            continue
        rows.append((bone, component, sign))
    return rows


def _sign_against_reference(frames, index):
    """+1.0 or -1.0, from SK_Plane2's steering bone; None on a disagreement worth stopping for.

    A MISSING REFERENCE IS NOT FATAL. plane2 may simply not be imported in this checkout, and
    refusing to author an asset over that would be refusing for want of unrelated content -
    the shape AircraftFieldLengthTest's skip already takes. It is reported, loudly, because an
    unchecked sign is the one thing here running on the fleet's habit rather than on evidence.
    """
    reference = bone_frames(REFERENCE_MESH)
    if not reference:
        say("    NOTE %s is absent, so the SIGN could not be checked against a shipped rig; "
            "taking the fleet's +1. Import plane2 and re-run to close this."
            % REFERENCE_MESH)
        return 1.0

    for bone in ("wheel_L", "nosewheel_steer"):
        if bone not in reference:
            fail("%s has no %s to check against" % (REFERENCE_MESH, bone))
            return None

    ref_index, ref_along = _closest_axis(reference["wheel_L"], AXLE_UE)
    if ref_index != index:
        fail("SK_Plane2's wheel_L turns about its local %s and SK_Plane5's about its local "
             "%s. plane5 does not follow the fleet's bone convention, so wiring it like the "
             "others would turn every part about the wrong axis."
             % ("XYZ"[ref_index], "XYZ"[index]))
        return None
    say("    PASS SK_Plane2's wheel_L resolves onto local %s too (%.3f) - same convention"
        % ("XYZ"[ref_index], ref_along))

    ours = frames["nosewheel_steer"][index]
    theirs = reference["nosewheel_steer"][index]
    agreement = sum(a * b for a, b in zip(ours, theirs))
    if abs(agreement) < 0.99:
        fail("nosewheel_steer's local %s points %.2f away from SK_Plane2's - the two steering "
             "bones are not oriented alike, so plane2's sign convention cannot be carried "
             "over" % ("XYZ"[index], agreement))
        return None
    sign = 1.0 if agreement > 0 else -1.0
    say("    PASS nosewheel_steer agrees with SK_Plane2's to %.3f, so the fleet's signs carry "
        "over (x %+.0f)" % (agreement, sign))
    return sign


def axis_plan_path():
    """Saved/plane5_axis_plan.json, beside the log this script's output lands in."""
    return os.path.join(unreal.Paths.project_saved_dir(), "plane5_axis_plan.json")


def write_axis_plan(resolved):
    """Hand the resolved axes to Tools/wire_plane5_anim.py rather than making it type them.

    ONE LIST, WHICH IS CLAUDE.md'S RULE AND NOT A CONVENIENCE. The bone, its variable, its
    rotator component and its sign are four columns of one table, and the first two are
    knowable from the .glb while the last two are only knowable from the imported skeleton.
    Split across two files they are two lists that must agree, with no compiler between them
    and a failure mode - a part turning about the wrong axis - that looks like a modelling bug
    rather than a wiring one. So the half that is MEASURED is written by the script that
    measured it, and the wiring script refuses to run without it.

    IN Saved/ AND THEREFORE NOT COMMITTED, deliberately. It is derived, it is worthless against
    a different export, and a stale copy is exactly the "half-run pipeline" that
    Airside.Content.FootprintMatchesTheMesh exists to catch elsewhere. Re-running this
    commandlet is step one of wiring the graph; the file is how step one talks to step two.
    """
    path = axis_plan_path()
    payload = {
        "mesh": MESH,
        "bones": [{"bone": bone, "component": component, "sign": sign}
                  for bone, component, sign in resolved],
    }
    try:
        with open(path, "w") as handle:
            json.dump(payload, handle, indent=2)
    except IOError as exc:
        fail("could not write %s: %s" % (path, exc))
        return
    say("wrote %s - Tools/wire_plane5_anim.py reads its rotation axes from this" % path)


def joint_names():
    """The joint names the .glb's skin declares, read from the file itself.

    A glb is a 12-byte header then a length-prefixed JSON chunk; no dependency needed, and the
    editor's Python has no glTF reader anyway.
    """
    try:
        with open(SOURCE, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            doc = json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        unreal.log_error("MARKER: FAIL could not read the skin from %s: %s" % (SOURCE, exc))
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


def report_plan():
    """The graph this asset wants, as a list rather than a memory of one.

    STILL PRINTED although the wiring is now scripted, for the reason that keeps UNRECOGNISED
    bones in the output: this is the only place the mapping is stated in English, and
    Tools/wire_plane5_anim.py's failure messages are easier to read next to it.
    """
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane5_anim.py authors it against the")
    say("  RUNNING editor and reads it back; this list is what it should agree with.")
    say("  One Transform (Modify) Bone per row, Rotation driven by:")
    unrecognised = 0
    for bone, variable in bone_plan():
        say("    %-16s  %s" % (bone, variable))
        if variable.startswith("?"):
            unrecognised += 1
    say("")
    if unrecognised:
        fail("%d joint(s) above match no rule - either the rig gained a part the sim cannot "
             "drive, or a bone was renamed" % unrecognised)
    else:
        say("PASS every joint maps to a UAirsideAgentAnim property; nothing is unwirable")

    say("")
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane5's reference pose in UE's own")
    say("    frame. Wiring a bone about the wrong axis reads as a modelling fault, not a")
    say("    wiring one - a door that sinks into the wing - so it is measured, not assumed.")
    resolved = resolve_axis()
    for bone, component, sign in resolved:
        say("    %-16s  %-5s  x %+.0f" % (bone, component, sign))
    if len(resolved) != len(DRIVEN_BONES):
        fail("resolved %d of %d driven bones - the rest are logged above"
             % (len(resolved), len(DRIVEN_BONES)))
    else:
        say("    PASS all %d driven bones resolve to one local axis" % len(resolved))
        write_axis_plan(resolved)

    say("")
    say("  ON EVERY ONE OF THOSE NODES:")
    say("    Translation Mode = IGNORE            <- leave it alone")
    say("    Rotation Mode    = ADD TO EXISTING   <- not Replace")
    say("    Scale Mode       = IGNORE")
    say("    Rotation Space   = Bone Space")
    say("")
    say("  TWO MODES BITE, and both were got wrong once on ABP_Plane2:")
    say("   * Translation on Replace writes the default (0,0,0), snapping each bone to its")
    say("     parent - root, on the ground - so the props and wheels detach and lie under")
    say("     the aeroplane. Their bind-pose positions ARE the spin axes.")
    say("   * Rotation on Replace discards the bone's bind-pose ORIENTATION, so every part")
    say("     reorients the same way. Add to Existing turns each part from where the rig")
    say("     put it.")
    say("")
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of.")
    say("    FCSPose::SafeSetCSBoneTransforms exists to refresh children already converted")
    say("    to component space, so rotating the PARENT last carries the rotated child with")
    say("    it. ABP_Plane1 and ABP_Plane2 both ship that order for nosewheel_steer.")
    say("    THIS RIG BINDS THE RULE TWICE: gear_nose > nosewheel_steer > nosewheel is three")
    say("    deep, and gear_L/gear_R are the parents of wheel_L/wheel_R. Retract before roll")
    say("    and the wheels spin inside the bay instead of with it.")


def verify(asset, skeleton):
    """Check what is on disk, whether this run created it or found it.

    Run every time rather than only after creation: the asset outlives the script, and a
    reparent or a retarget done by hand in the editor is exactly the kind of change that
    should be caught here rather than by a propeller that does not turn.
    """
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_Plane5_Skeleton" % ABP_NAME)

    # THROUGH BlueprintEditorLibrary, because UAnimBlueprint does not expose parent_class as a
    # readable editor property - get_editor_property raises on it.
    parent = unreal.BlueprintEditorLibrary.get_blueprint_parent_class(asset)
    say("parent class: %s" % parent)
    if parent is None or "AirsideAgentAnim" not in str(parent):
        fail("parent is not UAirsideAgentAnim - the graph would see none of the angles")
    else:
        say("PASS parent is UAirsideAgentAnim, so the graph sees the angles it applies")

    graphs = [str(g) for g in unreal.BlueprintEditorLibrary.list_graph_names(asset)]
    say("graphs: %s" % ", ".join(graphs))


def run():
    skeleton = unreal.EditorAssetLibrary.load_asset(SKELETON)
    if skeleton is None:
        fail("no skeleton at %s - run import_models.py first" % SKELETON)
        say("DONE")
        return

    path = "%s/%s" % (ABP_PATH, ABP_NAME)
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing is not None:
        # LOAD, never replace. Deleting and recreating this asset throws away whatever graph it
        # carries - and DA_Aircraft_Plane5 points at its generated class, so a delete breaks
        # that reference rather than updating it. Tools/wire_plane5_anim.py is the idempotent
        # half: it rebuilds the GRAPH inside this asset without replacing it.
        say("%s already exists; leaving it alone so its authored graph survives" % path)
        verify(existing, skeleton)
        report_plan()
        say("DONE")
        return

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    # THE PARENT IS THE POINT. UAirsideAgentAnim is what computes PropAngleDegrees,
    # WheelAngleDegrees, GearAngleDegrees, BayDoorAngleDegrees, bAirborne and GroundSpeed from
    # the agent's state; an Anim Blueprint left on plain UAnimInstance compiles, runs, and
    # exposes none of them.
    factory.set_editor_property("parent_class", unreal.AirsideAgentAnim)

    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ABP_NAME, ABP_PATH, unreal.AnimBlueprint, factory)
    if asset is None:
        fail("could not create %s" % path)
        say("DONE")
        return

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # Read back from disk, because an asset that reports success and saves nothing is this
    # project's most familiar failure.
    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    if reloaded is None:
        fail("%s did not survive the save" % path)
        say("DONE")
        return

    say("created %s" % path)
    verify(reloaded, skeleton)
    report_plan()
    say("DONE")


run()
