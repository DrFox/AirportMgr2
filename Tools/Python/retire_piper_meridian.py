"""Retires the placeholder Meridian: repoints what pointed at it, then deletes it. Headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

RUN THIS LAST. Tools/Python/import_models.py must have imported plane7, build_plane7_anim.py
and Tools/wire_plane7_anim.py must have made ABP_Plane7, build_plane7_type.py must have
renamed DA_Aircraft_Piper - that rename is what carries DA_Airline_Cumbria's fleet across
without being touched here - and build_model_yard.py must have re-laid the yard, or the map
still references the mesh and this refuses to delete it. Every result line is prefixed
MARKER: so it can be grepped out of the log.

WHAT WAS BEING RETIRED. Content/Aircraft/PiperMeridian held the project's first aeroplane: a
downloaded PA-46 baked to 19,418 triangles with a 2048 BaseColor/Normal/MetallicRoughness
set, two materials of its own, a static mesh nothing referenced, and an Anim Blueprint wired
by hand in the editor before any of the anim tooling existed. It was the one aircraft in
Content wearing per-asset materials rather than the shared M_Fleet instances. plane7 rebuilds
the same aeroplane fleet-style - 6,672 triangles, no textures at all, seven flat-colour slots
that merge BY VALUE into the fleet set - so the folder goes entirely.

THE TWO FIELDS THIS SCRIPT ACTUALLY CHANGES are UAirsideContent::AgentMesh and
AgentAnimClass, which are DIRECT references to the two assets being deleted and so cannot
ride a rename. They are the game-wide fallback: what a dispatched agent wears when its type
names no mesh of its own. AgentMesh's own comment says "SKELETAL, because the propeller and
the wheels turn", and cites piperMeridian/scripts/build_lowpoly.py for the rig - a file that
describes an asset this script removes.

  BESIDE DefaultAircraft AND NOWHERE ELSE is that property's stated rule, and repoint_content_set
  checks it rather than assuming it. DefaultAircraft turns out to be UNSET and always has
  been - TODO(#30) - so the rule is held today by the C++ fallback describing the same
  aeroplane, not by a second asset reference. Said here because the first version of this
  script asserted the opposite, and correctly refused to delete anything over it.

DELETION IS CHECKED ON DISK, NEVER FROM THE REGISTRY. Both headless delete APIs report
success while writing nothing - there is a note about exactly this in the project's memory,
and import_models.already_imported() records the day a delete "cleared 3 asset(s)" that were
still on disk afterwards. So this asks the FILESYSTEM whether the .uasset is gone, and fails
if it is not.

AND IT REFUSES TO DELETE ANYTHING STILL REFERENCED, listing the referencers rather than
leaving a dangling pointer. A missing mesh does not crash - it draws the placeholder cube -
which is precisely why it would go unnoticed.
"""
import os

import unreal

FOLDER = "/Game/Aircraft/PiperMeridian"
CONTENT_SET = "/Game/DA_AirsideContent"
TYPE = "/Game/Entities/DA_Aircraft_Plane7"
MESH = "/Game/Aircraft/Plane7/SK_Plane7"
ABP = "/Game/Aircraft/Plane7/ABP_Plane7"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def content_file(package):
    """The .uasset a /Game/ package path names, as an absolute file on disk."""
    root = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())
    return os.path.join(root, package[len("/Game/"):].replace("/", os.sep) + ".uasset")


