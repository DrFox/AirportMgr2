"""Points DA_AirsideContent at the fuel truck, so dispatched ground vehicles wear it instead
of the placeholder box. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Result lines are prefixed MARKER: and land in Saved/Logs/AirportMgr.log - the commandlet's
stdout carries only LogInit and errors, so a silent run has usually worked.

SET BY SCRIPT, which does not contradict UAirsideContent's reason for existing - the same
argument build_road_profiles.py and build_stand_asset.py make. What the data asset removed
was the asset PATH hardcoded in C++, where the editor cannot fix it up when the asset moves.
A script that fills the asset in is still content authoring; it is just repeatable.

ONE PROPERTY PAIR, and both or neither. A mesh without its anim class draws a truck whose
wheels never turn; an anim class without a mesh drives nothing at all. They are written
together and read back together below.
"""
import unreal

CONTENT = "/Game/DA_AirsideContent"
MESH = "/Game/Vehicles/FuelTruck1/SK_FuelTruck1"
ABP = "/Game/Vehicles/FuelTruck1/ABP_FuelTruck1"

MESH_PROPERTY = "vehicle_skeletal_mesh"
ANIM_PROPERTY = "vehicle_anim_class"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - nothing to point at the truck" % CONTENT)
        return

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        fail("%s not found - run import_fueltruck.py first" % MESH)
        return

    blueprint = unreal.EditorAssetLibrary.load_asset(ABP)
    if blueprint is None:
        fail("%s not found - run build_fueltruck_anim.py first" % ABP)
        return

    # THE GENERATED CLASS, not the Blueprint asset. TSoftClassPtr<UAnimInstance> wants the
    # UClass an Anim Blueprint compiles to; handing it the UAnimBlueprint sets a property
    # that type-checks in Python and resolves to nothing at runtime.
    anim_class = blueprint.generated_class()
    if anim_class is None:
        fail("%s has no generated class - it has not compiled" % ABP)
        return

    content.set_editor_property(MESH_PROPERTY, mesh)
    content.set_editor_property(ANIM_PROPERTY, anim_class)
    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)

    # READ BACK FROM DISK, because an asset that reports success and saves nothing is this
    # project's most familiar failure.
    reloaded = unreal.EditorAssetLibrary.load_asset(CONTENT)
    got_mesh = reloaded.get_editor_property(MESH_PROPERTY)
    got_anim = reloaded.get_editor_property(ANIM_PROPERTY)
    say("%s.%s = %s" % (CONTENT, MESH_PROPERTY, got_mesh))
    say("%s.%s = %s" % (CONTENT, ANIM_PROPERTY, got_anim))

    if got_mesh is None:
        fail("%s did not survive the save" % MESH_PROPERTY)
    else:
        say("PASS the content set names the truck's skeletal mesh")
    if got_anim is None:
        fail("%s did not survive the save" % ANIM_PROPERTY)
    else:
        say("PASS the content set names the truck's anim class")

    say("")
    say("NOTE the truck will draw, and drive, and its wheels will NOT turn until")
    say("     ABP_FuelTruck1's AnimGraph is wired by hand - see build_fueltruck_anim.py,")
    say("     which prints the bone plan. An unwired graph poses the reference pose, which")
    say("     is a correct-looking truck that slides.")
    say("DONE")


run()
