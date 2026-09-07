"""Authors the aircraft types and the Code C stand. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

NO LAYOUT IS DEFINED HERE. Every number comes from the C++ builders through Blueprint
exposure, so the figures the tests exercise and the figures the shipped assets carry are
the same figures rather than two transcriptions of them - the class of duplication that
puts a hold door in one place in code and another in content.

Four assets, and the splits between them are the point:

  DA_Aircraft_A320 / DA_Aircraft_B738   where each service CONNECTS to that airframe
  DA_Stand_CodeC                        what the ground PROVIDES, its plant, and the lane
  DA_FuelDepot                          where the trucks live, and how many

Both types park on the same Code C stand and put their hold doors metres apart, which is
why the geometry cannot live on the concrete.

The depot is here rather than in its own script because it is the same KIND of thing - a
UEntityDefinition the player places - and it is authored the same way, from a C++ builder
rather than from numbers typed here.

Re-running replaces the assets, and that needs the editor CLOSED: a running editor holds
the .uasset open and create_asset then refuses under -unattended.

AND A CLOSED EDITOR IS NOT ALWAYS ENOUGH. An asset another LOADED asset references is held
open by that reference, inside the commandlet itself: DA_Stand_CodeC names DA_Aircraft_A320
as its design aircraft, so a re-run deletes the aircraft in memory, fails to recreate it, and
skips the stand that depended on it (observed 2026-09-07). Nothing reaches disk when that
happens - the originals are intact and the run is a no-op for those assets - but if the
aircraft genuinely need re-authoring, delete their .uasset files from disk first.
"""
import unreal

ASSET_DIR = "/Game/Entities"
CONTENT_SET = "/Game/DA_AirsideContent"


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

    # The design aircraft goes IN rather than being set afterwards: the stand's service loop
    # is measured from the aeroplane it is sized for, so the builder has to know which one.
    unreal.EntityDefinition.build_code_c_stand(stand, design_aircraft)

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

    # THE LANE, logged as its own fact. It is invisible in the editor - no mesh, no material,
    # no marking builder - so this line is the only place its corners can be read back.
    loop = stand.get_editor_property("service_loop")
    unreal.log("MARKER: DA_Stand_CodeC service loop, %d corner(s)" % len(loop))
    for corner in loop:
        unreal.log("MARKER:   (%.0f, %.0f)" % (corner.x, corner.y))
    return stand


def build_fuel_depot():
    depot = replace_asset(
        "DA_FuelDepot", unreal.EntityDefinition,
        data_asset_factory(unreal.EntityDefinition))
    if depot is None:
        return None

    unreal.EntityDefinition.build_fuel_depot(depot)
    unreal.EditorAssetLibrary.save_asset("%s/DA_FuelDepot" % ASSET_DIR)

    # ZERO ANCHORS IS THE CORRECT ANSWER, not a build that half ran: a depot's POSE is its
    # road connection, and a second lead-in from the same small building into the same road
    # would be a duplicate painted line. Logged so the zero reads as intended.
    unreal.log("MARKER: DA_FuelDepot built, pose role %s, %d truck(s), %d anchors" % (
        depot.get_editor_property("pose_role"),
        depot.get_editor_property("trucks"),
        len(depot.get_editor_property("anchors"))))
    extent = depot.get_editor_property("footprint_extent")
    unreal.log("MARKER:   footprint half-extent (%.0f, %.0f)" % (extent.x, extent.y))
    return depot


def wire_depot_into_content(depot):
    """Point the content set's DefaultFuelDepot at it.

    SET BY SCRIPT, like build_road_material_set.py's slots: what UAirsideContent removed was
    a PATH IN C++, invisible to the editor and unfixable when a folder moves. What is written
    here is a real asset reference inside a .uasset, which the editor repoints like any other.
    """
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        unreal.log_error("MARKER: %s not found - nothing to wire the depot into." % CONTENT_SET)
        return

    content.set_editor_property("default_fuel_depot", depot)
    unreal.EditorAssetLibrary.save_asset(CONTENT_SET)
    unreal.log("MARKER: %s.DefaultFuelDepot -> %s" % (
        CONTENT_SET, content.get_editor_property("default_fuel_depot")))


airbus = build_aircraft("DA_Aircraft_A320", unreal.AircraftType.build_a320)
build_aircraft("DA_Aircraft_B738", unreal.AircraftType.build737)

# The stand is sized for the A320, and draws it to show how it would be used. When aircraft
# exist, occupancy replaces this with whatever is actually parked.
if airbus is not None:
    build_stand(airbus)

fuel_depot = build_fuel_depot()
if fuel_depot is not None:
    wire_depot_into_content(fuel_depot)
