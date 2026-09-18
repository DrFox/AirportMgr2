"""Sets bUsedWithSkeletalMesh on Content/Vehicles/FuelTruck1's materials. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY THIS EXISTS. import_fueltruck.py imports SK_FuelTruck1 as a SKELETAL mesh and lets
Interchange generate its fourteen materials from the glTF - and never sets the one flag that
decides whether they can draw on a skeletal mesh at all. A UMaterial with
bUsedWithSkeletalMesh false CANNOT compile for the skeletal vertex factory, so the renderer
substitutes the DEFAULT grey material. The component still holds the correct materials and
logs their correct names, which is why the symptom reads as "the materials were never set".

It hides particularly well: the editor sets the flag ITSELF on first use and recompiles, so
the truck looks right in the skeletal mesh editor and grey in PIE - but that only marks the
material DIRTY, so the fix dies with the session unless something saves it.

This is exactly the defect that was reported against plane2 on 2026-09-12 ("when the twotter
spawned none of its materials were set"). import_plane2.py fixed it at the import; the truck
was imported by a different script that did not learn it.

REPAIR IN PLACE, NOT RE-IMPORT. import_fueltruck.py's clear_previous() deletes the whole of
/Game/Vehicles/FuelTruck1 first, and UAirsideContent::VehicleMeshRigged points at
SK_FuelTruck1 - deleting an asset something references breaks the reference rather than
updating it. Mutating fourteen materials touches nothing that points at them.

MEASURED BEFORE IT IS MENDED. The flag is READ AND REPORTED first, because "these were
broken" is a claim this script is in a position to prove or to withdraw, and asserting a
defect that was not there is as expensive as missing one.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import flag_for_skeletal, say, fail  # noqa: E402


MESH_DIR = "/Game/Vehicles/FuelTruck1"
MESH = "%s/SK_FuelTruck1" % MESH_DIR


def survey(when):
    """Every material under the folder and the state of its flag."""
    off = []
    on = []
    for path in unreal.EditorAssetLibrary.list_assets("%s/Materials" % MESH_DIR, recursive=True):
        material = unreal.EditorAssetLibrary.load_asset(path)
        if not isinstance(material, unreal.Material):
            continue
        name = path.split("/")[-1].split(".")[0]
        if material.get_editor_property("used_with_skeletal_mesh"):
            on.append(name)
        else:
            off.append(name)
    say("%s: %d material(s) flagged, %d NOT flagged" % (when, len(on), len(off)))
    if off:
        say("  would draw as grey clay: %s" % ", ".join(sorted(off)))
    return on, off


def main():
    say("=" * 78)
    if not unreal.EditorAssetLibrary.does_asset_exist(MESH):
        fail("%s does not exist - nothing to repair" % MESH)
        say("DONE")
        return

    before_on, before_off = survey("before")
    if not before_on and not before_off:
        fail("no materials found under %s/Materials - the truck's slots point somewhere "
             "else, and this script is looking in the wrong place" % MESH_DIR)
        say("DONE")
        return

    if not before_off:
        say("nothing to do: every material was already flagged. The grey-clay defect was a "
            "PREDICTION from reading import_fueltruck.py, and it was wrong - the flag got "
            "set some other way. Leaving the assets alone.")
        say("DONE")
        return

    if not flag_for_skeletal(MESH_DIR):
        say("DONE")
        return

    # SAVED, FORCED. set_editor_property leaves the package dirty, but save_asset defaults to
    # only_if_is_dirty=True and a package the editor does not think dirty is silently skipped
    # - which is how a correct in-memory fix reaches nothing on disk.
    saved = 0
    for path in unreal.EditorAssetLibrary.list_assets("%s/Materials" % MESH_DIR, recursive=True):
        if unreal.EditorAssetLibrary.save_asset(path.split(".")[0], only_if_is_dirty=False):
            saved += 1
    say("saved %d material package(s)" % saved)

    # READ BACK THE WAY A FRESH EDITOR WOULD. flag_for_skeletal already re-read the flag off
    # the live objects; this re-reads after the save, which is the step that can silently do
    # nothing. Anything still off here means the write never reached disk.
    unreal.EditorAssetLibrary.load_asset(MESH)
    after_on, after_off = survey("after")
    if after_off:
        fail("%d material(s) are still unflagged after saving: %s. The packages did not "
             "reach disk - check that the editor is closed, and check git status on "
             "Content/Vehicles/FuelTruck1." % (len(after_off), ", ".join(sorted(after_off))))
    else:
        say("PASS all %d material(s) flagged and saved; %d of them were broken before this "
            "run: %s" % (len(after_on), len(before_off), ", ".join(sorted(before_off))))
    say("=" * 78)
    say("DONE")


main()
