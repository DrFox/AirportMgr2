"""Creates ABP_FuelTruck1, parented to UAirsideAgentAnim and targeting SK_FuelTruck1's
skeleton. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log. NOTE those lines
land in Saved/Logs/AirportMgr.log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH, the same wall build_plane2_anim.py hit and for
the same reason: UE 5.8 exposes no Python API for creating a node in a Blueprint graph or
connecting two pins. So the parts that are easy to get WRONG and tedious to redo are done
here - the parent class and the target skeleton - and the Transform (Modify) Bone nodes are
a few minutes in the editor against the bone names this script prints.

THE BONE PLAN IS READ FROM THE .glb, not listed here, for the reason build_plane2_anim.py
records: a hand-kept list of bones had already been wrong once, in a script whose whole job
is to tell someone which bones to wire. The export is the authority on its own rig.

SIX DRIVEN BONES, WHICH IS MORE THAN ANY AIRCRAFT HERE. Four wheels take WheelAngleDegrees
and the two front steer bones take SteerAngleDegrees. The steer bones are the PARENTS of the
front roll bones, so the roll axis turns with the steering - wire the steer node before the
wheel node it carries, and never the two onto one bone.

THE BEACON IS NOT DRIVEN. It is rigged and it would want a steady spin, but nothing in
UAirsideAgentAnim produces one: every angle there is derived from the agent's motion, and a
beacon turns whether or not the truck is moving. Reported as unrecognised rather than quietly
skipped, so the gap is a decision someone can see.
"""
import json
import struct

import unreal

SOURCE = r"C:\repos\AirportMgr2Models\fueltruck1\export\fueltruck1.glb"

SKELETON = "/Game/Vehicles/FuelTruck1/SK_FuelTruck1_Skeleton"
MESH = "/Game/Vehicles/FuelTruck1/SK_FuelTruck1"
ABP_PATH = "/Game/Vehicles/FuelTruck1"
ABP_NAME = "ABP_FuelTruck1"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def joint_names():
    """The joint names the .glb's skin declares, read from the file itself.

    A glb is a 12-byte header then a length-prefixed JSON chunk; no dependency needed, and
    the editor's Python has no glTF reader anyway.
    """
    try:
        with open(SOURCE, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            doc = json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        fail("could not read the skin from %s: %s" % (SOURCE, exc))
        return []

    nodes = doc.get("nodes", [])
    names = []
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            if 0 <= index < len(nodes):
                names.append(nodes[index].get("name", "?"))
    return names


def bone_plan():
    """(bone, variable) per joint the export declares, root excluded.

    The NAMES come from the .glb; the MAPPING is the decision this script owns. A joint that
    matches no rule is reported rather than skipped silently - an unrecognised bone is either
    a rig the sim cannot drive yet or a typo, and both want saying.
    """
    plan = []
    for name in joint_names():
        lowered = name.lower()
        if lowered == "root":
            continue
        if "steer" in lowered:
            # BEFORE the wheel rule. 'steer_FL' does not contain 'wheel' so the order does
            # not bite on this rig the way it does on plane2's 'nosewheel_steer' - but the
            # order is kept anyway, because the next vehicle's bone may well be named that
            # way and the failure is silent: a STEERING bone wired to the ROLL angle spins
            # the wheel about its strut and reads as a broken castor.
            plan.append((name, "SteerAngleDegrees"))
        elif "wheel" in lowered:
            plan.append((name, "WheelAngleDegrees"))
        else:
            plan.append((name, "?  UNRECOGNISED - nothing in UAirsideAgentAnim drives it"))
    return plan


def report_plan():
    """The editor work this script cannot do, as a list rather than a memory of one."""
    say("")
    say("STILL TO DO BY HAND - UE 5.8 exposes no Python API for graph nodes:")
    say("  open %s and, in AnimGraph, add one Transform (Modify) Bone per row:" % ABP_NAME)
    for bone, variable in bone_plan():
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
    say("  ORDER MATTERS ON THE FRONT AXLE: steer_FL carries wheel_FL, so put the steer")
    say("  node BEFORE the wheel node in the chain. Both on one bone cannot work - a single")
    say("  Transform (Modify) Bone applies its rotations in a fixed order and the wheel")
    say("  would wobble instead of steering.")
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
