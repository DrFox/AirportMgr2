"""Creates ABP_Plane8, parented to UAirsideAgentAnim and targeting SK_Plane8's skeleton, and
resolves the rotation axis its AnimGraph must drive each bone about. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
NOTE those lines land in the log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH. Tools/wire_plane8_anim.py authors the whole
graph over MCP against the RUNNING editor, then reads it back bone by bone. The two halves
want the editor in opposite states, which is one restart:

  1. editor CLOSED: this script, to create the asset and measure the axes
  2. editor UP:     Tools/wire_plane8_anim.py, to wire it

THE BONE PLAN AND THE AXIS RESOLVER LIVE IN Tools/Python/airside_anim.py, as plane5's,
plane6's and plane7's do. What is left here is what is TRUE OF plane8 AND OF NOTHING ELSE:
its paths, its bones, and the notes below.

THIRTY-FIVE DRIVEN JOINTS, TWICE plane6's SEVENTEEN, and every one of them is a part that
moves on the aeroplane: four fans, ten main axles on four bogies plus the nose pair, the
steer bone, four BOGIE BEAMS, five retract bones and ten bay doors. Nothing here is padding
and nothing is missing - plane8/scripts/build_rig.py asserts every bone carries a mesh and
every mesh a bone, and matches every name against the same rules BONE_RULES applies.

THIS IS THE RIG THE TRUCK VOCABULARY WAS WAITING FOR. BONE_RULES' `truck`/`bogie` needles
and FGearPerformance::TruckTiltSeconds were added on 2026-09-21 for plane6's 777, which then
shipped its bogies rigid; every header on that path names the A380 as the model that would
use them. It does: bogie_wing_L/R and bogie_body_L/R sit between each leg and its wheels,
gear > bogie > wheel, the chain UAirsideAgentAnim::TruckTiltAngleDegrees describes. The
measured tilt is 0 on the wing bogies and 54.72 on the body bogies, whose beams
counter-rotate to lie level in the bay - see wire_plane8_anim.py for how one
TruckTiltedAngleDegrees drives both.

FIVE RETRACT BONES, THREE FOLD ANGLES. The nose folds forward 77.66, the wing gear inboard
90.00, the body gear AFT 54.72, each solved against the skin in the models repo; one
GearRetractedAngleDegrees cannot fit all three and the wiring script's multiplier column is
where that is reconciled. Nothing about it changes what this script measures.

THREE BONES ARE RAKED ON PURPOSE - the same three as plane6's, for the same reasons. See
RAKED.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see
# import_models.py's header. Put it on before importing the shared module.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import (  # noqa: E402
    fail, joint_names, report_bone_plan, resolve_axis, say, write_axis_plan)

KEY = "plane8"
SOURCE = r"C:\repos\AirportMgr2Models\plane8\export\plane8.glb"

SKELETON = "/Game/Aircraft/Plane8/SK_Plane8_Skeleton"
MESH = "/Game/Aircraft/Plane8/SK_Plane8"
ABP_PATH = "/Game/Aircraft/Plane8"
ABP_NAME = "ABP_Plane8"

# THE BONES THIS GRAPH DRIVES, and every one of them turns about ITS OWN LENGTH - enforced by
# the one line plane8/scripts/build_rig.py keys its cycle with, exactly as plane6's is:
#
#     pb.rotation_euler = (0.0, math.radians(angles.get(pb.name, 0.0)), 0.0)
#
# THE MIRRORING LIVES IN THE REST DIRECTION, NOT IN THE NUMBER. gear_wing_L points +Y and
# gear_wing_R -Y so both fold INBOARD on one figure; gear_body_L/R BOTH point +X because the
# body gear folds AFT on both sides, which is the same world sense; gear_nose points -X and
# folds forward. All ten main wheel bones point +X so one WheelAngleDegrees rolls them
# together. The bogie bones point along their own pivot axle, signed so the solved tilt is a
# positive turn - +X on the wing bogies, -X on the body bogies.
#
# THE SENSE OF EACH NUMBER IS A SEPARATE QUESTION, and Tools/wire_plane8_anim.py carries it
# in its multiplier column: Blender is right-handed and UE is left-handed, so the import
# mirrors every bone-local rotation and the model's positive is the graph's negative.
DRIVEN_BONES = [
    "prop_L_in", "prop_L_out", "prop_R_in", "prop_R_out",
    "wheel_L1", "wheel_L2", "wheel_L3", "wheel_L4", "wheel_L5",
    "wheel_R1", "wheel_R2", "wheel_R3", "wheel_R4", "wheel_R5",
    "nosewheel", "nosewheel_steer",
    "bogie_wing_L", "bogie_wing_R", "bogie_body_L", "bogie_body_R",
    "gear_wing_L", "gear_wing_R", "gear_body_L", "gear_body_R", "gear_nose",
    "door_nose_L", "door_nose_R",
    "door_wing_L_in", "door_wing_L_out", "door_wing_R_in", "door_wing_R_out",
    "door_body_L_in", "door_body_L_out", "door_body_R_in", "door_body_R_out",
]

# THE BONES THAT ARE DELIBERATELY NOT SQUARE TO THE AIRFRAME - plane6's three, for plane6's
# reasons, measured on this rig rather than assumed to carry over:
#
#   * nosewheel_steer - raked 12.3 degrees: the leg runs from the axle at station 4.972 up
#     to the trunnion at 5.672, and a nose leg steers about its STRUT. Its local axis is
#     (0, 0.21, 0.98), 0.977 along UE's up - under resolve_axis's 0.99 line by design.
#   * door_nose_L / door_nose_R - the nose bay doors hinge on the line build_bays.py FITS to
#     the belly under a 4.2 m panel, which drops 0.75 m along its length; the axis is
#     (0, 0.989, -0.148), 8.5 degrees off level. Every OTHER door's fitted axis is within
#     3.1 degrees of level and 0.9985 or better along UE's axis, which is square.
#
# A stale declaration is how a guard stops guarding: resolve_axis FAILS a bone named here
# that turns out square, and one not named here that turns out raked.
RAKED = ("nosewheel_steer", "door_nose_L", "door_nose_R")


def report_plan():
    """The graph this asset wants, as a list rather than a memory of one."""
    say("")
    say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane8_anim.py authors it against the")
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
    say("  THE ROTATION AXIS PER BONE, resolved off SK_Plane8's reference pose in UE's own")
    say("    frame. Wiring a bone about the wrong axis reads as a modelling fault, not a")
    say("    wiring one, so it is measured, not assumed. THREE BONES ARE RAKED ON PURPOSE.")
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
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of. THIS RIG IS THE DEEPEST")
    say("    IN THE FLEET: gear_wing_L > bogie_wing_L > wheel_L1..L2 and gear_body_L >")
    say("    bogie_body_L > wheel_L3..L5 are three deep on four legs, plus gear_nose >")
    say("    nosewheel_steer > nosewheel. Wheels first, then bogies, then legs.")


def verify(asset, skeleton):
    """Check what is on disk, whether this run created it or found it."""
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_Plane8_Skeleton" % ABP_NAME)

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
        # LOAD, never replace: DA_Aircraft_Plane8 points at this asset's generated class,
        # and Tools/wire_plane8_anim.py rebuilds the GRAPH inside it without replacing it.
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
