"""Creates ABP_TruckCab1 and ABP_TankTrailer1, each parented to UAirsideAgentAnim and
targeting its own mesh's skeleton. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log - those lines land
in Saved/Logs/AirportMgr.log, not stdout.

THIS CREATES THE ASSETS AND NOT THEIR ANIMGRAPHS, the same wall build_fueltruck_anim.py hit.
5.8's UBlueprintGraphEditor CAN author AnimGraph nodes (verified 2026-09-20, see
docs/2026-09-20-animgraph-authoring.md), but only against a RUNNING editor via MCP, and this
run found no editor up (Tools/Mcp.py: connection refused). So the parts that are easy to get
wrong and tedious to redo are done here - the parent class and the target skeleton - and the
Transform (Modify) Bone nodes are a manual step (by hand, or by MCP once the editor is open)
against the bone plan this script prints.

THE BONE PLAN IS READ FROM EACH .glb, not listed here - airside_anim.joint_names and
bone_plan are the shared mechanism four other build_*_anim.py scripts already use.

FIFTH_WHEEL AND KINGPIN ARE EXCLUDED FROM THE PLAN, BY HAND, RATHER THAN LEFT TO bone_plan.
airside_anim.BONE_RULES matches by SUBSTRING, and 'wheel' is a substring of 'fifth_wheel' -
so an unfiltered bone_plan would tell whoever wires this graph to drive the tractor's kingpin
SOCKET with WheelAngleDegrees, spinning the coupling plate. No existing rig has a bone whose
name contains a driven word as a false substring, so this is the first time the rule needed a
carve-out; the exclusion happens HERE, in this model's own table, rather than teaching
BONE_RULES about one rig's naming. 'kingpin' matches no rule and would report UNRECOGNISED on
its own merits, but is excluded too, for the same reason: both are attachment points the
presenter positions from FTowLink geometry (spec 2026-09-24 revision), not bones any
AnimGraph here drives - UAirsideAgentAnim carries no coupling-angle property at all.

SIX AND FOUR DRIVEN BONES. The cab: four wheels take WheelAngleDegrees, the two front steer
bones take SteerAngleDegrees, and steer_FL/FR are the PARENTS of wheel_FL/FR (roll turns with
the steering) - wire the steer node before the wheel node it carries, matching
build_fueltruck_anim.py's own note on the aircraft convention (steer AFTER wheel, per
FCSPose::SafeSetCSBoneTransforms). The trailer: four wheels, no steer, no chain.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import bone_plan, fail, joint_names, say  # noqa: E402

MODELS_ROOT = r"C:\repos\AirportMgr2Models\truckCab1\export"

# key -> (glb, skeleton path, mesh path, abp folder, abp name, socket bones excluded from the
# driven-bone plan - see the header above for why fifth_wheel and kingpin are not on it).
RIGS = [
    dict(
        key="truckCab1",
        source=os.path.join(MODELS_ROOT, "truckCab1.glb"),
        skeleton="/Game/Vehicles/Rig/TruckCab1/SK_TruckCab1_Skeleton",
        mesh="/Game/Vehicles/Rig/TruckCab1/SK_TruckCab1",
        abp_path="/Game/Vehicles/Rig/TruckCab1",
        abp_name="ABP_TruckCab1",
        sockets={"fifth_wheel"},
    ),
    dict(
        key="tankTrailer1",
        source=os.path.join(MODELS_ROOT, "tankTrailer1.glb"),
        skeleton="/Game/Vehicles/Rig/TankTrailer1/SK_TankTrailer1_Skeleton",
        mesh="/Game/Vehicles/Rig/TankTrailer1/SK_TankTrailer1",
        abp_path="/Game/Vehicles/Rig/TankTrailer1",
        abp_name="ABP_TankTrailer1",
        sockets={"kingpin"},
    ),
]


def report_plan(rig):
    """The editor work this script cannot do, as a list rather than a memory of one."""
    names = joint_names(rig["source"])
    driven = [n for n in names if n.lower() != "root" and n not in rig["sockets"]]

    say("")
    say("STILL TO DO for %s - this commandlet's `unreal` module cannot make graph nodes, and "
        "no editor was up for MCP this run." % rig["abp_name"])
    say("  MCP CAN, against a RUNNING editor: docs/2026-09-20-animgraph-authoring.md.")
    say("  By hand, or by MCP once the editor is open, it is:")
    say("  open %s and, in AnimGraph, add one Transform (Modify) Bone per row:" % rig["abp_name"])
    for bone, variable in bone_plan(driven):
        say("    %-12s  Rotation driven by %s" % (bone, variable))
    for socket in sorted(s for s in rig["sockets"] if s in names):
        say("    %-12s  SOCKET, NOT DRIVEN - a bone_plan substring match ('wheel' inside "
            "'%s') would wire this to WheelAngleDegrees if not excluded by hand. It is a "
            "coupling point the presenter positions from FTowLink geometry, not a bone any "
            "AnimGraph here drives." % (socket, socket))
    say("")
    say("  ON EVERY DRIVEN NODE:")
    say("    Translation Mode = IGNORE            <- leave it alone")
    say("    Rotation Mode    = ADD TO EXISTING   <- not Replace (discards the bind pose)")
    say("    Scale Mode       = IGNORE")
    say("    Rotation Space   = Bone Space")
    say("")
    if any(v == "SteerAngleDegrees" for _, v in bone_plan(driven)):
        say("  ORDER MATTERS ON THE FRONT AXLE: steer_FL/FR carry wheel_FL/FR. Follow the")
        say("  aeroplanes' convention (build_fueltruck_anim.py): wire STEER AFTER WHEEL, so")
        say("  FCSPose::SafeSetCSBoneTransforms refreshes the already-rolled child last and")
        say("  the parent's steering carries it, rather than the wheel wobbling.")
        say("")
    say("  chain them into Output Pose, then Compile and Save.")


def verify(asset, skeleton_asset, rig):
    """Check what is on disk, whether this run created it or found it."""
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton_asset:
        fail("%s: target_skeleton is %s, expected %s" % (rig["abp_name"], got, skeleton_asset))
    else:
        say("PASS %s targets %s" % (rig["abp_name"], skeleton_asset.get_name()))

    parent = unreal.BlueprintEditorLibrary.get_blueprint_parent_class(asset)
    say("%s parent class: %s" % (rig["abp_name"], parent))
    if parent is None or "AirsideAgentAnim" not in str(parent):
        fail("%s: parent is not UAirsideAgentAnim - the graph would see none of the angles"
             % rig["abp_name"])
    else:
        say("PASS %s parent is UAirsideAgentAnim, so the graph sees the angles it applies"
            % rig["abp_name"])

    graphs = [str(g) for g in unreal.BlueprintEditorLibrary.list_graph_names(asset)]
    say("%s graphs: %s" % (rig["abp_name"], ", ".join(graphs)))


def run_one(rig):
    say("=" * 78)
    skeleton = unreal.EditorAssetLibrary.load_asset(rig["skeleton"])
    if skeleton is None:
        fail("no skeleton at %s - run import_rig.py first" % rig["skeleton"])
        return

    path = "%s/%s" % (rig["abp_path"], rig["abp_name"])
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing is not None:
        # LOAD, never replace - once the AnimGraph is authored, deleting and recreating this
        # asset throws that work away and it cannot be scripted back.
        say("%s already exists; leaving it alone so any authored graph survives" % path)
        verify(existing, skeleton, rig)
        report_plan(rig)
        return

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    # THE PARENT IS THE POINT. UAirsideAgentAnim computes WheelAngleDegrees and
    # SteerAngleDegrees from the agent's state; plain UAnimInstance exposes neither.
    factory.set_editor_property("parent_class", unreal.AirsideAgentAnim)

    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        rig["abp_name"], rig["abp_path"], unreal.AnimBlueprint, factory)
    if asset is None:
        fail("could not create %s" % path)
        return

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # READ BACK FROM DISK - an asset that reports success and saves nothing is this project's
    # most familiar failure.
    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    if reloaded is None:
        fail("%s did not survive the save" % path)
        return

    say("created %s" % path)
    verify(reloaded, skeleton, rig)
    report_plan(rig)


def run():
    for rig in RIGS:
        run_one(rig)
    say("DONE")


run()
