"""Points DA_AirsideContent at the articulated rig - truckCab1 and tankTrailer1 - so
UAirsideSettings::ResolveRigView resolves both meshes and both ABPs. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Result lines are prefixed MARKER: and land in Saved/Logs/AirportMgr.log.

MIRRORS build_fueltruck_content.py: SET BY SCRIPT rather than by hand, which does not
contradict UAirsideContent's reason for existing - the asset PATH is still a reference the
editor maintains, this just fills it in repeatably. TWO PROPERTY PAIRS, each written and read
back together: a mesh without its anim class draws a rig whose wheels never turn, and an anim
class without a mesh drives nothing.
"""
import unreal

CONTENT = "/Game/DA_AirsideContent"

CAB_MESH = "/Game/Vehicles/Rig/TruckCab1/SK_TruckCab1"
CAB_ABP = "/Game/Vehicles/Rig/TruckCab1/ABP_TruckCab1"
TRAILER_MESH = "/Game/Vehicles/Rig/TankTrailer1/SK_TankTrailer1"
TRAILER_ABP = "/Game/Vehicles/Rig/TankTrailer1/ABP_TankTrailer1"

PAIRS = [
    ("rig_cab_mesh", CAB_MESH, "rig_cab_anim_class", CAB_ABP),
    ("rig_trailer_mesh", TRAILER_MESH, "rig_trailer_anim_class", TRAILER_ABP),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        fail("%s not found - nothing to point at the rig" % CONTENT)
        return

    ok = True
    for mesh_property, mesh_path, anim_property, abp_path in PAIRS:
        mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
        if mesh is None:
            fail("%s not found - run import_rig.py first" % mesh_path)
            ok = False
            continue

        blueprint = unreal.EditorAssetLibrary.load_asset(abp_path)
        if blueprint is None:
            fail("%s not found - run build_rig_anim.py first" % abp_path)
            ok = False
            continue

        # THE GENERATED CLASS, not the Blueprint asset - see build_fueltruck_content.py's own
        # note: TSoftClassPtr<UAnimInstance> wants the UClass an Anim Blueprint compiles to.
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
    say("NOTE the rig will draw, and drive, once ARoadAgentActor spawns it - and neither")
    say("     ABP's AnimGraph is wired yet: see build_rig_anim.py's printed bone plan. An")
    say("     unwired graph poses the reference pose, which is a correct-looking rig that")
    say("     slides with its wheels and steering held still.")
    say("rig content: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


run()
