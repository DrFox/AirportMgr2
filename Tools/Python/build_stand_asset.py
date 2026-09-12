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

Re-running RE-AUTHORS IN PLACE, and needs the editor CLOSED: a running editor holds the
.uasset open and the save then fails.

IN PLACE, not delete-and-recreate, since 2026-09-07. Deleting was tried and cannot work here:
an asset another LOADED asset references is held open by that reference inside the commandlet
itself - DA_Stand_CodeC names DA_Aircraft_A320 as its design aircraft, and M_Starter names
DA_Stand_CodeC - so a re-run deleted the aircraft in memory, could not recreate it, and
skipped the stand that depended on it. Deleting the .uasset files from disk instead would
break the level's reference to them, which is worse.

The trade of re-authoring in place is that a field the C++ builder does NOT set keeps
whatever was last saved into it. So each builder sets every field it owns, defaults
included - see UEntityDefinition::BuildCodeCStand, which states PoseRole, FootprintExtent
and Trucks explicitly for exactly this reason.
"""
import unreal

ASSET_DIR = "/Game/Entities"
CONTENT_SET = "/Game/DA_AirsideContent"


def replace_asset(name, asset_class, factory):
    """The asset to author: the existing one if there is one, else a fresh one.

    Returns the SAME UObject every level and asset already points at, so re-running never
    breaks a reference - see the module docstring for why deleting cannot work here.
    """
    path = "%s/%s" % (ASSET_DIR, name)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.log("MARKER: re-authoring existing %s in place" % path)
            return existing
        unreal.log_error("MARKER: %s exists but will not load - re-author it by hand." % path)
        return None

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    created = tools.create_asset(name, ASSET_DIR, asset_class, factory)
    if created is None:
        unreal.log_error("MARKER: create_asset returned None for %s." % path)
    return created


def save(name):
    """Write the asset to disk, DIRTY OR NOT.

    only_if_is_dirty defaults to True and that silently did nothing here: the C++ builders
    write the object's properties directly and nothing marks the package dirty, so an asset
    re-authored in place was rebuilt in memory and never reached disk. Invisible while this
    script created every asset from scratch - a brand new package is dirty by construction -
    and it cost a run to find the first time a re-author kept the existing object
    (2026-09-07). The save is cheap and the assets are small; always write.
    """
    path = "%s/%s" % (ASSET_DIR, name)
    if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
        unreal.log_error("MARKER: save_asset refused %s - nothing reached disk." % path)


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

    save(name)

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

    save("DA_Stand_CodeC")

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
    save("DA_FuelDepot")

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
    unreal.EditorAssetLibrary.save_asset(CONTENT_SET, only_if_is_dirty=False)
    unreal.log("MARKER: %s.DefaultFuelDepot -> %s" % (
        CONTENT_SET, content.get_editor_property("default_fuel_depot")))


airbus = build_aircraft("DA_Aircraft_A320", unreal.AircraftType.build_a320)
build_aircraft("DA_Aircraft_B738", unreal.AircraftType.build737)

# The Meridian, as an ASSET rather than only as ResolveDefaultAirframe's fallback.
#
# Added 2026-09-11 for the offer inbox: M_Starter's runway is 15 m wide and admits a 15 m
# wingspan, so neither Code C type can ever land there and every offer was refused at the
# gate. A light type an airline can actually list is what gives a GA field traffic - and
# what makes the jets a REASON to build a wider runway rather than a broken inbox.
#
# Built by the same UAircraftType::BuildPiperMeridian the fallback's figures come from, so
# the asset cannot drift from the aeroplane already on screen. That is the whole point of
# the builders being C++: one description, two consumers.
build_aircraft("DA_Aircraft_Piper", unreal.AircraftType.build_piper_meridian)

# The stand is sized for the A320, and draws it to show how it would be used. When aircraft
# exist, occupancy replaces this with whatever is actually parked.
if airbus is not None:
    build_stand(airbus)

fuel_depot = build_fuel_depot()
if fuel_depot is not None:
    wire_depot_into_content(fuel_depot)
