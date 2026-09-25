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

EXTENDED 2026-09-24 (task 3b) FOR THE DRAWBAR CHAIN: utility1 (the tow) and fuelTrailer1 (the
towed bowser) join RIGS below. Their MECHANISM is identical to the rig's - create-if-absent,
target skeleton, parent class, print the plan - which is why they are entries in the SAME
table rather than a sibling script; only the model facts differ, and RIGS is where every
model's facts already live.

fuelTrailer1's steer_FL, steer_FR AND towbar_yaw ARE A SECOND CARVE-OUT, of a different shape
from fifth_wheel/kingpin's. Those two are SOCKETS - no AnimGraph channel could ever apply to
them. steer_FL/FR and towbar_yaw are the OPPOSITE: bone_plan's ordinary substring rule matches
them correctly (SteerAngleDegrees), and the match is even semantically sensible - the whole
front axle turntable, steer bones included, genuinely turns together by ONE angle. What is
missing is not a rule but a DATA CHANNEL: UAirsideAgentAnim.SteerAngleDegrees is copied from
FAgentMotion.SteerAngleDegrees, which FRoadAgent::DescribeMotion documents as "the CONTROL
INPUT... the angle the follower steered with" - utility1's OWN front-wheel deflection, not any
fact about a trailer riding behind it. Wiring fuelTrailer1's turntable to it would make the
BOWSER's own steering angle follow whatever utility1's front wheels happen to be doing at the
same instant, which is a different, wrong physical quantity - not "no animation", a WRONG one,
which this project's own conventions treat as worse (comments-that-admit-a-defect memory: a
plausible-looking wrong answer is the one nothing catches). The real source - the towbar
link's own angle relative to the body it follows - is not modelled anywhere yet (spec 2026-09-24
revision, section 4: "the towbar is animated on the trailer's own towbar_yaw bone from the
towbar link's angle" is future work, not this task's). So RIGS marks these three bones
'unwired' rather than 'sockets': report_plan STILL prints bone_plan's honest suggestion for
them (SteerAngleDegrees IS the rule match), but flags each one, by name, as NOT to be wired
this round - so a reader who only skims the per-bone list is not quietly handed the wrong wire.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import bone_plan, fail, joint_names, say  # noqa: E402

MODELS_ROOT = r"C:\repos\AirportMgr2Models\truckCab1\export"
UTILITY_ROOT = r"C:\repos\AirportMgr2Models\utility1\export"

# key -> (glb, skeleton path, mesh path, abp folder, abp name, socket bones excluded from the
# driven-bone plan - see the header above for why fifth_wheel and kingpin are not on it -
# 'unwired': bones bone_plan matches correctly but that are NOT to be wired this round because
# no data channel is right for them yet - see the header's second carve-out).
RIGS = [
    dict(
        key="truckCab1",
        source=os.path.join(MODELS_ROOT, "truckCab1.glb"),
        skeleton="/Game/Vehicles/Rig/TruckCab1/SK_TruckCab1_Skeleton",
        mesh="/Game/Vehicles/Rig/TruckCab1/SK_TruckCab1",
        abp_path="/Game/Vehicles/Rig/TruckCab1",
        abp_name="ABP_TruckCab1",
        sockets={"fifth_wheel"},
        unwired=set(),
    ),
    dict(
        key="tankTrailer1",
        source=os.path.join(MODELS_ROOT, "tankTrailer1.glb"),
        skeleton="/Game/Vehicles/Rig/TankTrailer1/SK_TankTrailer1_Skeleton",
        mesh="/Game/Vehicles/Rig/TankTrailer1/SK_TankTrailer1",
        abp_path="/Game/Vehicles/Rig/TankTrailer1",
        abp_name="ABP_TankTrailer1",
        sockets={"kingpin"},
        unwired=set(),
    ),
    dict(
        key="utility1",
        source=os.path.join(UTILITY_ROOT, "utility1.glb"),
        skeleton="/Game/Vehicles/Utility1/SK_Utility1_Skeleton",
        mesh="/Game/Vehicles/Utility1/SK_Utility1",
        abp_path="/Game/Vehicles/Utility1",
        abp_name="ABP_Utility1",
        # 'hitch' is the coupling SOCKET fuelTrailer1's tow_eye snaps onto - not driven.
        # 'beacon' matches no BONE_RULES needle (nothing here drives a lamp mast) and is left
        # to report as UNRECOGNISED rather than hidden in this set, which is for sockets a
        # name-based rule would otherwise mis-match, not for "nothing drives this yet".
        sockets={"hitch"},
        unwired=set(),
    ),
    dict(
        key="fuelTrailer1",
        source=os.path.join(UTILITY_ROOT, "fuelTrailer1.glb"),
        skeleton="/Game/Vehicles/FuelTrailer1/SK_FuelTrailer1_Skeleton",
        mesh="/Game/Vehicles/FuelTrailer1/SK_FuelTrailer1",
        abp_path="/Game/Vehicles/FuelTrailer1",
        abp_name="ABP_FuelTrailer1",
        # tow_eye (onto utility1's hitch) and hitch (fuelTrailer1's OWN rear coupling, for a
        # further trailer in the train - unused, this trailer sits at the train's end) are
        # both SOCKETS.
        sockets={"tow_eye", "hitch"},
        # See the module header's second carve-out: SteerAngleDegrees is utility1's OWN wheel
        # deflection, not the towbar's angle - wiring these now would be a wrong answer, not a
        # missing one.
        # THE CHANNEL EXISTS SINCE 2026-09-24 (task 4): UAirsideAgentAnim.TowbarAngleDegrees,
        # and Tools/wire_fuelTrailer1_anim.py wires all three to it. Kept in this set so this
        # plan never suggests bone_plan's SteerAngleDegrees for them.
        unwired={"steer_FL", "steer_FR", "towbar_yaw"},
    ),
]


def report_plan(rig):
    """The editor work this script cannot do, as a list rather than a memory of one."""
    names = joint_names(rig["source"])
    unwired = rig.get("unwired", set())
    driven = [n for n in names if n.lower() != "root" and n not in rig["sockets"] and n not in unwired]

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
    for bone in sorted(b for b in unwired if b in names):
        matched = dict(bone_plan([bone])).get(bone, "?")
        if matched.startswith("?"):
            say("    %-12s  NOT WIRED THIS ROUND - bone_plan finds no rule for it either, and "
                "the channel it takes is TowbarAngleDegrees (see this module's header, 'A "
                "SECOND CARVE-OUT'); Tools/wire_fuelTrailer1_anim.py wires it." % bone)
        else:
            say("    %-12s  NOT WIRED THIS ROUND, though bone_plan's ordinary rule matches it "
                "(%s) - see this module's own header, 'A SECOND CARVE-OUT'. %s is a WRONG "
                "answer for this bone, not a missing one; wire it to TowbarAngleDegrees "
                "instead - Tools/wire_fuelTrailer1_anim.py does." % (bone, matched, matched))
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
