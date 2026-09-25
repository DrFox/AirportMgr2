"""Points DA_AirsideContent at utility1 + fuelTrailer1 - the drawbar tow chain - so
UAirsideSettings::ResolveUtilityTowView resolves the cab's mesh/ABP and the trailer's. Run
headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Result lines are prefixed MARKER: and land in Saved/Logs/AirportMgr.log.

MIRRORS build_rig_content.py: SET BY SCRIPT rather than by hand, for the same reason - the
asset PATH is still a reference the editor maintains, this just fills it in repeatably. TWO
PROPERTY PAIRS, each written and read back together, for the reason build_fueltruck_content.py
gives: a mesh without its anim class draws a vehicle whose wheels never turn, and an anim class
without a mesh drives nothing.
"""
import unreal

CONTENT = "/Game/DA_AirsideContent"

UTILITY_MESH = "/Game/Vehicles/Utility1/SK_Utility1"
UTILITY_ABP = "/Game/Vehicles/Utility1/ABP_Utility1"
TRAILER_MESH = "/Game/Vehicles/FuelTrailer1/SK_FuelTrailer1"
TRAILER_ABP = "/Game/Vehicles/FuelTrailer1/ABP_FuelTrailer1"

PAIRS = [
    ("utility_mesh", UTILITY_MESH, "utility_anim_class", UTILITY_ABP),
    ("utility_trailer_mesh", TRAILER_MESH, "utility_trailer_anim_class", TRAILER_ABP),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - nothing to point at the tow chain" % CONTENT)
        return

    ok = True
    for mesh_property, mesh_path, anim_property, abp_path in PAIRS:
        mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
        if mesh is None:
            fail("%s not found - run reimport_utility1.py / import_fueltrailer1.py first"
                 % mesh_path)
            ok = False
            continue

        blueprint = unreal.EditorAssetLibrary.load_asset(abp_path)
        if blueprint is None:
            fail("%s not found - run build_rig_anim.py first" % abp_path)
            ok = False
            continue

        # THE GENERATED CLASS, not the Blueprint asset - build_fueltruck_content.py's own note:
        # TSoftClassPtr<UAnimInstance> wants the UClass an Anim Blueprint compiles to.
        anim_class = blueprint.generated_class()
        if anim_class is None:
            fail("%s has no generated class - it has not compiled" % abp_path)
            ok = False
            continue

        content.set_editor_property(mesh_property, mesh)
        content.set_editor_property(anim_property, anim_class)

    unreal.EditorAssetLibrary.save_asset(CONTENT, only_if_is_dirty=False)

    # READ BACK FROM DISK - an asset that reports success and saves nothing is this project's
    # most familiar failure.
    reloaded = unreal.EditorAssetLibrary.load_asset(CONTENT)
    for mesh_property, _, anim_property, _ in PAIRS:
        got_mesh = reloaded.get_editor_property(mesh_property)
        got_anim = reloaded.get_editor_property(anim_property)
        say("%s.%s = %s" % (CONTENT, mesh_property, got_mesh))
        say("%s.%s = %s" % (CONTENT, anim_property, got_anim))
        if got_mesh is None:
            fail("%s did not survive the save" % mesh_property)
            ok = False
        else:
            say("PASS the content set names %s" % mesh_property)
        if got_anim is None:
            fail("%s did not survive the save" % anim_property)
            ok = False
        else:
            say("PASS the content set names %s" % anim_property)

    say("")
    say("NOTE utility1 will draw, and drive, once ARoadAgentActor spawns it. ABP_Utility1's")
    say("     AnimGraph is NOT wired yet - see build_rig_anim.py's printed plan. fuelTrailer1's")
    say("     ABP wires wheel spin only (Task 4 owns steer_FL/FR and towbar_yaw - see")
    say("     build_rig_anim.py's own header, 'A SECOND CARVE-OUT').")
    say("utility tow content: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


run()
