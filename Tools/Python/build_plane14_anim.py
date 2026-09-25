"""Creates ABP_Plane14, parented to UAirsideAgentAnim and targeting SK_Plane14's skeleton, and
resolves the rotation axis its AnimGraph must drive each bone about. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
NOTE those lines land in the log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH. Tools/wire_plane14_anim.py authors the whole
graph over MCP against the RUNNING editor, then reads it back bone by bone. The two halves
want the editor in opposite states, which is one restart:

  1. editor CLOSED: this script, to create the asset and measure the axes
  2. editor UP:     Tools/wire_plane14_anim.py, to wire it

THE BONE PLAN AND THE AXIS RESOLVER LIVE IN Tools/Python/airside_anim.py. What is left here
is what is TRUE OF plane14 AND OF NOTHING ELSE: its paths, its bones, and the notes below.

ELEVEN DRIVEN JOINTS OF TWELVE - plane9's set WITHOUT the main-gear doors. The Phenom's mains
fold into the belly fairing with no door bone: their leg doors (maindoor_L/_R) are MESHES
skinned to gear_L/_R and ride the leg. Only the clamshell nose doors are bones.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see
# import_models.py's header. Put it on before importing the shared module.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import (  # noqa: E402
    fail, joint_names, report_bone_plan, resolve_axis, say, write_axis_plan)

KEY = "plane14"
SOURCE = r"C:\repos\AirportMgr2Models\plane14\export\plane14.glb"

SKELETON = "/Game/Aircraft/Plane14/SK_Plane14_Skeleton"
MESH = "/Game/Aircraft/Plane14/SK_Plane14"
ABP_PATH = "/Game/Aircraft/Plane14"
ABP_NAME = "ABP_Plane14"

# THE BONES THIS GRAPH DRIVES, and every one turns about ITS OWN LENGTH - plane14's
# build_rig.py keys its cycle about pose-bone Y, as plane9's does. The mirroring lives in the
# rest direction (gear_L and gear_R point opposite ways, so one figure folds both inboard); the
# handedness flip lives in wire_plane14_anim.py's multiplier column.
DRIVEN_BONES = [
    "prop_L", "prop_R",
    "wheel_L", "wheel_R", "nosewheel", "nosewheel_steer",
    "gear_L", "gear_R", "gear_nose",
    "door_nose_L", "door_nose_R",
]

# NO BONE IS DELIBERATELY RAKED. resolve_axis fails a bone more than about 8 degrees off square
# that is not declared here, and fails a declared one that turns out square, so the list is
# checked both ways every run - this empty tuple is a measured claim, not a default.
RAKED = ()


def report_plan():
    """The graph this asset wants, as a list rather than a memory of one."""
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane14_anim.py authors it against the")
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
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane14's reference pose in UE's own")
    say("    frame. Wiring a bone about the wrong axis reads as a modelling fault, not a")
    say("    wiring one, so it is measured, not assumed. ONE BONE IS RAKED ON PURPOSE.")
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
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of: gear_L > wheel_L,")
    say("    gear_R > wheel_R, gear_nose > nosewheel_steer > nosewheel. Wheels first.")


def verify(asset, skeleton):
    """Check what is on disk, whether this run created it or found it."""
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_Plane14_Skeleton" % ABP_NAME)

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
        # LOAD, never replace: DA_Aircraft_Plane14 points at this asset's generated class,
        # and Tools/wire_plane14_anim.py rebuilds the GRAPH inside it without replacing it.
        say("%s already exists; leaving it alone so its authored graph survives" % path)
        verify(existing, skeleton)
        report_plan()
        say("DONE")
        return

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    # THE PARENT IS THE POINT: UAirsideAgentAnim is what computes the angles; an Anim
    # Blueprint left on plain UAnimInstance compiles, runs, and exposes none of them.
    factory.set_editor_property("parent_class", unreal.AirsideAgentAnim)

    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ABP_NAME, ABP_PATH, unreal.AnimBlueprint, factory)
    if asset is None:
        fail("could not create %s" % path)
        say("DONE")
        return

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # Read back from disk: an asset that reports success and saves nothing is this
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
