"""What build_aircraft_anim.py (and the vehicles' build_rig_anim.py/build_vehicle_anims.py)
need and none of them should own a copy of.

Imported from the editor's Python, so `import unreal` is available. THE SCRIPT'S OWN
DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py's header for the
full note - so every caller puts it there before importing this.

WHY THIS FILE EXISTS, AND WHY IT IS NOT airside_import.py. Two promises made in two headers
came due on the same model:

  * plane1's own script (since retired - see Issue #294): "the fourth model is the one that
    extracts rather than copies". The fourth declined.
  * plane5's own script, more specifically: "THE SIXTH MODEL SHOULD EXTRACT. Said here
    rather than in a commit message nobody greps: one bone_plan() in airside_import.py, four
    callers."

plane7 is the sixth, so this is that extraction - but it lands HERE rather than in
airside_import.py, which is the one thing that note got wrong. airside_import.py is the
IMPORT mechanism: Interchange pipelines, the rename-and-move-up, the bounds and rig and
material checks. Anim authoring is a different job against different assets at a different
point in the pipeline, and the only reason the note named that file is that in September 2026
it was the only shared module there was. A second one is cheaper than a module that does two
jobs, which is the argument Present/ already makes about ARoadNetworkActor.

WHAT MOVED, AND WHAT IT COST. Four copies of bone_plan, five of joint_names, and one copy of
the axis resolver that plane7 would have made two of. The resolver was written for plane5 on
2026-09-20 and is the newest and least-copied of the three; extracting it now is the whole
point, because the alternative was a second copy of two hundred lines on the same day the
first duplication was being paid off.

THE CALLERS KEEP THEIR OWN FACTS. A model's SOURCE, MESH and ABP name are facts about that
model and stay in its own `aircraft/<key>.py` spec (or Tools/Python/build_rig_anim.py's own
table, for the vehicles), where somebody changing an export will be standing. What is here is
the MECHANISM - every line of it was identical across the copies, and the two that were not
identical had drifted rather than diverged on purpose. DRIVEN_BONES stopped being one of the
callers' own facts on 2026-09-26 (Issue #294): `driven_bone_plan()` below derives it, and its
chain order, from the .glb itself.
"""
import json
import math
import os
import re
import struct

import unreal


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


# ---------------------------------------------------------------------------- the .glb


