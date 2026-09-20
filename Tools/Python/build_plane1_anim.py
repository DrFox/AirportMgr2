"""Creates ABP_Plane1, parented to UAirsideAgentAnim and targeting SK_Plane1's skeleton.
Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log. NOTE those lines
land in Saved/Logs/AirportMgr.log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

THIS CREATES THE ASSET AND NOT ITS ANIMGRAPH, the same wall build_plane2_anim.py and
build_fueltruck_anim.py both hit: the `unreal` Python module exposes no API for creating a
node in a Blueprint graph or connecting two pins. BlueprintEditorLibrary can list graphs,
find pins, add function graphs and add overrides, and there it stops. So the parts that are
easy to get WRONG and tedious to redo are done here - the parent class, on which an Anim
Blueprint compiles happily while exposing none of the values the graph needs, and the target
skeleton - and the Transform (Modify) Bone nodes are a few minutes in the editor against the
bone names this script prints.

CORRECTED 2026-09-20: "no Python API" was true of this script's `unreal` module and FALSE of
the editor as a whole. 5.8's UBlueprintGraphEditor drives AnimGraph nodes, and Epic's MCP
EditorToolset exposes it - create_node, connect_pins, ObjectTools.set_properties for
BoneToModify and the four modes, compile_blueprint, all verified against an AnimGraph on
2026-09-20. A whole graph wires in one ProgrammaticToolset call. That path needs the editor
RUNNING and this one needs it closed, which is a RESTART between them and not a wall - the
whole pipeline ran unattended on 2026-09-20 and its graph diffed IDENTICAL to this asset's
hand-wired one. State machines remain hand work; see docs/2026-09-20-animgraph-authoring.md
for what is and is not reachable, and for the wiring script.

THIS IS THE THIRD COPY OF THAT MECHANISM AND THE SECOND COPY OF THE BONE PLAN, which is
worth saying plainly rather than discovering. build_plane2_anim.py has it, and
build_fueltruck_anim.py copied the asset half but not the plan. The MAPPING below - which
substring of a bone name picks which UAirsideAgentAnim property - is a list that must agree
with build_plane2_anim.py's and with the C++, and CLAUDE.md's rule is that such lists are
ONE list. They are two here because the extraction is a refactor of a working tool that
nobody asked for in the same breath as importing an aeroplane, and airside_import.py's
header records the same judgement about the three import_*.py scripts: fold them in when the
divergence bites. IT WILL BITE ON THE NEXT PROPERTY ADDED TO UAirsideAgentAnim - the gear and
door rules landed in build_plane2_anim.py on 2026-09-19 and would have to be typed here too -
so the fourth model is the one that extracts rather than copies.

ABP_Plane3 AND ABP_Plane4 HAVE NO SCRIPT AT ALL; both were duplicated from ABP_Plane2 in the
editor and retargeted by hand. That works and leaves nothing behind that says the parent and
the skeleton were ever checked, which is what verify() below exists to fix for this one.

FIVE DRIVEN BONES, THE FEWEST OF ANY AEROPLANE HERE, and the reason is the aeroplane: a 172
has one propeller where every other type in the fleet is a twin, and FIXED GEAR, so there is
nothing to retract and no bay door to hinge. plane4 drives twelve. The rig has no gear or
door bone for a retraction to reach even if a cycle were authored, which is the second half
of why DA_Aircraft_Plane1 declares none - see build_plane1_type.py.

ROTATION ONLY, AND ADDED RATHER THAN REPLACED, which build_plane2_anim.py got wrong once in
each direction and records at length. Both halves matter:

  * Translation on Replace writes the node's default (0,0,0), putting the bone at its parent
    - root, at ground level - so the propeller and the wheels fall off the aeroplane and lie
    underneath it. Their bind-pose positions ARE the spin axes; build_export.py placed them
    there deliberately and the graph has no business moving them.

  * Rotation on Replace discards the bone's bind-pose ORIENTATION, so every driven part
    reorients to whatever the rotator means in that space and they all end up pointing the
    same way. Add to Existing turns each part from where the rig left it.

Add is correct rather than merely safer: UAirsideAgentAnim accumulates PropAngleDegrees and
wraps it to 0..360, so it is an absolute angle from the bind pose, not a per-frame delta that
adding would integrate twice.
"""
import json
import struct

import unreal

SOURCE = r"C:\repos\AirportMgr2Models\plane1\export\plane1.glb"

SKELETON = "/Game/Aircraft/Plane1/SK_Plane1_Skeleton"
MESH = "/Game/Aircraft/Plane1/SK_Plane1"
ABP_PATH = "/Game/Aircraft/Plane1"
ABP_NAME = "ABP_Plane1"


