"""Creates ABP_Plane2, parented to UAirsideAgentAnim and targeting SK_Plane2's skeleton.
Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH, and the split is forced rather than chosen.
UE 5.8 exposes no Python API for creating a node in a Blueprint graph or connecting two pins:
BlueprintEditorLibrary can list graphs, find pins, add function graphs and add overrides, and
there it stops. That is the same wall UBuildBarWidget's header records for Widget Blueprints -
"UWidgetBlueprint::WidgetTree is not a scriptable property" - in a second place.

So the parts that are easy to get WRONG and tedious to redo are done here: the parent class
(an Anim Blueprint on the wrong parent compiles happily and exposes none of the values the
graph needs) and the target skeleton. The Transform (Modify) Bone nodes are then a few
minutes in the editor, against the bone names this script prints.

THE BONE PLAN IS READ FROM THE .glb, not listed here. It was listed here, naming a split
nosewheel_L and nosewheel_R, and the very next export replaced both with a single nosewheel -
so a hand-kept list would already have been wrong once, in a script whose whole job is to
tell someone which bones to wire. The export is the authority on its own rig.

Which VARIABLE drives which bone is still a decision, and stays here: anything named prop
turns at the propeller's rate, anything with wheel in it at the wheel's.

ROTATION ONLY, AND ADDED RATHER THAN REPLACED. Each Transform (Modify) Bone leaves
Translation and Scale on Ignore and sets Rotation to ADD TO EXISTING in Bone Space. Both
halves were got wrong once while wiring this:

  * Translation on Replace writes the node's default (0,0,0), putting the bone at its parent
    - root, at ground level - so the props and wheels fall off the aeroplane and lie
    underneath it. Their bind-pose positions ARE the spin axes; build_export.py placed them
    there deliberately and the graph has no business moving them.

  * Rotation on Replace discards the bone's bind-pose ORIENTATION, so every driven part
    reorients to whatever the rotator means in that space and they all end up pointing the
    same way. Add to Existing turns each part from where the rig left it.

Add is correct rather than merely safer: UAirsideAgentAnim accumulates PropAngleDegrees and
wraps it to 0..360, so it is an absolute angle from the bind pose, not a per-frame delta
that adding would integrate twice.

PLANE2 IS A TWIN, which is new: ABP_PiperMeridian drives ONE prop bone. Both propellers take
the same PropAngleDegrees here - they are not independently governed, and a real difference
between them would be a simulation feature, not an animation one.
"""
import json
import struct

import unreal

SOURCE = r"C:\repos\AirportMgr2Models\plane2\export\plane2.glb"

SKELETON = "/Game/Aircraft/Plane2/SK_Plane2_Skeleton"
MESH = "/Game/Aircraft/Plane2/SK_Plane2"
ABP_PATH = "/Game/Aircraft/Plane2"
ABP_NAME = "ABP_Plane2"

def bone_plan():
    """(bone, variable) per joint the export declares, root excluded.

    The NAMES come from the .glb; the MAPPING is the decision this script owns. A joint that
    matches neither rule is reported rather than skipped silently - an unrecognised bone is
    either a rig the sim does not know how to drive yet, or a typo, and both want saying.
    """
    plan = []
    for name in joint_names():
        lowered = name.lower()
        if lowered == "root":
            continue
        if "prop" in lowered:
            plan.append((name, "PropAngleDegrees"))
        elif "wheel" in lowered:
            plan.append((name, "WheelAngleDegrees"))
        else:
            plan.append((name, "?  UNRECOGNISED - nothing in UAirsideAgentAnim drives it"))
    return plan


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
        unreal.log_error("MARKER: FAIL could not read the skin from %s: %s" % (SOURCE, exc))
        return []

    nodes = doc.get("nodes", [])
    names = []
    for skin in doc.get("skins", []):
        for index in skin.get("joints", []):
            if 0 <= index < len(nodes):
                names.append(nodes[index].get("name", "?"))
    return names


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


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
    say("  TWO MODES BITE, and both were got wrong once here:")
    say("   * Translation on Replace writes the default (0,0,0), snapping each bone to its")
    say("     parent - root, on the ground - so the props and wheels detach and lie under")
    say("     the aeroplane. Their bind-pose positions ARE the spin axes.")
    say("   * Rotation on Replace discards the bone's bind-pose ORIENTATION and sets it to")
    say("     the rotator given, so every part reorients the same way and they all end up")
    say("     pointing up. Add to Existing turns each part from where the rig put it.")
    say("  Add is right because PropAngleDegrees is an ACCUMULATED absolute angle, wrapped")
    say("  to 0..360 - bind plus angle each frame, not a per-frame delta to integrate.")
    say("  chain them into Output Pose, then Compile and Save.")


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
        say("PASS %s targets SK_Plane2_Skeleton" % ABP_NAME)

    # THROUGH BlueprintEditorLibrary, because UAnimBlueprint does not expose parent_class as
    # a readable editor property - get_editor_property raises on it, which is what stopped
    # the first run of this script after its first assertion.
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
        fail("no skeleton at %s - run import_plane2.py first" % SKELETON)
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
    # THE PARENT IS THE POINT. UAirsideAgentAnim is what computes PropAngleDegrees,
    # WheelAngleDegrees, bAirborne, GroundSpeed and bPropIsDisc from the agent's state; an
    # Anim Blueprint left on plain UAnimInstance compiles, runs, and exposes none of them.
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
