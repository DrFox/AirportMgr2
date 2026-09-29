"""Authors DA_Scenario_Default and names it the default scenario. Run headless, editor CLOSED:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

WHY AN ASSET (spec 2026-09-29-ops-batch3 §0): until this ran, Content/Ops did not exist, so
UAirportOpsSettings::ResolveDefaultScenario returned UScenario's CDO and every figure the game is
tuned by - the opening balance, the day length, what moves an airline's satisfaction - was a
constructor default nobody could see in the editor. A primary data asset is Unreal's equivalent of
a config file; a DataTable or raw JSON was rejected for one struct (tables and mods are what those
are for).

CREATED FROM THE CDO'S DEFAULTS and nothing else: the script sets no figure, so creating the asset
changes no number in play (AirportOps.Content.DefaultScenarioIsTheAsset checks two against the CDO).
The constructor defaults stay in C++ - they are what this asset is created from and what a test's
NewObject gets.

LOAD-OR-CREATE, never delete-and-recreate: once it exists the asset is the designer's, and re-running
this must not reset their edits to the constructor's figures. It only (re)writes the ini line and
re-saves.

THE INI LINE is written here too, so the asset and the setting naming it are made in one breath -
an asset nobody names is still the CDO in play, and a name with no asset is an Error at attach.
The PrimaryAssetTypesToScan entry for /Game/Ops already exists in DefaultGame.ini.
"""
import os

import unreal

OPS_DIR = "/Game/Ops"
NAME = "DA_Scenario_Default"
PACKAGE = "{}/{}".format(OPS_DIR, NAME)

SETTINGS_SECTION = "[/Script/AirportOps.AirportOpsSettings]"
SETTINGS_LINE = "DefaultScenario=Scenario:{}".format(NAME)


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def build_asset():
    # EXISTS FIRST: load_asset on a package that is not there logs an Error, which fails the
    # commandlet run for an asset being authored for the first time (build_airlines.py's lesson).
    asset = (unreal.EditorAssetLibrary.load_asset(PACKAGE)
             if unreal.EditorAssetLibrary.does_asset_exist(PACKAGE) else None)
    if asset is None:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        asset = tools.create_asset(asset_name=NAME, package_path=OPS_DIR,
                                   asset_class=unreal.Scenario, factory=None)
        if asset is None:
            # A primary data asset with no factory of its own: DataAssetFactory with the class set.
            factory = unreal.DataAssetFactory()
            factory.set_editor_property("data_asset_class", unreal.Scenario)
            asset = tools.create_asset(asset_name=NAME, package_path=OPS_DIR,
                                       asset_class=unreal.Scenario, factory=factory)
        if asset is None:
            fail("could not create {}".format(PACKAGE))
            return None
        say("created {}".format(PACKAGE))
    else:
        say("{} exists - kept as the designer left it".format(PACKAGE))

    # FORCED: save_asset does nothing for an asset the editor does not think is dirty - the
    # failure that makes a headless author look like it worked and leave nothing on disk.
    unreal.EditorAssetLibrary.save_asset(PACKAGE, only_if_is_dirty=False)
    return asset


def verify_on_disk(asset):
    # THE DISK, NOT THE RETURN: headless save and delete both report success while doing nothing
    # (memory: unreal-headless-delete-reports-success).
    path = os.path.join(unreal.Paths.project_content_dir(), "Ops", NAME + ".uasset")
    if not os.path.isfile(path):
        fail("{} is not on disk after the save".format(path))
        return False
    tuning = asset.get_editor_property("airline_satisfaction")
    # READ BACK OFF THE ASSET, so the line says what was saved rather than echoing a guess.
    say("{} on disk, {} bytes: opens at {}, starts {}:00, shortfall penalty {}".format(
        path, os.path.getsize(path), asset.get_editor_property("starting_balance"),
        asset.get_editor_property("start_hour"), tuning.get_editor_property("shortfall_penalty")))
    return True


def write_setting():
    ini = os.path.join(unreal.Paths.project_config_dir(), "DefaultGame.ini")
    with open(ini, "rb") as handle:
        raw = handle.read().decode("utf-8")
    newline = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.replace("\r\n", "\n").split("\n")

    if SETTINGS_SECTION in lines:
        start = lines.index(SETTINGS_SECTION) + 1
        end = start
        while end < len(lines) and not lines[end].startswith("["):
            end += 1
        body = [line for line in lines[start:end] if not line.startswith("DefaultScenario=")]
        # The section's own lines kept, the one this script owns replaced - idempotent.
        insert = [SETTINGS_LINE] + body
        lines[start:end] = insert
    else:
        while lines and lines[-1] == "":
            lines.pop()
        lines += ["", "; The scenario a new game opens with - written by Tools/Python/build_scenario.py.",
                  SETTINGS_SECTION, SETTINGS_LINE, ""]

    text = newline.join(lines)
    if not text.endswith(newline):
        text += newline
    with open(ini, "wb") as handle:
        handle.write(text.encode("utf-8"))
    say("{} names {} under {}".format(ini, SETTINGS_LINE, SETTINGS_SECTION))


asset = build_asset()
if asset is not None and verify_on_disk(asset):
    write_setting()
say("scenario done")