def joint_names(source):
    """The joint names a .glb's skin declares, read from the file itself.

    A glb is a 12-byte header then a length-prefixed JSON chunk; no dependency needed, and the
    editor's Python has no glTF reader anyway.

    FIVE IDENTICAL COPIES BEFORE THIS ONE - four anim scripts and airside_import.py's own,
    which takes an already-parsed doc rather than a path and so cannot simply be called from
    here. That one stays: it is reading a document the import already has open, and making it
    re-read the file to share this function would be the tail wagging the dog.
    """
    try:
        with open(source, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            doc = json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        fail("could not read the skin from %s: %s" % (source, exc))
        return []

    nodes = doc.get("nodes", [])
    names = []
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            if 0 <= index < len(nodes):
                names.append(nodes[index].get("name", "?"))
    return names


# ------------------------------------------------------------------------- the bone plan


# THE RULES, IN ORDER, MOST SPECIFIC FIRST. A bone is matched by SUBSTRING on its lowered
# name, so the ordering is the whole of the decision - see bone_plan below for why each row
# sits where it does. A list rather than an if-chain because the ORDER is the contract, and a
# list makes the contract one thing to read rather than a shape to infer from control flow.
BONE_RULES = [
    ("prop",  "PropAngleDegrees"),
    ("steer", "SteerAngleDegrees"),
    ("truck", "TruckTiltAngleDegrees"),
    ("bogie", "TruckTiltAngleDegrees"),
    ("wheel", "WheelAngleDegrees"),
    ("gear",  "GearAngleDegrees"),
    ("door",  "BayDoorAngleDegrees"),
]

UNRECOGNISED = "?  UNRECOGNISED - nothing in UAirsideAgentAnim drives it"


def bone_plan(names):
    """(bone, variable) per joint, root excluded: which UAirsideAgentAnim property drives it.

    The NAMES come from the .glb; the MAPPING is the decision this function owns. A joint that
    matches no rule is reported rather than skipped silently - an unrecognised bone is either
    a rig the sim does not know how to drive yet, or a typo, and both want saying.

    THIS LIST MUST AGREE WITH UAirsideAgentAnim'S PROPERTY NAMES and there is no compiler to
    check it - see CLAUDE.md, "check where a list is CONSUMED". Since 2026-09-21 there IS a
    checker one step down the pipeline: `Tools/wire_plane_anim.py <key> --verify` reads the
    compiled graph back and fails on any bone whose driver disagrees.

    THE FOUR COPIES HAD ALREADY DRIFTED, exactly the way plane1's own script (since retired -
    see Issue #294) predicted they would: the gear and door rules landed in plane2's script on
    2026-09-19 and were typed into plane1's copy separately. The reasons those copies carried
    are kept below, because they are what the ORDERING rests on and the ordering is the only
    thing here that can be wrong in a way nothing catches:

      * STEER BEFORE WHEEL, and this one is load-bearing rather than merely disciplined: every
        aircraft rig here has a 'nosewheel_steer', which contains both needles. The wheel rule
        winning would tell someone to wire a STEERING bone to the ROLL angle, which spins the
        nose gear about its own strut and reads on screen as a broken castor. fueltruck1's
        'steer_FL' does not collide, and its copy kept the order anyway, because the next
        vehicle's bone may well be named the aircraft way and the failure is silent.
      * GEAR AFTER WHEEL. On plane5's and plane7's rigs the retract bones are the PARENTS of
        the rolling ones, so a wrong winner here retracts the aeroplane every time it rolls
        forward.
      * TRUCK BEFORE BOTH OF THOSE, added 2026-09-21 for plane6's 777 - which then shipped
        with no truck bones, so the first rig to match these two needles will be the A380's
        (plane8). The rule stays: it costs nothing on a rig with no such bone, and the whole
        point of BONE_RULES is that a rig which GROWS a part gets picked up instead of
        reported unrecognised, which is the argument the last bullet below already makes.
        A wide-body main leg
        is three bones deep - gear_L > truck_L > wheel_L1..L3 - and the middle one is the only
        bone in this fleet whose plausible names collide with TWO other rules at once: a rig
        that called it `gear_truck_L` would be read as a retract bone and one that called it
        `truck_wheel_L` as a rolling one. Both failures are silent and both look like
        modelling faults on screen. Ordering it above the pair costs nothing, because no rule
        below it can match a name containing "truck" that was not meant for it.
      * TWO NEEDLES, ONE VARIABLE, for the truck. Boeing say TRUCK and Airbus say BOGIE, the
        fleet will eventually carry both, and a rigger reaching for the word their drawing
        uses should not have their bone come out UNRECOGNISED. Checked against the one rig
        that could have collided before this went in: fueltruck1 drives `wheel_*` and
        `steer_*` and has no bone containing either needle, so no existing plan changes -
        verified joint by joint rather than assumed, which is the check GEAR AFTER WHEEL above
        records having skipped once.
      * EVERY RULE IS OFFERED TO EVERY RIG, including rules no bone of that rig can match.
        plane1's copy kept gear and door for a 172 whose gear is welded on, so that its plan
        could not quietly become a different plan from plane2's; fueltruck1's copy omitted
        prop, gear and door altogether. Unifying the two changes no rig's plan today - checked
        joint by joint across all four rigs rather than assumed - and it means a rig that
        grows a door gets picked up instead of reported unrecognised.
      * A STALE NOTE IN A LIST NOBODY RE-READS IS HOW A FEATURE SHIPS SWITCHED OFF. plane2's
        copy carried "(NOT YET in UAirsideAgentAnim - leave this bone unwired until it is)"
        beside the steer rule for long after the property existed, so the plan was telling its
        reader to leave a working bone unwired. One copy is one place for that to go stale.
    """
    plan = []
    for name in names:
        lowered = name.lower()
        if lowered == "root":
            continue
        for needle, variable in BONE_RULES:
            if needle in lowered:
                plan.append((name, variable))
                break
        else:
            plan.append((name, UNRECOGNISED))
    return plan


def report_bone_plan(names):
    """Print the plan and return the number of joints nothing drives."""
    unrecognised = 0
    for bone, variable in bone_plan(names):
        say("    %-16s  %s" % (bone, variable))
        if variable.startswith("?"):
            unrecognised += 1
    return unrecognised


def driven_bone_plan(source, excluded=()):
    """(bone, variable) for every joint bone_plan matches a rule to, in CHAIN ORDER, less any
    name in `excluded` - Issue #294's replacement for the fourteen hand-typed DRIVEN_BONES
    lists build_plane<N>_anim.py used to carry.

    CHAIN ORDER, DERIVED RATHER THAN TYPED. joint_names() returns a .glb skin's joints in the
    order Blender's exporter writes them, which is a PREORDER walk of the armature - a parent
    is always visited before its whole subtree, because that is what "walk down from the
    root, describing each node before its children" means. Reversing a preorder walk always
    yields an ordering where every node comes after all of its own descendants (run the same
    argument backwards: the parent that preceded its subtree in the original list follows it
    in the reversed one), which is exactly wire_anim_lib's own rule - "a bone goes AFTER every
    bone it is the PARENT of". Checked against all fourteen rigs' actual .glb joint arrays,
    including plane8's three-deep bogie chains (gear > bogie > wheel) and plane6's/plane11's
    three-axle bogies, before this replaced the fourteen typed lists: every one of them agrees
    in MEMBERSHIP, and where the reversed order does not reproduce the exact permutation a
    typed list was free to choose within one rank (props and doors hang unconstrained off
    root, so their relative order was always arbitrary), the result is still a valid chain -
    see this PR's harness table.

    UNRECOGNISED JOINTS ARE DROPPED HERE rather than failed - report_bone_plan(), called
    first by every caller, already prints and fails on them against the FULL, unfiltered
    joint list; a second failure over the filtered one would be the same fact from two
    functions.

    `excluded` IS build_rig_anim.py's carve-out, by name: a bone whose name matches a
    BONE_RULES needle as a false substring (that script's fifth_wheel/kingpin) is named here,
    in the one spec that knows its rig is the exception, rather than teaching BONE_RULES a
    rig's naming. No aircraft needs it today.
    """
    reversed_names = list(reversed(joint_names(source)))
    return [(bone, variable) for bone, variable in bone_plan(reversed_names)
            if not variable.startswith("?") and bone not in excluded]


def raked_line(raked):
    """The say() line reporting how many bones a rig deliberately rakes off-square - derived
    from len(raked) rather than typed, which is Issue #294's fix for plane14's OWN say-line
    lie: its RAKED tuple was correctly empty (plane14's nosewheel_steer measures square) but
    its say() line read "ONE BONE IS RAKED ON PURPOSE" anyway, copied from plane9's/plane12's
    and never re-typed when RAKED was. A line computed from the data it describes cannot
    drift from it a second time."""
    if not raked:
        return "NO BONE IS RAKED."
    if len(raked) == 1:
        return "ONE BONE IS RAKED ON PURPOSE."
    return "%s BONES ARE RAKED ON PURPOSE." % len(raked)


def anim_multiplier_for(bone, variable, overrides):
    """The multiplier Tools/wire_plane_anim.py's PLAN row takes for one bone - the fleet's
    convention, unless `overrides` (an AircraftSpec's anim_multiplier) names this bone
    specifically.

    THE CONVENTION, established across the whole fleet before plane8: -1.0 on every
    travelling or spinning bone, and None (the raw angle, undriven) on the one bone whose
    variable is SteerAngleDegrees, because ABP_Plane2's shipped graph drives its steer bone
    with a plain GetSteerAngleDegrees() and no negation. plane8's five gear bones and four
    bogie bones do not fold by the fleet's one figure each - see aircraft/plane8.py - and are
    named in `overrides` instead of bending this rule to fit one rig.

    THE -1 IS A MIRROR, NOT A SIGN CONVENTION, and it was MEASURED rather than argued -
    plane6's retired wiring script recorded the probe (its own header is gone with it since
    Issue #294; the finding is not): Blender is right-handed and UE is left-handed, so import
    mirrors every bone-local rotation. Turning SK_Plane6's gear_L by its OWN authored +90
    lifted wheel_L2 494 uu OUTBOARD - through the wing it retracts into - against
    plane6/scripts/gear_pivots.json's own retracted pose, which says +90 about world +Y puts
    the wheel 4.94 m up INBOARD. The probe was validated first against the one bone whose
    direction is known correct on screen (with -1 the wheels roll FORWARD on every rig;
    without it, backwards), and AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe now pins the
    fold direction so this cannot regress silently again.
    """
    if bone in overrides:
        return overrides[bone]
    return None if variable == "SteerAngleDegrees" else -1.0


# ----------------------------------------------------------------------- the rotation axis


# A Transform (Modify) Bone applied in Bone Space takes an FRotator whose three components
# turn about the bone's own local axes: Roll is local X, Pitch local Y, Yaw local Z.
COMPONENT_OF_LOCAL_AXIS = {0: "Roll", 1: "Pitch", 2: "Yaw"}

# The axle runs ACROSS the aeroplane. This is the anchor the whole resolution hangs on and the
# one rotation axis in a rig that is knowable without reading anybody's comment: wheel_L and
# wheel_R sit either side of the centreline, so whatever they turn about lies along UE's Y.
AXLE_UE = (0.0, 1.0, 0.0)

# The left main wheel, in the two spellings this fleet uses. ONE a side up to plane5, and
# wheel_L1..L<N> once a leg carries a bogie - plane6's 777 has three axles a side and
# therefore no bone called `wheel_L` at all.
_AXLE_ANCHOR = re.compile(r"^wheel_L\d*$")


def axle_anchor(bones):
    """The bone whose local axle anchors the whole rig's rotation axis, or None.

    WHY A FUNCTION AND NOT THE LITERAL "wheel_L" IT REPLACED. resolve_axis needs ONE bone it
    can be sure turns about UE's Y, and every left main wheel qualifies - they are parallel
    by construction, riding one axle beam. Which one is therefore arbitrary, and hard-coding
    the single-axle spelling made "this rig has one main wheel a side" a silent precondition
    of wiring any AnimGraph. plane6 is where that came due: its bogie's wheels are
    wheel_L1..L3, and the KeyError would have read as a missing bone rather than as an
    assumption about how many wheels an aeroplane has.

    THE LOWEST-NUMBERED, so the choice is a rule rather than whatever sorted() happened to
    put first. `wheel_L` itself sorts ahead of `wheel_L1` on the same rule, which is what a
    single-axle rig wants and what keeps plane1, plane2, plane3, plane4, plane5 and plane7
    resolving against exactly the bone they resolved against before.

    NOT THE RIGHT one, deliberately: the sign convention below is carried from SK_Plane2's
    LEFT wheel, and anchoring on the other side would flip it.
    """
    candidates = sorted(name for name in bones if _AXLE_ANCHOR.match(name))
    return candidates[0] if candidates else None


# How nearly two steering bones must point the same way before one's SIGN may be carried to
# the other. See _sign_against_reference.
#
# 0.85 IS 32 DEGREES, AND IT WAS 0.99 - ONE DEGREE - UNTIL plane6. The tighter figure was not
# measuring what the check is for. The question asked here is a SIGN: does this rig's steering
# bone point broadly WITH plane2's or AGAINST it, so that plane2's plain positive carries over
# or has to be negated. Any pair short of perpendicular answers that; what the guard is really
# there to refuse is a rig whose steer bone is oriented on some different principle, where the
# dot is near zero and the sign is a coin toss.
#
# A ONE-DEGREE BAND MADE A MODELLING FACT INTO A WIRING REFUSAL. plane6's nose leg is raked
# 12.4 degrees - it has to be, to fold past the nose cone, and a nose leg steers about its
# strut - so it agrees with plane2's vertical strut at 0.977 and was refused for it. The angle
# is LOGGED at every run whatever it is, which is the part that would actually catch a rig
# drifting; a threshold that fires on a correct model teaches its reader to widen it, and the
# next widening is the one that admits a genuinely wrong sign.
STEER_AGREEMENT = 0.85

# The rig a new one is checked against. SK_Plane2 is a SHIPPED, HAND-WIRED, KNOWN-GOOD asset -
# ABP_Plane2 has driven the Twin Otter in game for weeks - and it carries the three bone
# classes every aeroplane here shares with it: a propeller, a rolling wheel and a steering
# bone.
#
# NOT SK_Plane4, although plane4 is the first rig with gear and doors. ABP_Plane4 was
# duplicated from ABP_Plane2 in the editor and retargeted by hand, so it is not independent
# evidence; and plane4's own bind orientations are not uniform the way plane5's and plane7's
# are - measured 2026-09-21, its two main gear bones resolve onto OPPOSITE local axes.
# Checking against it would be checking against the less trustworthy of the two.
REFERENCE_MESH = "/Game/Aircraft/Plane2/SK_Plane2"


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
    pose. {} with a logged failure if the asset is not there.

    FAILS LOUDLY IF THE POSE IS SHORT OF THE SKELETON'S OWN BONE COUNT (found 2026-09-24, task
    3b fix round 1): unreal.AnimPose.get_bone_names(skeleton.get_reference_pose()) silently
    dropped SK_Utility1's un-skinned 'hitch' bone - 8 of its 9 declared bones came back, no
    error - while a transient SkeletalMeshComponent's get_bone_name(i) saw all 9. A caller
    asking resolve_axis for that missing bone's axis got "no bone named hitch" two frames away
    with nothing here explaining why; a caller asking for a bone that WAS present, expecting a
    dict of every driven bone, would have no way to notice one was quietly missing at all.
    Checked here, once, against the component's own count - the same mechanism
    airside_import.report_rig already uses to read bone NAMES - rather than trusted.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    if mesh is None:
        fail("no %s" % mesh_path)
        return {}
    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is None:
        fail("%s has no skeleton" % mesh_path)
        return {}
    pose = skeleton.get_reference_pose()
    names = unreal.AnimPose.get_bone_names(pose)

    component = unreal.new_object(unreal.SkeletalMeshComponent)
    component.set_skeletal_mesh_asset(mesh)
    declared = component.get_num_bones()
    if len(names) != declared:
        fail("%s: AnimPose's reference pose carries %d bone(s), the skeleton declares %d - "
             "at least one bone (typically an un-skinned socket) is being silently dropped "
             "from this function's result - see bone_frames' own comment (2026-09-24)."
             % (mesh_path, len(names), declared))
        return {}

    frames = {}
    for name in names:
        transform = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        frames[str(name)] = _local_axes(transform.rotation)
    return frames


