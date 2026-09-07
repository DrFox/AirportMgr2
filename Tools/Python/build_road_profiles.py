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

# The service road, in uu (a uu is a centimetre): a 6 m lane between 0.6 m kerbs, on a 5 m
# corner. Wide enough for two vans to pass, tight enough that a road reads as a road beside
# a 23 m taxiway. These are the defaults URoadProfile::MakeServiceRoadTransient uses, so the
# shipped asset and every test fixture are the same cross-section.
LANE_WIDTH = 600.0
KERB_WIDTH = 60.0
FILLET_RADIUS = 500.0


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
