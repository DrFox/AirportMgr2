"""Creates ABP_Plane6, parented to UAirsideAgentAnim and targeting SK_Plane6's skeleton, and
resolves the rotation axis its AnimGraph must drive each bone about. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
NOTE those lines land in the log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH. Tools/wire_plane6_anim.py authors the whole
graph over MCP against the RUNNING editor, then reads it back bone by bone. The two halves
want the editor in opposite states, which is one restart:

  1. editor CLOSED: this script, to create the asset and measure the axes
  2. editor UP:     Tools/wire_plane6_anim.py, to wire it

See docs/2026-09-20-animgraph-authoring.md for what is and is not reachable that way. State
machines remain hand work; nothing here needs one.

THE BONE PLAN AND THE AXIS RESOLVER LIVE IN Tools/Python/airside_anim.py, as plane5's and
plane7's do. What is left in this file is what is TRUE OF plane6 AND OF NOTHING ELSE: its
paths, its bones, and the three notes below.

SEVENTEEN DRIVEN JOINTS, THE MOST IN THE FLEET, and the count is not padding: a 777's main
leg carries a THREE-AXLE BOGIE, so there are six main wheel bones rather than two. A bone
rotates about its own length, so one bone cannot roll three axles - tyres off its line would
ORBIT it rather than spin - and plane6/scripts/build_rig.py says so in as many words. All six
take the one WheelAngleDegrees, because the count of bones a variable drives has never been
part of the contract; plane7 already proved the other end of that with a single prop.

NO TRUCK BONES, ALTHOUGH THE FLEET GREW THE VOCABULARY FOR THEM ON THIS AEROPLANE'S ACCOUNT.
BONE_RULES' `truck`/`bogie` needles and FGearPerformance::TruckTiltSeconds were both added on
2026-09-21 for this 777 and it then shipped with its bogies RIGID on their legs - so nothing
here matches either needle and TruckTiltSeconds stays zero. That is recorded rather than
quietly left, because a reader who knows the field was written for this model would otherwise
read its zero as an omission. The A380 (plane8) is where it starts earning its place.

THREE BONES ARE RAKED ON PURPOSE, which is new, and it is why resolve_axis grew a `raked`
argument rather than having its squareness check deleted. See RAKED below.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript. The commandlet
# executes the file without adding its folder the way `python foo.py` would, so the import
# below raises ModuleNotFoundError and the run dies before a single MARKER: line - which
# reads as "the commandlet did nothing" rather than as a missing path. Put it on first.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import (  # noqa: E402
    fail, joint_names, report_bone_plan, resolve_axis, say, write_axis_plan)

KEY = "plane6"
SOURCE = r"C:\repos\AirportMgr2Models\plane6\export\plane6.glb"

SKELETON = "/Game/Aircraft/Plane6/SK_Plane6_Skeleton"
MESH = "/Game/Aircraft/Plane6/SK_Plane6"
ABP_PATH = "/Game/Aircraft/Plane6"
ABP_NAME = "ABP_Plane6"

# THE BONES THIS GRAPH DRIVES, and every one of them turns about ITS OWN LENGTH.
#
# That is not a transcription of a comment, it is what plane6/scripts/build_rig.py DOES. Its
# pose() keys the cycle in two lines:
#
#     pb.rotation_mode = 'XYZ'
#     pb.rotation_euler = (0.0, math.radians(angles.get(pb.name, 0.0)), 0.0)
#
# ONE COMPONENT, THE MIDDLE ONE, FOR ALL SEVENTEEN. In Blender a pose bone's local Y IS the
# bone's length, so "every bone's LENGTH is its rotation axis" is enforced by the code that
# keys the cycle rather than merely asserted by the header above it.
#
# THE MIRRORING LIVES IN THE REST DIRECTION, NOT IN THE NUMBER, the same as plane5's and
# plane7's. gear_L points +Y and gear_R points -Y so both fold INBOARD on one GearAngleDegrees;
# gear_nose points -X and folds FORWARD - a 777's nose leg retracts forward, which is the
# opposite of plane7's aft-folding Meridian and needs no different row anywhere. All six wheel
# bones point the SAME way (+X) so that one WheelAngleDegrees spins them together rather than
# three each way.
#
# THE SENSE OF THAT ONE NUMBER IS A SEPARATE QUESTION FROM THE MIRRORING, and getting the two
# confused is what shipped plane6's gear folding OUTBOARD on its first wiring. Blender is
# right-handed and UE is left-handed, so the import mirrors every bone-local rotation: the
# model's positive is the graph's negative, for gear and doors exactly as for the wheels that
# have always carried a -1. Tools/wire_plane6_anim.py's header has the measurements and the
# control that established it.
DRIVEN_BONES = [
    "prop_L", "prop_R",
    "wheel_L1", "wheel_L2", "wheel_L3",
    "wheel_R1", "wheel_R2", "wheel_R3",
    "nosewheel", "nosewheel_steer",
    "gear_L", "gear_R", "gear_nose",
    "door_main_L", "door_main_R", "door_nose_L", "door_nose_R",
]

# THE BONES THAT ARE DELIBERATELY NOT SQUARE TO THE AIRFRAME.
#
# resolve_axis asserts that every driven bone's rotation axis lies along one of UE's own axes,
# because on the six rigs before this one it did, and a bone the rigger failed to align is a
# change to look at. plane6 is the first aeroplane here where that premise is FALSE BY DESIGN,
# so the three exceptions are declared with their reasons rather than the guard being dropped
# off the other fourteen bones:
#
#   * nosewheel_steer - raked 12.4 degrees. The leg HAS to be raked to fold forward past the
#     nose cone (plane6/scripts/build_gear.py's header argues it), and a nose leg steers about
#     its STRUT rather than about the vertical. plane4's is vertical and its steer bone is +Z;
#     this one runs axle-to-trunnion. A steer bone forced square would turn the wheel about a
#     line the strut does not lie on, which reads as the tyre scrubbing sideways.
#   * door_nose_L / door_nose_R - the nose bay doors hinge on the slanted line the bay's own
#     edge fits, because the bay is cut into a curving belly ahead of a raked leg. A square
#     hinge would swing them into the fuselage.
#
# SQUARENESS WAS NEVER A CORRECTNESS REQUIREMENT. A Transform (Modify) Bone in Bone Space
# turns a bone about its OWN axes, so the graph never asks what the world thinks. These three
# are wired exactly like the other fourteen; the only difference is that the check reports
# their angle instead of refusing them, and would fail if one of them were straightened
# without this list being updated.
RAKED = ("nosewheel_steer", "door_nose_L", "door_nose_R")


def report_plan():
    """The graph this asset wants, as a list rather than a memory of one.

    STILL PRINTED although the wiring is now scripted, for the reason that keeps UNRECOGNISED
    bones in the output: this is the only place the mapping is stated in English, and
    Tools/wire_plane6_anim.py's failure messages are easier to read next to it.
    """
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane6_anim.py authors it against the")
    say("  RUNNING editor and reads it back; this list is what it should agree with.")
    say("  One Transform (Modify) Bone per row, Rotation driven by:")
    unrecognised = report_bone_plan(joint_names(SOURCE))
    say("")
    if unrecognised:
        fail("%d joint(s) above match no rule - either the rig gained a part the sim cannot "
             "drive, or a bone was renamed" % unrecognised)
    else:
        say("PASS every joint maps to a UAirsideAgentAnim property; nothing is unwirable")

    say("")
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane6's reference pose in UE's own")
    say("    frame. Wiring a bone about the wrong axis reads as a modelling fault, not a")
    say("    wiring one - a door that sinks into the wing - so it is measured, not assumed.")
    say("    THREE BONES ARE RAKED ON PURPOSE and are reported with their angle; see RAKED.")
    resolved = resolve_axis(MESH, DRIVEN_BONES, raked=RAKED)
    for bone, component, sign in resolved:
        say("    %-16s  %-5s  x %+.0f" % (bone, component, sign))
    if len(resolved) != len(DRIVEN_BONES):
        fail("resolved %d of %d driven bones - the rest are logged above"
             % (len(resolved), len(DRIVEN_BONES)))
    else:
        say("    PASS all %d driven bones resolve to one local axis" % len(resolved))
        write_axis_plan(KEY, MESH, resolved)

    say("")
    say("  ON EVERY ONE OF THOSE NODES:")
    say("    Translation Mode = IGNORE            <- leave it alone")
    say("    Rotation Mode    = ADD TO EXISTING   <- not Replace")
    say("    Scale Mode       = IGNORE")
    say("    Rotation Space   = Bone Space")
    say("")
    say("  TWO MODES BITE, and both were got wrong once on ABP_Plane2:")
    say("   * Translation on Replace writes the default (0,0,0), snapping each bone to its")
    say("     parent - root, on the ground - so the fans and wheels detach and lie under")
    say("     the aeroplane. Their bind-pose positions ARE the spin axes.")
    say("   * Rotation on Replace discards the bone's bind-pose ORIENTATION, so every part")
    say("     reorients the same way. Add to Existing turns each part from where the rig")
    say("     put it.")
    say("")
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of.")
    say("    FCSPose::SafeSetCSBoneTransforms exists to refresh children already converted")
    say("    to component space, so rotating the PARENT last carries the rotated child with")
    say("    it. ABP_Plane1 and ABP_Plane2 both ship that order for nosewheel_steer.")
    say("    THIS RIG BINDS THE RULE HARDEST: gear_nose > nosewheel_steer > nosewheel is")
    say("    three deep, and gear_L/gear_R are the parents of THREE wheels each. Retract")
    say("    before roll and six wheels spin inside the bay instead of with it.")


def verify(asset, skeleton):
    """Check what is on disk, whether this run created it or found it.

    Run every time rather than only after creation: the asset outlives the script, and a
    reparent or a retarget done by hand in the editor is exactly the kind of change that
    should be caught here rather than by a fan that does not turn.
    """
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_Plane6_Skeleton" % ABP_NAME)

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
        # carries - and DA_Aircraft_Plane6 points at its generated class, so a delete breaks
        # that reference rather than updating it. Tools/wire_plane6_anim.py is the idempotent
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
