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

THE BONES, from the export's own 'plane2_rig' skin:

    root          the whole airframe hangs off it; nothing drives it
    prop_L        PropAngleDegrees   about the blade axis
    prop_R        PropAngleDegrees   about the blade axis
    wheel_L       WheelAngleDegrees  about the axle
    wheel_R       WheelAngleDegrees  about the axle
    nosewheel_L   WheelAngleDegrees  about the axle
    nosewheel_R   WheelAngleDegrees  about the axle

PLANE2 IS A TWIN, which is new: ABP_PiperMeridian drives ONE prop bone. Both propellers take
the same PropAngleDegrees here - they are not independently governed, and a real difference
between them would be a simulation feature, not an animation one.
"""
import unreal

SKELETON = "/Game/Aircraft/Plane2/SK_Plane2_Skeleton"
MESH = "/Game/Aircraft/Plane2/SK_Plane2"
ABP_PATH = "/Game/Aircraft/Plane2"
ABP_NAME = "ABP_Plane2"

# What the graph has to drive, printed so the editor work is against a list rather than a
# memory of one. UAirsideAgentAnim computes each of these; the graph only applies them.
BONE_PLAN = [
    ("prop_L", "PropAngleDegrees"),
    ("prop_R", "PropAngleDegrees"),
    ("wheel_L", "WheelAngleDegrees"),
    ("wheel_R", "WheelAngleDegrees"),
    ("nosewheel_L", "WheelAngleDegrees"),
    ("nosewheel_R", "WheelAngleDegrees"),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def report_plan():
    """The editor work this script cannot do, as a list rather than a memory of one."""
    say("")
    say("STILL TO DO BY HAND - UE 5.8 exposes no Python API for graph nodes:")
    say("  open %s and, in AnimGraph, add one Transform (Modify) Bone per row:" % ABP_NAME)
    for bone, variable in BONE_PLAN:
        say("    %-12s  Rotation from %s, about its spin axis, Bone Space, Replace"
            % (bone, variable))
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