def bone_plan():
    """(bone, variable) per joint the export declares, root excluded.

    The NAMES come from the .glb; the MAPPING is the decision this script owns. A joint that
    matches neither rule is reported rather than skipped silently - an unrecognised bone is
    either a rig the sim does not know how to drive yet, or a typo, and both want saying.

    THIS LIST MUST AGREE WITH UAirsideAgentAnim'S PROPERTY NAMES and there is no compiler to
    check it - see CLAUDE.md, "check where a list is CONSUMED". It must also agree with
    build_plane2_anim.py's copy of the same rules; the header says why there are two and when
    to make it one.
    """
    plan = []
    for name in joint_names():
        lowered = name.lower()
        if lowered == "root":
            continue
        if "prop" in lowered:
            plan.append((name, "PropAngleDegrees"))
        elif "steer" in lowered:
            # BEFORE the wheel rule, because 'nosewheel_steer' matches both and the wheel
            # rule would tell someone to wire a STEERING bone to the ROLL angle - which spins
            # the nose gear about the strut and looks like a broken castor. This rig has that
            # exact collision, so the ordering is load-bearing here rather than defensive.
            plan.append((name, "SteerAngleDegrees"))
        elif "wheel" in lowered:
            plan.append((name, "WheelAngleDegrees"))
        elif "gear" in lowered:
            # NO BONE IN THIS RIG REACHES THESE TWO, and they are kept anyway so the plan
            # does not quietly become a different plan from plane2's. A 172's gear is welded
            # on; if one of these ever fires here, the export has grown a retraction and the
            # type needs a gear cycle to match.
            plan.append((name, "GearAngleDegrees"))
        elif "door" in lowered:
            plan.append((name, "BayDoorAngleDegrees"))
        else:
            plan.append((name, "?  UNRECOGNISED - nothing in UAirsideAgentAnim drives it"))
    return plan


def joint_names():
    """The joint names the .glb's skin declares, read from the file itself.

    A glb is a 12-byte header then a length-prefixed JSON chunk; no dependency needed, and
    the editor's Python has no glTF reader anyway.

    READ RATHER THAN LISTED, which build_plane2_anim.py learned the hard way: it carried a
    hand-written list naming a split nosewheel_L and nosewheel_R, and the very next export
    replaced both with a single nosewheel. THIS rig proves the point twice over - it was
    re-exported three times on 2026-09-19 and its main gear went from one `wheel` bone to
    wheel_L and wheel_R in the last of them. A list typed here on the first attempt would
    have been wrong by the third.
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
    say("STILL TO DO - this commandlet's `unreal` module cannot make graph nodes.")
    say("  MCP CAN, against the running editor: docs/2026-09-20-animgraph-authoring.md.")
    say("  By hand, it is:")
    say("  open %s and, in AnimGraph, add one Transform (Modify) Bone per row:" % ABP_NAME)
    for bone, variable in bone_plan():
        say("    %-16s  Rotation driven by %s" % (bone, variable))
    say("")
    say("  TWO BONES, NEVER BOTH ONTO ONE: the steer bone is the nose wheel's PARENT.")
    say("  ORDER IS DISPUTED AND THE ASSETS WIN. This line read 'wire nosewheel_steer")
    say("  BEFORE nosewheel' until 2026-09-20, when a graph authored to it diffed against")
    say("  ABP_Plane1 and ABP_Plane2 and found BOTH ship the reverse - steer LAST. The")
    say("  engine backs the assets: FCSPose::SafeSetCSBoneTransforms refreshes children")
    say("  already in component space, so steering the parent last carries the rolled")
    say("  wheel with it. Wire steer AFTER nosewheel unless someone rules otherwise.")
    say("")
    say("  ON EVERY ONE OF THOSE NODES:")
    say("    Translation Mode = IGNORE            <- leave it alone")
    say("    Rotation Mode    = ADD TO EXISTING   <- not Replace")
    say("    Scale Mode       = IGNORE")
    say("    Rotation Space   = Bone Space")
    say("")
    say("  TWO MODES BITE, and both were got wrong once on ABP_Plane2:")
    say("   * Translation on Replace writes the default (0,0,0), snapping each bone to its")
    say("     parent - root, on the ground - so the propeller and wheels detach and lie")
    say("     under the aeroplane. Their bind-pose positions ARE the spin axes.")
    say("   * Rotation on Replace discards the bone's bind-pose ORIENTATION and sets it to")
    say("     the rotator given, so every part reorients the same way and they all end up")
    say("     pointing up. Add to Existing turns each part from where the rig put it.")
    say("  Add is right because PropAngleDegrees is an ACCUMULATED absolute angle, wrapped")
    say("  to 0..360 - bind plus angle each frame, not a per-frame delta to integrate.")
    say("  Chain them into Output Pose, then Compile and Save.")
    say("")
    say("  THEN RUN build_plane1_type.py, which needs this asset's generated class to")
    say("  exist before it can point DA_Aircraft_Plane1 at it or set its wheel radius.")


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
        say("PASS %s targets SK_Plane1_Skeleton" % ABP_NAME)

    # THROUGH BlueprintEditorLibrary, because UAnimBlueprint does not expose parent_class as
    # a readable editor property - get_editor_property raises on it, which is what stopped
    # the first run of build_plane2_anim.py after its first assertion.
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
        fail("no skeleton at %s - run import_models.py first" % SKELETON)
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
    # WheelAngleDegrees, SteerAngleDegrees, bAirborne, GroundSpeed and bPropIsDisc from the
    # agent's state; an Anim Blueprint left on plain UAnimInstance compiles, runs, and
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
