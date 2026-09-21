"""Resolves the rotation axes ABP_Plane4's AnimGraph must drive each bone about, and checks
the asset it belongs to. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.

THIS ONE DOES NOT CREATE THE ASSET, and it is the only build_plane<N>_anim.py that never
could. ABP_Plane4 predates this tooling: it was DUPLICATED from ABP_Plane2 in the editor on
2026-09-19 and retargeted by hand, so it has existed for days and nothing recorded which axis
it drove, in which order, or why. What this script adds is the half that was missing - the
MEASUREMENT - so that Tools/wire_plane4_anim.py can rebuild the graph from evidence rather
than from whatever the duplication happened to carry.

WHY IT WAS WRITTEN ON 2026-09-21 AND NOT BEFORE. plane4's gear folded OUTBOARD, through the
wing it retracts into, and had done since the day it was wired; nothing in the editor drove a
retract until PR #250's animation bench, so nobody had seen it. plane6's 777 showed the same
fault, the probe that found it was pointed at the rest of the fleet, and plane4 was the one
aircraft that could not simply have a -1 added to a table - because it had no table. See
Tools/wire_plane4_anim.py.

NOT A NEW CONVENTION FOR plane4, THE FLEET'S. airside_anim.py's REFERENCE_MESH note says
plane4 "is not independent evidence" and that its bind orientations are less uniform than
plane5's and plane7's - which is a reason to MEASURE this rig rather than to inherit from it,
and is exactly what resolve_axis does. If a bone here turns out not to be square to the
airframe, it is named in RAKED with the reason, as plane6's three are.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript. See import_models.py
# for the full note. Put it on first.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import (  # noqa: E402
    fail, joint_names, report_bone_plan, resolve_axis, say, write_axis_plan)

KEY = "plane4"
SOURCE = r"C:\repos\AirportMgr2Models\plane4\export\plane4.glb"

SKELETON = "/Game/Aircraft/Plane4/SK_Plane4_Skeleton"
MESH = "/Game/Aircraft/Plane4/SK_Plane4"
ABP_PATH = "/Game/Aircraft/Plane4"
ABP_NAME = "ABP_Plane4"

# ELEVEN DRIVEN JOINTS OF TWELVE. The mains have NO DOORS and want none - their wheels sit in
# a well behind a fixed fairing, which is why the rig has door_nose_L/_R and no door_main_*
# pair. build_plane4_type.py's GEAR block records the same fact from the other side.
DRIVEN_BONES = [
    "prop_L", "prop_R",
    "wheel_L", "wheel_R",
    "nosewheel", "nosewheel_steer",
    "gear_L", "gear_R", "gear_nose",
    "door_nose_L", "door_nose_R",
]

# Nothing on this rig is deliberately off-axis - unlike plane6, whose raked nose leg and
# slanted bay hinges are named here. Left empty and NOT deleted, so that a rig which grows a
# raked bone has an obvious place to declare it rather than a check to weaken.
RAKED = ()


def report_plan():
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane4_anim.py authors it against the")
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
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane4's reference pose in UE's own")
    say("    frame. This rig's graph was duplicated by hand and never measured; these are")
    say("    the figures the rebuild uses instead of that inheritance.")
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
    say("  ON EVERY ONE OF THOSE NODES: Translation IGNORE, Rotation ADD TO EXISTING,")
    say("    Scale IGNORE, Rotation Space BONE SPACE. Replace on either of the first two")
    say("    detaches the part or discards its bind orientation - both were got wrong once")
    say("    on ABP_Plane2, which is the asset THIS one was duplicated from.")
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of, so gear_L follows")
    say("    wheel_L and gear_nose follows nosewheel_steer follows nosewheel.")


def verify(asset, skeleton):
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_Plane4_Skeleton" % ABP_NAME)

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
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # DELIBERATELY NOT CREATED HERE. Every other rig's script creates its ABP because it
        # ran before the asset existed; this one runs years after. An ABP_Plane4 that is
        # MISSING is a checkout problem or a deletion, and inventing an empty one would point
        # DA_Aircraft_Plane4 at a Blueprint that drives nothing while reporting success.
        fail("no %s. This script measures an asset that already exists; it does not create "
             "one. Restore it from git rather than letting a fresh empty Blueprint take its "
             "place." % path)
        say("DONE")
        return

    verify(asset, skeleton)
    report_plan()
    say("DONE")


run()