def resolve_axis(mesh_path, driven_bones, raked=()):
    """(bone, rotator component, sign) for every driven bone, MEASURED - or [] on failure.

    WHY THIS IS MEASURED AND NOT TYPED. Every wiring script in this project before plane5 took
    "Yaw, in Bone Space" on faith, on the strength of wire_fueltruck_anim.py's note that "the
    rigger orients each bone so that its own Z is the axis it is meant to turn about". That
    note is TRUE OF THE RIGS IT WAS WRITTEN AGAINST and it is a property of those assets, not
    a law. It is exactly the shape of claim CLAUDE.md says to distrust: a statement ABOUT a
    mechanism standing in for the mechanism. plane5 was also the first rig where a single
    wrong axis would be easy to miss - four doors and three legs, most of them small and most
    of the time stowed - and plane7 has the same thirteen.

    THE ANCHOR IS THE AXLE - see AXLE_UE and axle_anchor. Whichever of the left main wheel's
    own local axes lies along UE Y is the local axis this rig rotates about, and since each
    build_rig.py poses every bone about the same one, it is the local axis for all of them.

    THE UNIFORMITY IS CHECKED RATHER THAN TRUSTED: every driven bone's chosen axis must lie
    along one of UE's own axes to within about a degree. A bone whose rotation axis points
    somewhere diagonal is a bone the rigger did not align, and on a rig built from one table
    and posed by one line that is a change to look at rather than a rounding.

    `raked` IS HOW A RIGGER SAYS "I MEANT THAT ONE", and plane6 is what forced it to exist.
    Squareness is NOT a correctness requirement - a Transform (Modify) Bone in Bone Space
    turns the bone about its OWN axes, so the graph never asks what the world thinks - it is
    a MODELLING sanity check, and it was written against six rigs that happened to be square
    everywhere. A 777 is not: its nose leg is raked 12.4 degrees so it can fold past the nose
    cone, and a nose leg steers about its STRUT rather than about the vertical, so
    nosewheel_steer is diagonal ON PURPOSE. Its two bay doors hinge on a slanted line for the
    same reason. Deleting the check to admit them would have taken the guard off the other
    fourteen bones of the same rig.

    SO A NAMED BONE IS REPORTED WITH ITS ANGLE INSTEAD OF FAILED, and a named bone that turns
    out to be SQUARE fails - a stale declaration is how a guard quietly stops guarding, and
    this one would go stale the moment a rig is straightened. Both directions are checked.

    THE SIGN COMES FROM A SHIPPED GRAPH. ABP_Plane2 drives nosewheel_steer with a plain
    GetSteerAngleDegrees() on Yaw and no negation, so a steer bone whose local axis points the
    same way as plane2's takes the same plain positive. This is the one thing that cannot be
    derived from the new rig alone: a sign is only meaningful against a convention, and the
    convention lives in the assets that already work.
    """
    label = mesh_path.rsplit("/", 1)[-1]
    frames = bone_frames(mesh_path)
    if not frames:
        return []

    missing = [bone for bone in driven_bones if bone not in frames]
    if missing:
        fail("%s has no %s - the rig was renamed. Bones: %s"
             % (label, ", ".join(missing), ", ".join(sorted(frames))))
        return []

    anchor = axle_anchor(driven_bones)
    if anchor is None:
        fail("no left main wheel among the driven bones, so there is no axle to anchor the "
             "rotation axis on. Bones offered: %s" % ", ".join(driven_bones))
        return []

    index, along = _closest_axis(frames[anchor], AXLE_UE)
    if abs(along) < 0.99:
        fail("%s's axle is not along UE Y - its closest local axis is %s at %.2f. Either "
             "the bone is not on the axle, or the import reoriented the rig."
             % (anchor, "XYZ"[index], along))
        return []
    component = COMPONENT_OF_LOCAL_AXIS[index]
    say("    the rig turns about each bone's local %s (%s), anchored on %s's axle lying "
        "along UE Y at %.3f" % ("XYZ"[index], component, anchor, along))

    sign = _sign_against_reference(frames, index, label)
    if sign is None:
        return []

    rows = []
    for bone in driven_bones:
        axis = frames[bone][index]
        squareness = max(abs(value) for value in axis)
        declared = bone in raked
        if squareness >= 0.99 and declared:
            fail("%s is declared raked and its local %s is square to the airframe after all "
                 "(%.3f along one of UE's axes). The declaration is stale - drop it, or the "
                 "next bone the rigger tilts by accident is admitted without a word."
                 % (bone, "XYZ"[index], squareness))
            continue
        if squareness < 0.99 and not declared:
            fail("%s's local %s points (%.2f, %.2f, %.2f), which is along none of UE's axes. "
                 "A driven bone on this rig should be square to the airframe, or be named in "
                 "this model's `raked` list with the reason it is not."
                 % (bone, "XYZ"[index], axis[0], axis[1], axis[2]))
            continue
        if declared:
            # REPORTED WITH ITS ANGLE, not merely permitted. The figure is the only thing that
            # would show a raked bone drifting - a hinge re-fitted to a different slope still
            # lands in the list and would otherwise pass in silence.
            say("    NOTE %s is raked %.1f deg off UE's axes, as declared (local %s points "
                "(%.2f, %.2f, %.2f)) - it turns about its own length either way"
                % (bone, math.degrees(math.acos(min(1.0, squareness))), "XYZ"[index],
                   axis[0], axis[1], axis[2]))
        rows.append((bone, component, sign))
    return rows


