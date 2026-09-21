"""Creates ABP_Plane7, parented to UAirsideAgentAnim and targeting SK_Plane7's skeleton, and
resolves the rotation axis its AnimGraph must drive each bone about. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
NOTE those lines land in the log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH. Tools/wire_plane7_anim.py authors the whole
graph over MCP against the RUNNING editor, then reads it back bone by bone. The two halves
want the editor in opposite states, which is one restart:

  1. editor CLOSED: this script, to create the asset and measure the axes
  2. editor UP:     Tools/wire_plane7_anim.py, to wire it

See docs/2026-09-20-animgraph-authoring.md for what is and is not reachable that way. State
machines remain hand work; nothing here needs one.

THE BONE PLAN IS NOT COPIED A FIFTH TIME, and that is the whole difference between this
script and the five before it. build_plane1_anim.py's header said "the fourth model is the one
that extracts rather than copies"; the fourth declined, and build_plane5_anim.py's header then
named this one in as many words. It lives in Tools/Python/airside_anim.py now, with the axis
resolver plane5 introduced - which would otherwise have been copied here in its entirety on
the same day the older duplication was paid off. What is left in this file is what is TRUE OF
plane7 AND OF NOTHING ELSE: its paths, its bones, and the two notes below.

THIRTEEN JOINTS, TWELVE DRIVEN, the same count as plane5 and arranged the same way - both
bays have doors and all four take the same BayDoorAngleDegrees. The one structural difference
is the propeller: plane5 is a twin with prop_L and prop_R, and a Meridian is a SINGLE-engine
aeroplane with one `prop`. It still contains "prop", so the shared rule drives it with no
special case; the count of bones a variable drives has never been part of the contract.

THIS RIG REPLACES SK_PiperMeridian, whose ABP was hand-wired in the editor before any of this
tooling existed and carried no record of which axis it drove or why. That is the second reason
the axes below are MEASURED rather than inherited: there is no trustworthy predecessor for
this particular aeroplane to inherit from.
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

KEY = "plane7"
SOURCE = r"C:\repos\AirportMgr2Models\plane7\export\plane7.glb"

SKELETON = "/Game/Aircraft/Plane7/SK_Plane7_Skeleton"
MESH = "/Game/Aircraft/Plane7/SK_Plane7"
ABP_PATH = "/Game/Aircraft/Plane7"
ABP_NAME = "ABP_Plane7"

# THE BONES THIS GRAPH DRIVES, and every one of them turns about ITS OWN LENGTH.
#
# That is not a transcription of a comment, it is what plane7/scripts/build_rig.py DOES. Its
# pose() keys the cycle in two lines:
#
#     for name in GEAR_BONES + DOOR_BONES:
#         pb.rotation_euler = (0.0, math.radians(deg), 0.0)   # about the bone's own Y
#
# ONE COMPONENT, THE MIDDLE ONE, POSITIVE, FOR ALL SEVEN. In Blender a pose bone's local Y IS
# the bone's length, so "every bone's LENGTH is its rotation axis" is enforced by the code
# that keys the cycle rather than merely asserted by the header above it.
#
# THE MIRRORING LIVES IN THE REST DIRECTION, NOT IN THE NUMBER. gear_L and gear_R both fold
# INBOARD and gear_nose folds AFT on the same positive value, because their bones point
# opposite ways - +Y, -Y and +X respectively. Same for the doors: the main pair close OUTBOARD
# and the nose pair INBOARD off one BayDoorAngleDegrees. plane5's King Air folds FORWARD, so
# its retract bones lie along X where these lie along Y; the engine never sees the difference
# because it rolls each bone about its own length.
#
# SO THE RIG IS UNIFORM, and the only open question in UE is which rotator component Blender's
# bone-Y became on import, and with what sign. That is measured by resolve_axis() rather than
# assumed. The whole fleet's shipped graphs say Yaw; this checks it rather than inheriting it.
DRIVEN_BONES = [
    "gear_L", "gear_R", "gear_nose",
    "wheel_L", "wheel_R", "nosewheel", "nosewheel_steer",
    "door_main_L", "door_main_R", "door_nose_L", "door_nose_R",
    "prop",
]


def report_plan():
    """The graph this asset wants, as a list rather than a memory of one.

    STILL PRINTED although the wiring is now scripted, for the reason that keeps UNRECOGNISED
    bones in the output: this is the only place the mapping is stated in English, and
    Tools/wire_plane7_anim.py's failure messages are easier to read next to it.
    """
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane7_anim.py authors it against the")
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
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane7's reference pose in UE's own")
    say("    frame. Wiring a bone about the wrong axis reads as a modelling fault, not a")
    say("    wiring one - a door that sinks into the wing - so it is measured, not assumed.")
    resolved = resolve_axis(MESH, DRIVEN_BONES)
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
    say("     parent - root, on the ground - so the prop and wheels detach and lie under")
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
        say("PASS %s targets SK_Plane7_Skeleton" % ABP_NAME)

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
        # carries - and DA_Aircraft_Plane7 points at its generated class, so a delete breaks
        # that reference rather than updating it. Tools/wire_plane7_anim.py is the idempotent
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
