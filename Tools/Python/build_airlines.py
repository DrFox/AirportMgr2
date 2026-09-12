"""Authors the starting UAirlineDefinition assets. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

TWO AIRLINES, DELIBERATELY UNEQUAL. One narrow-body operator whose fleet needs a real
runway, and one light operator that can use almost anything. That pair is what makes the
capability filter visible at all: on a short strip only the light airline offers, and the
first long runway the player builds changes what appears in the inbox. A single airline
would make the filter untestable by eye.

The fleet references are the SAME UAircraftType assets the arrival path already uses, so
the offer and the aeroplane that turns up cannot describe different aircraft.
"""
import unreal

ENTITY_DIR = "/Game/Entities"

A320 = "/Game/Entities/DA_Aircraft_A320.DA_Aircraft_A320"
B738 = "/Game/Entities/DA_Aircraft_B738.DA_Aircraft_B738"
PIPER = "/Game/Entities/DA_Aircraft_Piper.DA_Aircraft_Piper"
PLANE2 = "/Game/Entities/DA_Aircraft_Plane2.DA_Aircraft_Plane2"

# name -> (display name, fleet asset paths, offers per game day)
#
# OffersPerDay is the rate at full capability; UOpsRuntime sums it across airlines and asks
# for one offer that often. Six and four are a starting pair, not a balance decision - the
# numbers live on the asset precisely so they can be changed without a build.
AIRLINES = {
    # The jets. On a 15 m GA strip NOTHING of theirs is offered, and that is the design
    # working: the inbox says so, and a wider runway is what unlocks them.
    "DA_Airline_Meridian": ("Meridian", [A320, B738], 6.0),

    # The light operator, whose Meridian fits a 15 m strip - so the starting field has
    # traffic from the first minute. 2026-09-11: this used to fly an A320 too, which meant
    # the starter map could offer nothing at all and every Accept was greyed out.
    #
    # 2026-09-12: the Twin Otter joins it, and the pair is deliberately unequal in the same
    # way the two airlines are. The Meridian's 13.11 m span fits a 15 m strip; the Twin
    # Otter's 19.75 m does not, so on the starting field Cumbria offers only the Meridian
    # and the Twin Otter appears once the player widens a runway. It needs far LESS length
    # than the Meridian (366 m against 510) while needing more WIDTH, which is the first
    # time those two requirements pull in opposite directions - and the clearest thing in
    # the game so far that says the two are separate decisions.
    "DA_Airline_Cumbria": ("Cumbria Air", [PIPER, PLANE2], 4.0),
}


def load_type(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        unreal.log_warning("MARKER: missing aircraft type {}".format(path))
    return asset


def build():
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    for name, (display, fleet_paths, per_day) in AIRLINES.items():
        package = "{}/{}".format(ENTITY_DIR, name)

        # Load-or-create rather than delete-and-recreate: deleting an asset something else
        # references breaks the reference rather than updating it.
        asset = unreal.EditorAssetLibrary.load_asset(package)
        if asset is None:
            asset = tools.create_asset(
                asset_name=name,
                package_path=ENTITY_DIR,
                asset_class=unreal.AirlineDefinition,
                factory=None,
            )
        if asset is None:
            unreal.log_warning("MARKER: could not create {}".format(package))
            continue

        fleet = [t for t in (load_type(p) for p in fleet_paths) if t is not None]

        asset.set_editor_property("display_name", unreal.Text(display))
        asset.set_editor_property("fleet", fleet)
        asset.set_editor_property("offers_per_day", per_day)
        asset.set_editor_property("offer_weight", 1.0)

        # save_asset does nothing for an asset the editor does not think is dirty, so the
        # save is forced - this is the failure that makes a headless author look like it
        # worked and leave nothing on disk.
        unreal.EditorAssetLibrary.save_asset(package, only_if_is_dirty=False)
        unreal.log("MARKER: {} '{}' with {} type(s), {}/day".format(
            name, display, len(fleet), per_day))


build()
unreal.log("MARKER: airlines done")
