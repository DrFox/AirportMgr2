"""Authors the service-road cross-section and wires it into the content set. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

NO LAYOUT IS DEFINED HERE. Every band width, guideline class and flag comes from
URoadProfile::FillServiceRoad through Blueprint exposure, so the figures Airside.Build.
RoadProfileGuideline exercises and the figures the shipped asset carries are the same
figures rather than two transcriptions of them.

WHY THIS ASSET HAS TO EXIST AT ALL. A road segment stores a POINTER to its profile, and
URoadNetwork::DefaultProfile repairs a null one with the TAXIWAY profile - so a service road
laid against a transient profile would come back from a save as a taxiway: widened to 23 m
and, far worse, admitting aircraft onto a lane laid for vans. Without this asset the road
tool refuses to lay anything and says so. See UAirsideContent::ServiceRoadProfile.

The taxiway profile is deliberately NOT authored here. It has never been an asset: segments
that carry no profile fall back to ARoadNetworkActor's own FallbackWidth, which is
per-instance tuning the content set has no business overriding (see ResolveProfile).

Re-running replaces the asset, and that needs the editor CLOSED: a running editor holds the
.uasset open and create_asset then refuses under -unattended.
"""
import unreal

ASSET_DIR = "/Game"
CONTENT_SET = "/Game/DA_AirsideContent"

# The service road, in uu (a uu is a centimetre): a 6 m lane between 0.6 m kerbs, on a 7.5 m
# corner. Wide enough for two vans to pass, tight enough that a road reads as a road beside
# a 23 m taxiway. These are the defaults URoadProfile::MakeServiceRoadTransient uses, so the
# shipped asset and every test fixture are the same cross-section.
#
# FILLET_RADIUS WAS 500 UNTIL 2026-09-14, AND THE TRUCKS COULD NOT TURN ON IT. A rigid
# vehicle cannot follow an arc tighter than Wheelbase / sin(lock) at any speed; the fuel
# truck's is 4.71 m, and RoadNetworkSolver scales the preferred radius DOWN when a junction
# cannot fit it - 500 became 418 on the reported route, under the 510 the lock then needed,
# so every corner from depot to stand dropped to MinTaxiSpeed and crawled. Raised WITH
# ResolveDefaultVehicle's lock (45 -> 50 deg); the two are one decision, and
# Airside.Model.ServiceRoadFilletClearsTheTruckLock now fails if they drift apart.
#
# KEEP EQUAL TO URoadProfile::MakeServiceRoadTransient's default. This script authors
# DA_RoadProfile_ServiceRoad and the asset is what the game lays; the C++ default is what
# the tests exercise. They are two transcriptions of one cross-section.
LANE_WIDTH = 600.0
KERB_WIDTH = 60.0
FILLET_RADIUS = 750.0

# THE STANDARD TAXIWAY WIDTHS, by ICAO aerodrome code letter - the same reasoning
# UAirsideContent::RunwayProfiles gives for its own set: a taxiway conforms to one of these
# or it is not a taxiway, and a tool that picks from a list makes that true by construction
# rather than by validation.
#
# THE FILLET SCALES WITH THE WIDTH rather than sitting at one figure for all five. A junction
# between two 25 m taxiways that turned on the same radius as one between two 10.5 m ones
# would pave a corner the wider aircraft cannot use - and the corner radius is what decides
# whether an aircraft can take the turn at all (FSpeedProfile: R >= L/sin(lock)). Two thirds
# of the width is the ratio the standard 23 m taxiway already uses at its 1500 uu fillet.
TAXIWAY_WIDTHS = [
    ("B", 1050.0),
    ("C", 1500.0),
    ("D", 1800.0),
    ("E", 2300.0),   # the project's standard, and URoadProfile::StandardTaxiwayWidth
    ("F", 2500.0),
]
TAXIWAY_FILLET_RATIO = 2.0 / 3.0


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


