"""Creates ABP_FuelTruck1, parented to UAirsideAgentAnim and targeting SK_FuelTruck1's
skeleton. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log. NOTE those lines
land in Saved/Logs/AirportMgr.log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH, the same wall plane2's own script hit (since
retired - see Issue #294) and for the same reason: the `unreal` Python module exposes no API
for creating a node in a Blueprint
graph or connecting two pins. So the parts that are easy to get WRONG and tedious to redo are
done here - the parent class and the target skeleton - and the Transform (Modify) Bone nodes
are a few minutes in the editor against the bone names this script prints.

CORRECTED 2026-09-20: the three copies of that sentence all said "UE 5.8 exposes no Python
API", which is a claim about the engine and is wrong. 5.8's UBlueprintGraphEditor authors
AnimGraph nodes and Epic's MCP EditorToolset exposes it; verified end to end on 2026-09-20.
It needs the editor RUNNING, which this commandlet needs closed - one restart between the
halves, run unattended end to end on 2026-09-20 against a graph that came out IDENTICAL to a
hand-wired one. docs/2026-09-20-animgraph-authoring.md has the reachable set.

THE BONE PLAN IS READ FROM THE .glb, not listed here, for the reason plane2's own script
recorded: a hand-kept list of bones had already been wrong once, in a script whose whole job
is to tell someone which bones to wire. The export is the authority on its own rig.

SIX DRIVEN BONES, WHICH IS MORE THAN ANY AIRCRAFT HERE. Four wheels take WheelAngleDegrees
and the two front steer bones take SteerAngleDegrees. The steer bones are the PARENTS of the
front roll bones, so the roll axis turns with the steering - wire the steer node before the
wheel node it carries, and never the two onto one bone.

THE BEACON IS DRIVEN SINCE 2026-09-25 - UAirsideAgentAnim::BeaconAngleDegrees, wired by
Tools/wire_vehicle_anim.py from Tools/Python/build_vehicle_anims.py's plan, which also supersedes
this script for the bowser's defaults. What follows was true of the Tripo truck: THE BEACON WAS
NOT DRIVEN. It is rigged and it would want a steady spin, but nothing in
UAirsideAgentAnim produces one: every angle there is derived from the agent's motion, and a
beacon turns whether or not the truck is moving. Reported as unrecognised rather than quietly
skipped, so the gap is a decision someone can see.
"""
import os
import sys

import unreal
# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript. The commandlet
# executes the file without adding its folder the way `python foo.py` would, so the import
# below raises ModuleNotFoundError and the run dies before a single MARKER: line - which
# reads as "the commandlet did nothing" rather than as a missing path. Put it on first.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import bone_plan, fail, joint_names, say  # noqa: E402

SOURCE = r"C:\repos\AirportMgr2Models\rigidCab1\export\fueltruck1.glb"  # built in rigidCab1.blend since 2026-09-24

SKELETON = "/Game/Vehicles/FuelTruck1/SK_FuelTruck1_Skeleton"
MESH = "/Game/Vehicles/FuelTruck1/SK_FuelTruck1"
ABP_PATH = "/Game/Vehicles/FuelTruck1"
ABP_NAME = "ABP_FuelTruck1"



def report_plan():
    """The editor work this script cannot do, as a list rather than a memory of one."""
    say("")
    say("STILL TO DO - this commandlet's `unreal` module cannot make graph nodes.")
    say("  MCP CAN, against the running editor: docs/2026-09-20-animgraph-authoring.md.")
    say("  By hand, it is:")
    say("  open %s and, in AnimGraph, add one Transform (Modify) Bone per row:" % ABP_NAME)
    for bone, variable in bone_plan(joint_names(SOURCE)):
        say("    %-12s  Rotation driven by %s" % (bone, variable))
    say("")
    say("  ON EVERY ONE OF THOSE NODES:")
    say("    Translation Mode = IGNORE            <- leave it alone")
    say("    Rotation Mode    = ADD TO EXISTING   <- not Replace")
    say("    Scale Mode       = IGNORE")
    say("    Rotation Space   = Bone Space")
    say("")
    say("  TWO MODES BITE, both got wrong once while wiring ABP_Plane2:")
    say("   * Translation on Replace writes the default (0,0,0), snapping each bone to its")
    say("     parent - so the wheels detach and lie under the truck. Their bind-pose")
    say("     positions ARE the spin axes; build_export.py put them there deliberately.")
    say("   * Rotation on Replace discards the bone's bind-pose ORIENTATION, so every")
    say("     driven part reorients the same way. Add to Existing turns each wheel from")
    say("     where the rig left it - which matters here, as the wheels carry 1.23 deg of")
    say("     camber that Replace would flatten.")
    say("")
    say("  ORDER MATTERS ON THE FRONT AXLE: steer_FL carries wheel_FL. Both on one bone")
    say("  cannot work - a single Transform (Modify) Bone applies its rotations in a fixed")
    say("  order and the wheel would wobble instead of steering.")
    say("  WHICH WAY ROUND IS DISPUTED. This line said steer BEFORE wheel; ABP_Plane1 and")
    say("  ABP_Plane2 both ship the reverse, and FCSPose::SafeSetCSBoneTransforms backs")
    say("  them - it refreshes children already in component space, so the parent steering")
    say("  last carries the rolled wheel. Follow the aeroplanes: steer AFTER the wheel.")
    say("")
    say("  chain them into Output Pose, then Compile and Save.")


def verify(asset, skeleton):
    """Check what is on disk, whether this run created it or found it.

    Run every time rather than only after creation: the asset outlives the script, and a
    reparent or retarget done by hand is exactly the kind of change that should be caught
    here rather than by wheels that do not turn.
    """
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets SK_FuelTruck1_Skeleton" % ABP_NAME)

    # THROUGH BlueprintEditorLibrary, because UAnimBlueprint does not expose parent_class as
    # a readable editor property - get_editor_property raises on it.
    parent = unreal.BlueprintEditorLibrary.get_blueprint_parent_class(asset)
    say("parent class: %s" % parent)
    if parent is None or "AirsideAgentAnim" not in str(parent):
        fail("parent is not UAirsideAgentAnim - the graph would see none of the angles")
    else:
        say("PASS parent is UAirsideAgentAnim, so the graph sees the angles it applies")

    # The AnimGraph is what this script cannot author, so say plainly whether one has been
    # authored yet rather than leaving "is it wired?" to be answered by running the game.
    graphs = [str(g) for g in unreal.BlueprintEditorLibrary.list_graph_names(asset)]
    say("graphs: %s" % ", ".join(graphs))


def run():
    skeleton = unreal.EditorAssetLibrary.load_asset(SKELETON)
    if skeleton is None:
        fail("no skeleton at %s - run import_fueltruck.py first" % SKELETON)
        say("DONE")
        return

    path = "%s/%s" % (ABP_PATH, ABP_NAME)
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing is not None:
        # LOAD, never replace. Once the AnimGraph is authored by hand, deleting and
        # recreating this asset throws that work away - and it cannot be scripted back.
        say("%s already exists; leaving it alone so any authored graph survives" % path)
        verify(existing, skeleton)
        report_plan()
        say("DONE")
        return

    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    # THE PARENT IS THE POINT. UAirsideAgentAnim computes WheelAngleDegrees and
    # SteerAngleDegrees from the agent's state; an Anim Blueprint left on plain
    # UAnimInstance compiles, runs, and exposes none of them.
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
