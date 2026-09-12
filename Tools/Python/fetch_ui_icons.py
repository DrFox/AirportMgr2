"""Downloads the UI icons from game-icons.net and imports them. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

EVERY DOWNLOAD IS VALIDATED, because a wrong name fails SILENTLY. Asking for an icon that
does not exist returns HTTP 200 with an HTML 404 page: curl reports success and writes a
file that is not an image. Verified during design with 'lorc/cancel'. So each result is
checked for a PNG signature and a failure NAMES the icon rather than leaving a blank button.
Never weaken that check to get a name past it - fix the name instead.

THE BACKGROUND IS TRANSPARENT, NOT BLACK, and the alpha channel is asserted for the same
reason the signature is. The design spec called for "white-on-black silhouettes, which tint
cleanly"; they do not. UMG's tint MULTIPLIES, so white glyph on OPAQUE black tints to
`glyph=tint, background=black` - a black tile behind every icon on a slate panel. Caught by
compositing the first fetch over the panel colour and looking at it. Transparent gives a
real alpha channel, and a silhouette that takes any slot colour on any surface.

ATTRIBUTION IS AN OBLIGATION. game-icons.net is CC-BY: each author must be credited. The
credits file is written from the SAME table that drives the downloads, so it cannot drift
from what was actually fetched.
"""
import io
import json
import os
import urllib.request

import unreal

OUT_DIR = "/Game/UI/Icons"
ATTRIBUTION = os.path.join(
    unreal.Paths.project_content_dir(), "UI", "Icons", "ATTRIBUTION.md")

# White on TRANSPARENT at 1x1 aspect: a silhouette that tints cleanly to any palette slot
# on any surface. See the module docstring for why black does not.
URL = "https://game-icons.net/icons/ffffff/transparent/1x1/%s/%s.png"

# FBuildAction::Id -> (author, icon name on game-icons.net)
#
# THE IDS ARE EXACT AND LOWERCASE, taken from BuildActions.cpp. Tool ids are built as
# "tool.<display text lowercased>", so two of them CONTAIN A SPACE - "tool.holding point"
# and "tool.fuel depot". That is why the style map is written from the manifest below
# rather than derived from asset names: a space is not legal in an asset name, so the
# round-trip name -> id cannot be reversed.
#
# EVERY NAME BELOW WAS FETCHED AND CHECKED. Six of a first guess of seventeen did not exist
# (stone-block, runway, pathfinder, cursor, plus-circle, pause-button) and each returned an
# HTML page under HTTP 200. game-icons.net tag pages list valid names and are the way to
# find more: curl https://game-icons.net/tags/<tag>.html and grep for author/name.
#
# THE TIME CONTROLS HAVE NO ICON, deliberately. Slower, pause and faster are geometric
# glyphs that render exactly as text and need no texture; the bar draws them as such.
ICONS = {
    "tool.select":         ("delapouite", "mouse"),
    "tool.taxiway":        ("delapouite", "path-tile"),
    "tool.apron":          ("delapouite", "stone-path"),
    "tool.stand":          ("delapouite", "position-marker"),
    "tool.guidelines":     ("delapouite", "path-distance"),
    "tool.runway":         ("delapouite", "airplane-arrival"),
    "tool.holding point":  ("delapouite", "stop-sign"),
    "tool.road":           ("delapouite", "road"),
    "tool.fuel depot":     ("delapouite", "fuel-tank"),

    "edit.remove":         ("delapouite", "trash-can"),
    "edit.insert":         ("delapouite", "split-arrows"),
    "edit.undo":           ("delapouite", "anticlockwise-rotation"),
    "edit.redo":           ("delapouite", "clockwise-rotation"),
    "edit.clear":          ("delapouite", "broom"),

    "aircraft.land":       ("delapouite", "commercial-airplane"),
    "aircraft.guidelines": ("delapouite", "control-tower"),

    "selection.depart":    ("delapouite", "airplane-departure"),
    "selection.follow":    ("delapouite", "binoculars"),

    "game.save":           ("delapouite", "save"),
    "game.load":           ("delapouite", "load"),
}

# The notification icons, one per ENotificationSeverity. Keyed by the severity name rather
# than an action id: these do not come from the action registry and never will, so they are
# fetched alongside but mapped to NAMED fields on UUIStyle (see IconInfo and friends).
NOTIFICATION_ICONS = {
    "info":    ("delapouite", "info"),
    "success": ("delapouite", "check-mark"),
    "warning": ("lorc",       "hazard-sign"),
}

# Actions that are drawn as a glyph rather than a texture. The bar skips the icon lookup
# for these, and AirportMgr.UI.EveryActionResolvesAnIcon skips them too - BY SECTION, so a
# fourth time control does not need that test edited.
GLYPH_ONLY = {"time.slower": "<<", "time.pause": "||", "time.faster": ">>"}


def asset_name_for(action_id):
    """T_Icon_<id with dots and spaces as underscores>.

    LOSSY ON PURPOSE and the loss is why manifest.json exists: "tool.holding point" and a
    hypothetical "tool.holding.point" would both land on T_Icon_tool_holding_point, so the
    reverse mapping cannot be derived. build_ui_style.py reads the manifest instead.
    """
    return "T_Icon_%s" % action_id.replace(".", "_").replace(" ", "_")


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def has_alpha(data):
    """True when this PNG carries real transparency. No PIL: the editor's Python lacks it.

    IHDR is always the first chunk, so its colour type is at a fixed offset: 4 and 6 are
    grey+alpha and RGBA. Type 3 is a palette, whose transparency lives in a tRNS chunk
    instead - which is what game-icons.net actually returns.
    """
    if len(data) < 26:
        return False
    colour_type = data[25]
    return colour_type in (4, 6) or (colour_type == 3 and b"tRNS" in data)


