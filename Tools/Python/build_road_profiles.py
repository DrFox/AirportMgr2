"""Authors the road cross-sections and wires them into the content set. Run headless:

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
tool refuses to lay anything and says so. See UAirsideContent::ServiceRoadProfiles.

The taxiway profile is deliberately NOT authored here. It has never been an asset: segments
that carry no profile fall back to ARoadNetworkActor's own FallbackWidth, which is
per-instance tuning the content set has no business overriding (see ResolveProfile).

Re-running FILLS EACH ASSET IN PLACE (since 2026-09-23). It used to delete and recreate, which
dies on references: DA_AirsideContent and every saved level point at these assets, and a
delete under -unattended either refuses or leaves the referencers pointing at nothing. It
still needs the editor CLOSED, because a running editor holds the .uasset open.
"""
import unreal

ASSET_DIR = "/Game"
CONTENT_SET = "/Game/DA_AirsideContent"

# The service road's CROSS-SECTION, in uu (a uu is a centimetre): two 3 m lanes, one each way,
# between 0.6 m kerbs - 7.2 m overall, the NARROW road. LANE_WIDTH IS PER LANE since
# 2026-09-23; until then it was kerb to kerb, so "600" meant 4.8 m of carriageway. These are
# the defaults URoadProfile::MakeServiceRoadTransient uses, so the shipped asset and every
# test fixture are the same cross-section.
#
# THE CORNER IS NOT STATED HERE, and this comment used to name one - "on a 7.5 m corner" -
# for a breath after the constant beneath it had gone. A figure in prose drifts exactly as a
# figure in code does, and is harder to grep. The corner is whatever
# URoadProfile::ResolvedFilletRadius derives; at the vehicles admitted on 2026-09-16 that is
# about 11.2 m, and it will move again the day a bigger one is admitted.
#
# THERE IS NO FILLET_RADIUS HERE ANY MORE, and its absence is the point. It was 500 until
# 2026-09-14 and 750 after, while the figure it had to agree with - the truck's
# Wheelbase / sin(lock) - lived in another language in another directory. The two drifted ten
# centimetres apart, RoadNetworkSolver scaled 500 down to 418 to fit a junction, and every
# corner from depot to stand crawled. The fix at the time went the WRONG WAY: the truck's
# steering lock was widened to fit the road.
#
# The radius is now DERIVED in C++ from the largest service vehicle admitted - see
# URoadProfile::ResolvedFilletRadius - so this script states nothing and can drift from
# nothing. Passing zero below is what asks for that derivation.
LANE_WIDTH = 300.0

# THE ROAD TIERS (spec 2026-09-23 section 1), narrow first - the order the road tool cycles in.
# Per-lane widths; every tier is two lanes between the same kerbs. The Narrow tier keeps the
# asset name every placed road already points at, so existing roads stay Narrow.
ROAD_TIERS = [
    ("DA_RoadProfile_ServiceRoad", LANE_WIDTH),          # Narrow, 2 x 3.0 m
    ("DA_RoadProfile_ServiceRoad_Standard", 350.0),      # Standard, 2 x 3.5 m
    ("DA_RoadProfile_ServiceRoad_Wide", 450.0),          # Wide, 2 x 4.5 m - the articulated rig's
]
KERB_WIDTH = 60.0
DERIVE_FILLET = 0.0

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
    """The existing asset of this name, to be refilled in place; created only if missing.

    NOT delete-and-recreate - see the module docstring. The caller's Fill resets every band
    and guideline, so reusing the object leaves nothing of the old cross-section behind.
    """
    path = "%s/%s" % (ASSET_DIR, name)

    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.log("MARKER: refilling existing %s in place" % path)
            return existing

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


def build_service_roads():
    """One asset per road tier, narrowest first - the order the tool cycles in."""
    built = []
    for name, lane_width in ROAD_TIERS:
        profile = replace_asset(name, unreal.RoadProfile, data_asset_factory(unreal.RoadProfile))
        if profile is None:
            continue

        unreal.RoadProfile.fill_two_way_road(profile, lane_width, KERB_WIDTH, DERIVE_FILLET)
        # only_if_is_dirty=False: a Fill through Python does not mark the package dirty, and the
        # default then saves NOTHING while reporting success (memory: save_asset writes nothing
        # unless forced).
        unreal.EditorAssetLibrary.save_asset("%s/%s" % (ASSET_DIR, name), only_if_is_dirty=False)

        bands = profile.get_editor_property("bands")
        guidelines = profile.get_editor_property("guidelines")
        unreal.log("MARKER: %s built, %d bands, %d guideline(s), %.1f m overall" % (
            name, len(bands), len(guidelines),
            sum(band.get_editor_property("width") for band in bands) / 100.0))
        for band in bands:
            unreal.log("MARKER:   band %s %.0f uu slot '%s'" % (
                band.get_editor_property("type"), band.get_editor_property("width"),
                band.get_editor_property("material_slot")))
        for lane in guidelines:
            unreal.log("MARKER:   guideline class %s, %s, offset %.0f" % (
                lane.get_editor_property("class_"), lane.get_editor_property("direction"),
                lane.get_editor_property("centre_offset")))
        built.append(profile)
    return built

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
        unreal.EditorAssetLibrary.save_asset("%s/%s" % (ASSET_DIR, name), only_if_is_dirty=False)

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


def wire_into_content(profiles):
    """Point the content set's ServiceRoadProfiles at the tiers, narrowest first.

    SET BY SCRIPT, and that does not contradict UAirsideContent's reason for existing. What
    that class removed was a PATH IN C++, invisible to the editor and unfixable when a folder
    moves. What is written here is a real asset reference inside a .uasset, which the editor
    repoints like any other - the same thing build_road_material_set.py and
    build_stand_asset.py already do for their own slots.
    """
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        unreal.log_error("MARKER: %s not found - nothing to wire the profiles into." % CONTENT_SET)
        return

    content.set_editor_property("service_road_profiles", profiles)
    unreal.EditorAssetLibrary.save_asset(CONTENT_SET, only_if_is_dirty=False)
    unreal.log("MARKER: %s.ServiceRoadProfiles -> %d profile(s)" % (
        CONTENT_SET, len(content.get_editor_property("service_road_profiles"))))


roads = build_service_roads()
if roads:
    wire_into_content(roads)

taxiways = build_taxiways()
if taxiways:
    wire_taxiways_into_content(taxiways)
