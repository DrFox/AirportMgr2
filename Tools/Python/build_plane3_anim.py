"""Resolves the rotation axes ABP_Plane3's AnimGraph must drive each bone about, and checks
the asset it belongs to. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.

THIS DOES NOT CREATE THE ASSET. ABP_Plane3 has existed since the Q400 was first imported and
carries a graph that drives its props, wheels and steering; what changed on 2026-09-21 is the
RIG UNDER IT. plane3/scripts/build_gear_rig.py added three retract bones and four bay-door
bones and re-parented the wheels beneath their legs, so the mesh went from 7 joints to 14 and
the graph is now missing seven of them. Tools/wire_plane3_anim.py rebuilds it from the plan
this script measures.

TWO HALVES, TWO EDITOR STATES, ONE RESTART:

  1. editor CLOSED: this script, to measure the axes
  2. editor UP:     Tools/wire_plane3_anim.py, to wire it

RUN reimport_plane3.py FIRST. This measures SK_Plane3's reference pose, and until the mesh
has been re-imported that pose has seven bones and the resolve below fails by name on the
seven it cannot find - which is the correct failure, and says which step was skipped.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript. See import_models.py
# for the full note. Put it on first.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import (  # noqa: E402
    fail, joint_names, report_bone_plan, resolve_axis, say, write_axis_plan)

KEY = "plane3"
SOURCE = r"C:\repos\AirportMgr2Models\plane3\export\plane3.glb"

SKELETON = "/Game/Aircraft/Plane3/SK_Plane3_Skeleton"
MESH = "/Game/Aircraft/Plane3/SK_Plane3"
ABP_PATH = "/Game/Aircraft/Plane3"
ABP_NAME = "ABP_Plane3"

# THIRTEEN DRIVEN JOINTS OF FOURTEEN, and seven of them are new.
#
# ALL THREE RETRACT BONES POINT -X AND ALL THREE LEGS FOLD FORWARD, which is the Q400 rather
# than a copy of anything. plane5's nose leg folds AFT so its gear_nose points +X; plane6's
# mains fold inboard about a spanwise bone. A Dash 8 folds its mains forward into the engine
# nacelles and its nose leg forward too, so all three share one rest direction and one
# GearAngleDegrees drives the lot. build_gear_rig.py's header argues it at the source.
#
# THE DOORS SPLIT THE OTHER WAY FROM THE LEGS: door_main_L/_R close OUTBOARD and
# door_nose_L/_R close INBOARD, off the single BayDoorAngleDegrees, because the two pairs
# point opposite ways at rest. The mirroring is in the bone, never in the figure - the rule
# plane4 stated and every rig since has followed.
DRIVEN_BONES = [
    "prop_L", "prop_R",
    "wheel_L", "wheel_R",
    "nosewheel", "nosewheel_steer",
    "gear_L", "gear_R", "gear_nose",
    "door_main_L", "door_main_R", "door_nose_L", "door_nose_R",
]

# NOTHING ON THIS RIG IS DECLARED RAKED, AND THE FIRST DRAFT OF THIS LINE GOT IT WRONG.
#
# plane3's door hinges ARE slightly off-axis - they are fitted to the bay edge they seal, and
# a fuselage belly curves - so all four were listed here on the strength of the axes
# gear_pivots.json publishes. Then they were measured against the threshold rather than
# against the word "raked":
#
#     door_main_L  axis (0, -0.99988, -0.01545)   0.89 deg off
#     door_nose_L  axis (0,  0.99117, -0.1326 )   7.62 deg off
#     resolve_axis's bound                        8.11 deg (squareness 0.99)
#
# Both are INSIDE it, so the strict check passes them unaided and a declaration would have
# failed as stale - which is exactly what resolve_axis's raked-but-square branch is for, and
# it caught this before the script was ever run. plane6's three bones at 11.8 and 12.4 degrees
# remain the only ones in the fleet that need naming.
#
# THE NOSE DOORS ARE HALF A DEGREE INSIDE THE BOUND, which is worth knowing: re-fit those
# hinges to a slightly deeper belly curve and they cross it, and the failure will read as a
# broken rig rather than as a tolerance. The answer then is to declare them here, not to widen
# the bound.
RAKED = ()


def report_plan():
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane3_anim.py authors it against the")
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
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane3's reference pose in UE's own")
    say("    frame. Seven of these bones did not exist before 2026-09-21, so none of them")
    say("    has an axis to inherit - they are measured, like plane5's and plane6's.")
    resolved = resolve_axis(MESH, DRIVEN_BONES, raked=RAKED)
    for bone, component, sign in resolved:
        say("    %-16s  %-5s  x %+.0f" % (bone, component, sign))
    if len(resolved) != len(DRIVEN_BONES):
        fail("resolved %d of %d driven bones - the rest are logged above. If the missing "
             "ones are the gear and doors, reimport_plane3.py has not been run and this "
             "mesh still carries the old seven-joint rig."
             % (len(resolved), len(DRIVEN_BONES)))
    else:
        say("    PASS all %d driven bones resolve to one local axis" % len(resolved))
        write_axis_plan(KEY, MESH, resolved)

    say("")
    say("  ON EVERY ONE OF THOSE NODES: Translation IGNORE, Rotation ADD TO EXISTING,")
    say("    Scale IGNORE, Rotation Space BONE SPACE. Replace on either of the first two")
    say("    detaches the part or discards its bind orientation.")
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of. THIS RIG CHANGED SHAPE")
    say("    ON 2026-09-21 AND THE ORDER CHANGED WITH IT: wheel_L and wheel_R used to hang")
    say("    off root and now hang off gear_L and gear_R, and nosewheel_steer moved under")
    say("    gear_nose. Retract before roll and the wheels spin inside the nacelle.")


def verify(asset, skeleton):
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_Plane3_Skeleton" % ABP_NAME)

    parent = unreal.BlueprintEditorLibrary.get_blueprint_parent_class(asset)
    say("parent class: %s" % parent)
    if parent is None or "AirsideAgentAnim" not in str(parent):
        fail("parent is not UAirsideAgentAnim - the graph would see none of the angles")
    else:
        say("PASS parent is UAirsideAgentAnim, so the graph sees the angles it applies")

    graphs = [str(g) for g in unreal.BlueprintEditorLibrary.list_graph_names(asset)]
    say("graphs: %s" % ", ".join(graphs))


def bone_count():
    """Say how many bones the mesh actually carries, because that is the one number that
    tells a reader whether the reimport has happened yet."""
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        fail("no %s" % MESH)
        return
    try:
        component = unreal.new_object(unreal.SkeletalMeshComponent)
        component.set_skeletal_mesh_asset(mesh)
        names = [str(component.get_bone_name(i)) for i in range(component.get_num_bones())]
    except Exception as exc:
        say("NOTE could not read the skeleton's bones (%s)" % exc)
        return
    say("SK_Plane3 carries %d bone(s): %s" % (len(names), ", ".join(names)))
    missing = [b for b in DRIVEN_BONES if b not in names]
    if missing:
        fail("the mesh has no %s - run Tools/Python/reimport_plane3.py first; this skeleton "
             "is still the pre-gear rig" % ", ".join(missing))


def run():
    skeleton = unreal.EditorAssetLibrary.load_asset(SKELETON)
    if skeleton is None:
        fail("no skeleton at %s" % SKELETON)
        say("DONE")
        return

    path = "%s/%s" % (ABP_PATH, ABP_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # NOT CREATED HERE - see the header. DA_Aircraft_Plane3 points at this Blueprint's
        # generated class, so an empty replacement would drive nothing while reporting
        # success.
        fail("no %s. This script measures an asset that already exists; restore it from git "
             "rather than letting a fresh empty Blueprint take its place." % path)
        say("DONE")
        return

    bone_count()
    verify(asset, skeleton)
    report_plan()
    say("DONE")


run()