def build_service_road():
    profile = replace_asset(
        "DA_RoadProfile_ServiceRoad", unreal.RoadProfile,
        data_asset_factory(unreal.RoadProfile))
    if profile is None:
        return None

    unreal.RoadProfile.fill_service_road(profile, LANE_WIDTH, KERB_WIDTH, FILLET_RADIUS)
    unreal.EditorAssetLibrary.save_asset("%s/DA_RoadProfile_ServiceRoad" % ASSET_DIR)

    bands = profile.get_editor_property("bands")
    guidelines = profile.get_editor_property("guidelines")
    unreal.log("MARKER: DA_RoadProfile_ServiceRoad built, %d bands, %d guideline(s)" % (
        len(bands), len(guidelines)))
    for band in bands:
        unreal.log("MARKER:   band %s %.0f uu slot '%s'" % (
            band.get_editor_property("type"), band.get_editor_property("width"),
            band.get_editor_property("material_slot")))
    for lane in guidelines:
        unreal.log("MARKER:   guideline class %s, %s, max wingspan %.0f" % (
            lane.get_editor_property("class_"), lane.get_editor_property("direction"),
            lane.get_editor_property("max_wingspan")))
    unreal.log("MARKER:   continuous=%s exit_length=%.0f" % (
        profile.get_editor_property("continuous_through_junctions"),
        profile.get_editor_property("exit_length")))
    return profile


def build_taxiways():
    """One asset per standard width, narrowest first - the order the tool cycles in."""
    built = []
    for letter, width in TAXIWAY_WIDTHS:
        name = "DA_RoadProfile_Taxiway_%s" % letter
        profile = replace_asset(name, unreal.RoadProfile, data_asset_factory(unreal.RoadProfile))
        if profile is None:
            continue

        fillet = width * TAXIWAY_FILLET_RATIO
        unreal.RoadProfile.fill(profile, width, fillet)
        unreal.EditorAssetLibrary.save_asset("%s/%s" % (ASSET_DIR, name))

        unreal.log("MARKER: %s built, %.1f m wide, %.1f m fillet" % (
            name, width / 100.0, fillet / 100.0))
        built.append(profile)
    return built


def wire_taxiways_into_content(profiles):
    """Point the content set's TaxiwayProfiles at them, in width order.

    NOT the actor's own Profile, which is per-instance tuning the content set has no
    business in - see ARoadNetworkActor::ResolveProfile. This is the standard set the width
    cycle offers, and a level that never uses the cycle is untouched by it.
    """
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        unreal.log_error("MARKER: %s not found - nothing to wire the taxiways into." % CONTENT_SET)
        return

    content.set_editor_property("taxiway_profiles", profiles)
    unreal.EditorAssetLibrary.save_asset(CONTENT_SET)
    unreal.log("MARKER: %s.TaxiwayProfiles -> %d profile(s)" % (
        CONTENT_SET, len(content.get_editor_property("taxiway_profiles"))))


def wire_into_content(profile):
    """Point the content set's ServiceRoadProfile at it.

    SET BY SCRIPT, and that does not contradict UAirsideContent's reason for existing. What
    that class removed was a PATH IN C++, invisible to the editor and unfixable when a folder
    moves. What is written here is a real asset reference inside a .uasset, which the editor
    repoints like any other - the same thing build_road_material_set.py and
    build_stand_asset.py already do for their own slots.
    """
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        unreal.log_error("MARKER: %s not found - nothing to wire the profile into." % CONTENT_SET)
        return

    content.set_editor_property("service_road_profile", profile)
    unreal.EditorAssetLibrary.save_asset(CONTENT_SET)
    unreal.log("MARKER: %s.ServiceRoadProfile -> %s" % (
        CONTENT_SET, content.get_editor_property("service_road_profile")))


road = build_service_road()
if road is not None:
    wire_into_content(road)

taxiways = build_taxiways()
if taxiways:
    wire_taxiways_into_content(taxiways)