def fetch(author, name):
    """Bytes of a real PNG, or None. Validation is the point - see the module docstring."""
    url = URL % (author, name)
    try:
        with urllib.request.urlopen(url, timeout=30) as response:
            data = response.read()
    except Exception as exc:
        fail("%s/%s could not be fetched: %s" % (author, name, exc))
        return None

    # HTTP 200 is NOT enough. A wrong name returns an HTML page with a 200.
    if not data.startswith(b"\x89PNG"):
        fail("%s/%s returned %d bytes that are not a PNG - check the author and name at %s"
             % (author, name, len(data), url))
        return None

    # An opaque icon is a black tile on the panel once UMG multiplies the tint through it.
    if not has_alpha(data):
        fail("%s/%s has no alpha channel - it would render as a black tile behind the "
             "glyph. Check the background segment of %s is 'transparent'." % (author, name, url))
        return None
    return data


def import_one(tools, staging, key, author, name, asset_name):
    """Fetch, validate and import one icon. Returns True when the asset is on disk."""
    data = fetch(author, name)
    if data is None:
        return False

    png = os.path.join(staging, "%s.png" % name)
    with open(png, "wb") as handle:
        handle.write(data)

    task = unreal.AssetImportTask()
    task.filename = png
    task.destination_path = OUT_DIR
    task.destination_name = asset_name
    task.automated = True
    task.replace_existing = True
    task.save = True
    tools.import_asset_tasks([task])

    path = "%s/%s" % (OUT_DIR, asset_name)
    texture = unreal.EditorAssetLibrary.load_asset(path)
    if texture is None:
        fail("%s imported nothing" % path)
        return False

    texture.set_editor_property("compression_settings",
                                unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property("srgb", True)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("imported %s from %s/%s" % (asset_name, author, name))
    return True


def run():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    staging = os.path.join(unreal.Paths.project_saved_dir(), "UIIcons")
    os.makedirs(staging, exist_ok=True)

    credited = []
    failures = 0
    for action_id, (author, name) in sorted(ICONS.items()):
        data = fetch(author, name)
        if data is None:
            failures += 1
            continue

        png = os.path.join(staging, "%s.png" % name)
        with open(png, "wb") as handle:
            handle.write(data)

        asset_name = asset_name_for(action_id)
        task = unreal.AssetImportTask()
        task.filename = png
        task.destination_path = OUT_DIR
        task.destination_name = asset_name
        task.automated = True
        task.replace_existing = True
        task.save = True
        tools.import_asset_tasks([task])

        path = "%s/%s" % (OUT_DIR, asset_name)
        texture = unreal.EditorAssetLibrary.load_asset(path)
        if texture is None:
            fail("%s imported nothing" % path)
            failures += 1
            continue

        # A UI icon must not be compressed as a colour map or it gets blocky at edges, and
        # it must not be mip-mapped: the bar draws it at one size.
        texture.set_editor_property("compression_settings",
                                    unreal.TextureCompressionSettings.TC_EDITOR_ICON)
        texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
        texture.set_editor_property("srgb", True)
        # Forced: save_asset does nothing for an asset the editor does not think is dirty,
        # which is the failure that makes a headless author look like it worked and leave
        # nothing on disk.
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

        credited.append((action_id, author, name))
        say("imported %s from %s/%s" % (asset_name, author, name))

    # The severity icons, into their own manifest section.
    notification_assets = {}
    for severity, (author, name) in sorted(NOTIFICATION_ICONS.items()):
        asset_name = "T_Icon_Note_%s" % severity
        if import_one(tools, staging, severity, author, name, asset_name):
            notification_assets[severity] = "%s/%s" % (OUT_DIR, asset_name)
            credited.append(("notification.%s" % severity, author, name))
        else:
            failures += 1

    os.makedirs(os.path.dirname(ATTRIBUTION), exist_ok=True)
    with io.open(ATTRIBUTION, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("# Icon attribution\n\n")
        handle.write("Icons from [game-icons.net](https://game-icons.net), used under\n")
        handle.write("[CC BY 3.0](https://creativecommons.org/licenses/by/3.0/).\n\n")
        handle.write("Generated by `Tools/Python/fetch_ui_icons.py` from the same table that\n")
        handle.write("drives the downloads, so it cannot drift from what was fetched.\n\n")
        handle.write("| Action | Icon | Author |\n|---|---|---|\n")
        for action_id, author, name in credited:
            handle.write("| `%s` | %s | %s |\n" % (action_id, name, author))
    say("wrote %s with %d credits" % (ATTRIBUTION, len(credited)))

    # ONE TABLE, TWO CONSUMERS. build_ui_style.py reads this rather than reversing asset
    # names back into action ids, which cannot be done for the two ids containing a space.
    # Deriving it twice would be the lists-that-must-agree bug in a new place.
    manifest_path = os.path.join(staging, "manifest.json")
    with io.open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump({
            "actions": {action_id: "%s/%s" % (OUT_DIR, asset_name_for(action_id))
                        for action_id, _, _ in credited
                        if not action_id.startswith("notification.")},
            "notifications": notification_assets,
        }, handle, indent=2, sort_keys=True)
    say("wrote %s with %d entries" % (manifest_path, len(credited)))

    say("ALL VERIFIED" if failures == 0 else "FAILED: %d icon(s)" % failures)
    say("DONE")


run()
