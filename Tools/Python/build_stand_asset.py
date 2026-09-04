"""Authors the aircraft types and the Code C stand. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

NO LAYOUT IS DEFINED HERE. Every number comes from the C++ builders through Blueprint
exposure, so the figures the tests exercise and the figures the shipped assets carry are
the same figures rather than two transcriptions of them - the class of duplication that
puts a hold door in one place in code and another in content.

Three assets, and the split between them is the point:

  DA_Aircraft_A320 / DA_Aircraft_B738   where each service CONNECTS to that airframe
  DA_Stand_CodeC                        what the ground PROVIDES, and its fixed plant

Both types park on the same Code C stand and put their hold doors metres apart, which is
why the geometry cannot live on the concrete.

Re-running replaces the assets, and that needs the editor CLOSED: a running editor holds
the .uasset open and create_asset then refuses under -unattended.
"""
import unreal

ASSET_DIR = "/Game/Entities"


def replace_asset(name, asset_class, factory):
    """Delete any existing asset of this name and create a fresh one."""
    path = "%s/%s" % (ASSET_DIR, name)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    created = tools.create_asset(name, ASSET_DIR, asset_class, factory)
    if created is None:
        unreal.log_error(
            "MARKER: create_asset returned None for %s - the asset is still loaded. "
            "Close the editor, or delete the .uasset from disk, and re-run." % path)
    return created


def data_asset_factory(asset_class):
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", asset_class)
    return factory


def build_aircraft(name, builder):
    aircraft = replace_asset(name, unreal.AircraftType, data_asset_factory(unreal.AircraftType))
    if aircraft is None:
        return None

    builder(aircraft)

    # The same check the model runs. An unnamed or duplicated id makes two service points
    # indistinguishable and a lookup silently returns the first, which is a belt loader
    # sent to the refuel panel with every appearance of success.
    if not unreal.AircraftType.has_usable_service_ids(aircraft):
        unreal.log_error("MARKER: %s has empty or duplicate service point ids" % name)
        return None

    unreal.EditorAssetLibrary.save_asset("%s/%s" % (ASSET_DIR, name))

    points = aircraft.get_editor_property("service_points")
    unreal.log("MARKER: %s built, %d service points" % (name, len(points)))
    for point in points:
        local = point.get_editor_property("local_position")
        unreal.log("MARKER:   %s at (%.0f, %.0f)" % (
            point.get_editor_property("id"), local.x, local.y))
    return aircraft


def build_stand(design_aircraft):
    stand = replace_asset(
        "DA_Stand_CodeC", unreal.EntityDefinition,
        data_asset_factory(unreal.EntityDefinition))
    if stand is None:
        return None

    unreal.EntityDefinition.build_code_c_stand(stand)
    stand.set_editor_property("design_aircraft", design_aircraft)

    if not unreal.EntityDefinition.has_usable_anchor_ids(stand):
        unreal.log_error("MARKER: DA_Stand_CodeC has empty or duplicate fixture ids")
        return None

    unreal.EditorAssetLibrary.save_asset("%s/DA_Stand_CodeC" % ASSET_DIR)

    fixtures = stand.get_editor_property("anchors")
    unreal.log("MARKER: DA_Stand_CodeC built, %d ground fixtures" % len(fixtures))
    for fixture in fixtures:
        local = fixture.get_editor_property("local_position")
        unreal.log("MARKER:   %s at (%.0f, %.0f)" % (
            fixture.get_editor_property("id"), local.x, local.y))
    return stand


airbus = build_aircraft("DA_Aircraft_A320", unreal.AircraftType.build_a320)
build_aircraft("DA_Aircraft_B738", unreal.AircraftType.build737)

# The stand is sized for the A320, and draws it to show how it would be used. When aircraft
# exist, occupancy replaces this with whatever is actually parked.
if airbus is not None:
    build_stand(airbus)