def _sign_against_reference(frames, index, label):
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
        fail("SK_Plane2's wheel_L turns about its local %s and %s's about its local %s. That "
             "rig does not follow the fleet's bone convention, so wiring it like the others "
             "would turn every part about the wrong axis."
             % ("XYZ"[ref_index], label, "XYZ"[index]))
        return None
    say("    PASS SK_Plane2's wheel_L resolves onto local %s too (%.3f) - same convention"
        % ("XYZ"[ref_index], ref_along))

    ours = frames["nosewheel_steer"][index]
    theirs = reference["nosewheel_steer"][index]
    agreement = sum(a * b for a, b in zip(ours, theirs))
    if abs(agreement) < STEER_AGREEMENT:
        fail("nosewheel_steer's local %s points %.2f away from SK_Plane2's - the two steering "
             "bones are not oriented alike, so plane2's sign convention cannot be carried "
             "over" % ("XYZ"[index], agreement))
        return None
    sign = 1.0 if agreement > 0 else -1.0
    say("    PASS nosewheel_steer agrees with SK_Plane2's to %.3f (%.1f deg apart), so the "
        "fleet's signs carry over (x %+.0f)"
        % (agreement, math.degrees(math.acos(min(1.0, abs(agreement)))), sign))
    return sign


def axis_plan_path(key):
    """Saved/<key>_axis_plan.json, beside the log this script's output lands in."""
    return os.path.join(unreal.Paths.project_saved_dir(), "%s_axis_plan.json" % key)


