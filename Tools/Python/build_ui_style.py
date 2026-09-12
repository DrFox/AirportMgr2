"""Authors DA_UIStyle and maps the icons into it. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

The COLOURS are the concept sheet's buildings row, so the UI is the colour the hangars will
be. They are set here rather than left to the CDO so they are visible in a diff and editable
in the Details panel - the asset is the thing a designer opens.

The ICON MAP is read from the manifest fetch_ui_icons.py writes, not derived from asset
names: two action ids contain a space and sanitise lossily, so the reverse mapping does not
exist. One table, two consumers.
"""
import io
import json
import os

import unreal

STYLE_PATH = "/Game/UI"
STYLE_NAME = "DA_UIStyle"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def srgb(hex_string):
    """An sRGB hex triple as the FLinearColor the engine would build from it.

    The standard sRGB electro-optical transfer function, computed rather than reflected:
    FLinearColor::FromSRGBColor is a static on a struct and Python reaches neither. Checked
    against Engine/Source/Runtime/Core/Public/Math/Color.h's sRGBToLinearTable, which this
    agrees with to 1e-9 - so the asset's colours and UUIStyle's CDO defaults are the SAME
    numbers rather than two transcriptions that could drift.
    """
    value = int(hex_string, 16)

    def to_linear(byte):
        channel = byte / 255.0
        if channel <= 0.04045:
            return channel / 12.92
        return ((channel + 0.055) / 1.055) ** 2.4

    return unreal.LinearColor(
        to_linear((value >> 16) & 255), to_linear((value >> 8) & 255),
        to_linear(value & 255), 1.0)


# Spec section 2.1, verbatim. SEMANTIC SLOTS: each is named for what it IS, never for where
# it appears, which is what makes a re-skin six edits instead of a hunt.
COLOURS = {
    "panel":      "4F5E6A",
    "panel_dark": "3E4A54",
    "button":     "5D6D7A",
    "accent":     "F4BA38",
    "text":       "E4E0D9",
    "text_muted": "9FB0BD",
}


def run():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (STYLE_PATH, STYLE_NAME)

    # Load-or-create rather than delete-and-recreate: deleting an asset something else
    # references breaks the reference instead of updating it.
    style = unreal.EditorAssetLibrary.load_asset(path)
    if style is None:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", unreal.UIStyle)
        style = tools.create_asset(STYLE_NAME, STYLE_PATH, unreal.UIStyle, factory)
    if style is None:
        fail("could not create %s" % path)
        say("DONE")
        return

    for prop, hex_string in COLOURS.items():
        style.set_editor_property(prop, srgb(hex_string))

    # READ THE MANIFEST fetch_ui_icons.py wrote. Deriving the id back from the asset name
    # cannot work: "tool.holding point" sanitises to T_Icon_tool_holding_point, and
    # underscores-to-dots would give "tool.holding.point", which matches no action.
    manifest_path = os.path.join(unreal.Paths.project_saved_dir(), "UIIcons", "manifest.json")
    if not os.path.isfile(manifest_path):
        fail("no %s - run fetch_ui_icons.py first" % manifest_path)
        say("DONE")
        return
    with io.open(manifest_path, encoding="utf-8") as handle:
        manifest = json.load(handle)

    icons = {}
    for action_id, asset_path in sorted(manifest.items()):
        texture = unreal.EditorAssetLibrary.load_asset(asset_path)
        if texture is None:
            fail("manifest names %s for '%s' but it did not load" % (asset_path, action_id))
            continue
        icons[unreal.Name(action_id)] = texture
    style.set_editor_property("icons_by_action_id", icons)
    say("mapped %d icons from the manifest" % len(icons))

    # Forced: save_asset does nothing for an asset the editor does not think is dirty.
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # Reload and read back: an asset edit that reports success and writes nothing is the
    # known failure mode, and a style that silently kept its defaults would look almost
    # right, which is worse than looking wrong.
    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    got = reloaded.get_editor_property("accent")
    want = srgb(COLOURS["accent"])
    if abs(got.r - want.r) > 1e-4 or abs(got.g - want.g) > 1e-4 or abs(got.b - want.b) > 1e-4:
        fail("accent read back as %r, expected %r" % (got, want))
    else:
        say("PASS accent survived the save")

    read_back = reloaded.get_editor_property("icons_by_action_id")
    if len(read_back) != len(icons):
        fail("icon map did not survive the save: %d of %d entries" % (len(read_back), len(icons)))
    else:
        say("PASS icon map survived the save, %d entries" % len(icons))

    # The two ids with a space are the reason the manifest exists; assert one arrived under
    # its real id rather than a sanitised one, because that is the failure this design
    # avoids and it would otherwise show up only as a blank button.
    if unreal.Name("tool.holding point") not in read_back:
        fail("'tool.holding point' is not in the map - the space-bearing ids did not survive")
    else:
        say("PASS 'tool.holding point' is keyed by its real id, space and all")

    say("ALL VERIFIED")
    say("DONE")


run()
