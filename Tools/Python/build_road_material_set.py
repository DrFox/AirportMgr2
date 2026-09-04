"""Authors DA_RoadMaterials, the name -> material table for profile bands. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

Run AFTER build_apron_material.py and build_kerb_material.py - it references what they
author and reports, rather than silently accepts, anything missing.

AUTHORED BY SCRIPT RATHER THAN BY HAND, and that is not a convenience. The INDEX of a slot
is the material id baked into every road mesh and the index handed to
ConfigureMaterialSet, which has no slot names and matches purely by position. Reordering
this table re-skins every existing surface with no error anywhere. A hand-edited asset
records no reason for its ordering; this file does.
"""
import unreal

MAT_DIR = "/Game/Materials"
ASSET_DIR = "/Game/Materials"
ASSET_NAME = "DA_RoadMaterials"

# ORDER IS THE CONTRACT. Append only; never insert, never sort.
#
# Asphalt is first so that slot 0 - the fallback for an unresolved or unnamed band - is the
# surface a road was made of before this table existed. A fallback that changed the look of
# every un-named band would make adding this asset a visual regression.
SLOTS = [
    ("Asphalt",  "%s/M_RoadSurface" % MAT_DIR),
    ("Concrete", "%s/M_ApronConcrete" % MAT_DIR),
    ("Kerb",     "%s/M_RoadKerb" % MAT_DIR),
]


def build_material_set():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (ASSET_DIR, ASSET_NAME)

    # Replaced in place through delete_loaded_asset, for the reason the material scripts
    # give: an asset referenced from C++ is already in memory, delete_asset then reports
    # success while the package stays loaded, and create_asset refuses unattended.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.RoadMaterialSet)

    asset = tools.create_asset(ASSET_NAME, ASSET_DIR, unreal.RoadMaterialSet, factory)
    if asset is None:
        unreal.log_error("MARKER: create_asset returned None for %s" % path)
        return None

    slots = []
    for name, material_path in SLOTS:
        material = unreal.load_asset(material_path)
        if material is None:
            # Reported, not skipped. Skipping would shorten the array and shift every later
            # slot's index - which silently re-skins the road rather than leaving one band
            # wrong. URoadMaterialSet::ResolveMaterials fills an empty slot with the engine
            # default, so a hole here is visible rather than invisible.
            unreal.log_error(
                "MARKER: %s not found - slot '%s' will be left empty, NOT removed" %
                (material_path, name))

        slot = unreal.RoadMaterialSlot()
        slot.set_editor_property("name", name)
        slot.set_editor_property("material", material)
        slots.append(slot)
        unreal.log("MARKER: slot %d = %s -> %s" % (len(slots) - 1, name, material_path))

    asset.set_editor_property("slots", slots)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("MARKER: %s built with %d slot(s)" % (path, len(slots)))
    return asset


build_material_set()