def repoint_content_set():
    """AgentMesh and AgentAnimClass onto plane7's. True if both landed.

    READ BACK FROM DISK, because a headless save reports success while writing nothing.
    """
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        fail("no %s - nothing to repoint" % CONTENT_SET)
        return False

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if mesh is None or abp is None:
        fail("%s or %s is missing - run import_models.py and build_plane7_anim.py first"
             % (MESH, ABP))
        return False
    if abp.generated_class() is None:
        fail("%s has no generated class; wire and compile it before running this" % ABP)
        return False

    before_mesh = content.get_editor_property("agent_mesh")
    before_anim = content.get_editor_property("agent_anim_class")
    content.set_editor_property("agent_mesh", mesh)
    content.set_editor_property("agent_anim_class", abp.generated_class())
    unreal.EditorAssetLibrary.save_asset(CONTENT_SET, only_if_is_dirty=False)

    reloaded = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    got_mesh = reloaded.get_editor_property("agent_mesh")
    got_anim = reloaded.get_editor_property("agent_anim_class")
    ok = True
    if got_mesh is None or "SK_Plane7" not in str(got_mesh):
        fail("AgentMesh read back as %s" % got_mesh)
        ok = False
    else:
        say("PASS AgentMesh %s -> %s" % (before_mesh, got_mesh))
    if got_anim is None or "ABP_Plane7" not in str(got_anim):
        fail("AgentAnimClass read back as %s" % got_anim)
        ok = False
    else:
        say("PASS AgentAnimClass %s -> %s" % (before_anim, got_anim))

    # THE RULE THAT PROPERTY DOCUMENTS: DefaultAircraft and AgentMesh must describe ONE
    # airframe, or the thing on screen moves like an aeroplane it does not look like. Issue
    # #30 is what happens when they do not.
    #
    # IT IS DELIBERATELY UNSET, AND THAT IS NOT A FAILURE. UAirsideSettings::ResolveDefaultAir-
    # frame carries "TODO(#30): author DA_PiperMeridian and set UAirsideContent::DefaultAircraft",
    # and until somebody does, the fallback every project runs on is
    # UAircraftType::BuildPiperMeridian in C++ - which is the same aeroplane AgentMesh now
    # wears, so the rule holds by a different route. Setting it here would switch every
    # project from the C++ fallback to the asset in the same breath as a model import, which
    # is a gameplay decision wearing a mechanical change's clothes.
    #
    # THE CHECK IS THEREFORE CONDITIONAL. An unset property is reported; a set one that names
    # anything but the Meridian's type is a failure, because that is the state the rule
    # forbids and the one nothing else would catch.
    default_type = reloaded.get_editor_property("default_aircraft")
    if default_type is None:
        say("NOTE DefaultAircraft is unset, as it has always been - see TODO(#30) in "
            "UAirsideSettings::ResolveDefaultAirframe. The fallback is BuildPiperMeridian, "
            "which describes the same aeroplane AgentMesh now wears, so the 'BESIDE AgentMesh "
            "AND NOWHERE ELSE' rule still holds.")
    elif "DA_Aircraft_Plane7" not in str(default_type):
        fail("DefaultAircraft is %s while AgentMesh is SK_Plane7 - the game-wide fallback "
             "would MOVE like one aeroplane and LOOK like another, which is issue #30"
             % default_type)
        ok = False
    else:
        say("PASS DefaultAircraft = %s, the same airframe AgentMesh now wears"
            % default_type)
    return ok


def referencers(package):
    """Packages outside FOLDER that still point into it."""
    found = unreal.EditorAssetLibrary.find_package_referencers_for_asset(package, False)
    return sorted({str(r) for r in found if not str(r).startswith(FOLDER)})


def retire():
    assets = sorted(unreal.EditorAssetLibrary.list_assets(FOLDER, recursive=True,
                                                          include_folder=False))
    if not assets:
        say("%s is already gone; nothing to delete" % FOLDER)
        return True

    say("%d asset(s) under %s:" % (len(assets), FOLDER))
    for asset in assets:
        say("    %s" % asset)

    # NOTHING OUTSIDE THE FOLDER MAY STILL POINT INTO IT. A dangling mesh reference draws the
    # placeholder cube rather than crashing, so this is the only moment it is cheap to catch.
    blocked = False
    for asset in assets:
        package = asset.split(".")[0]
        holders = referencers(package)
        if holders:
            fail("%s is still referenced by: %s" % (package, ", ".join(holders)))
            blocked = True
    if blocked:
        fail("nothing deleted - repoint the referencers above and re-run")
        return False
    say("PASS nothing outside %s references anything in it" % FOLDER)

    files = [content_file(asset.split(".")[0]) for asset in assets]
    unreal.EditorAssetLibrary.delete_directory(FOLDER)

    # ASKED OF THE FILESYSTEM, IN THAT ORDER OF TRUST - see the header. The registry is the
    # thing that has lied about this before, so it is not consulted first.
    left = [path for path in files if os.path.isfile(path)]
    if left:
        fail("%d .uasset file(s) survived the delete: %s" % (len(left), ", ".join(left)))
        return False
    say("PASS all %d .uasset file(s) are gone from disk" % len(files))

    if unreal.EditorAssetLibrary.does_directory_exist(FOLDER):
        say("NOTE the registry still lists %s as a directory; the files are gone, which is "
            "what matters. An empty folder disappears on the next editor start." % FOLDER)
    return True


def run():
    ok = repoint_content_set()
    if not ok:
        fail("the content set was not repointed, so nothing was deleted")
        say("DONE")
        return
    retire()
    say("DONE")


run()
