"""Authors DA_Stand_CodeC, the Code C contact stand definition. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

The LAYOUT IS NOT DEFINED HERE. It comes from UEntityDefinition::BuildCodeCStand, called
through Blueprint exposure, so the numbers the tests exercise and the numbers the shipped
asset carries are the same numbers rather than two transcriptions of them - the class of
duplication that puts a fuel truck in one place in code and another in content.

Re-running replaces the asset. As with the materials, that needs the editor CLOSED: a
running editor holds the .uasset open and create_asset then refuses under -unattended.
"""
import unreal

ASSET_DIR = "/Game/RoadNet/Entities"
ASSET_NAME = "DA_Stand_CodeC"


def build_stand_asset():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (ASSET_DIR, ASSET_NAME)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.EntityDefinition)

    definition = tools.create_asset(ASSET_NAME, ASSET_DIR, unreal.EntityDefinition, factory)
    if definition is None:
        unreal.log_error(
            "MARKER: create_asset returned None for %s - the asset is still loaded. "
            "Close the editor, or delete the .uasset from disk, and re-run." % path)
        return None

    # The one source of truth for the layout.
    unreal.EntityDefinition.build_code_c_stand(definition)

    # The same check PlaceEntity runs. An unnamed or duplicated anchor id makes two anchors
    # indistinguishable and a lookup silently returns the first, which is a fuel truck sent
    # to the belt loader's position with every appearance of success.
    if not unreal.EntityDefinition.has_usable_anchor_ids(definition):
        unreal.log_error("MARKER: %s has empty or duplicate anchor ids" % path)
        return None

    unreal.EditorAssetLibrary.save_asset(path)

    anchors = definition.get_editor_property("anchors")
    unreal.log("MARKER: %s built with %d anchors" % (path, len(anchors)))
    for anchor in anchors:
        local = anchor.get_editor_property("local_position")
        unreal.log("MARKER:   %s at (%.0f, %.0f)" % (
            anchor.get_editor_property("id"), local.x, local.y))
    return definition


build_stand_asset()