def write_axis_plan(key, mesh_path, rows):
    """Hand the resolved graph - `rows` of (bone, variable, component, sign, multiplier), in
    CHAIN ORDER - to Tools/wire_plane_anim.py rather than making it type any of it.

    ONE LIST, WHICH IS CLAUDE.md'S RULE AND NOT A CONVENIENCE, and Issue #294 widened it: this
    file used to carry only the component and sign resolve_axis() measures, while the bone,
    its variable and its wiring multiplier lived a SECOND TIME in each Tools/wire_plane<N>_anim
    .py's own hand-typed PLAN - two lists that had to agree, with no compiler between them and
    a failure mode (a part driven by the wrong property, or turning about the wrong axis) that
    looks like a modelling bug rather than a wiring one. Now every column of the row a wiring
    run needs lives in the one file the build step writes, and Tools/wire_plane_anim.py reads
    it whole rather than carrying a table of its own.

    IN Saved/ AND THEREFORE NOT COMMITTED, deliberately. It is derived, it is worthless against
    a different export, and a stale copy is exactly the "half-run pipeline" that
    Airside.Content.FootprintMatchesTheMesh exists to catch elsewhere. Re-running the
    commandlet is step one of wiring the graph; the file is how step one talks to step two.
    """
    path = axis_plan_path(key)
    payload = {
        "mesh": mesh_path,
        "bones": [{"bone": bone, "variable": variable, "component": component,
                   "sign": sign, "multiplier": multiplier}
                  for bone, variable, component, sign, multiplier in rows],
    }
    try:
        with open(path, "w") as handle:
            json.dump(payload, handle, indent=2)
    except IOError as exc:
        fail("could not write %s: %s" % (path, exc))
        return
    say("wrote %s - Tools/wire_plane_anim.py %s reads its whole graph from this" % (path, key))
