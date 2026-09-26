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
import os

import unreal

ENTITY_DIR = "/Game/Entities"

A320 = "/Game/Entities/DA_Aircraft_A320.DA_Aircraft_A320"
B738 = "/Game/Entities/DA_Aircraft_B738.DA_Aircraft_B738"
PIPER = "/Game/Entities/DA_Aircraft_Plane7.DA_Aircraft_Plane7"
PLANE1 = "/Game/Entities/DA_Aircraft_Plane1.DA_Aircraft_Plane1"
PLANE2 = "/Game/Entities/DA_Aircraft_Plane2.DA_Aircraft_Plane2"
PLANE5 = "/Game/Entities/DA_Aircraft_Plane5.DA_Aircraft_Plane5"
PLANE10 = "/Game/Entities/DA_Aircraft_Plane10.DA_Aircraft_Plane10"
PLANE12 = "/Game/Entities/DA_Aircraft_Plane12.DA_Aircraft_Plane12"
PLANE15 = "/Game/Entities/DA_Aircraft_Plane15.DA_Aircraft_Plane15"

# name -> (display name, fleet asset paths, offers per game day)
#
# OffersPerDay is the rate at full capability; UOpsRuntime sums it across airlines and asks
# for one offer that often. Six and four are a starting pair, not a balance decision - the
# numbers live on the asset precisely so they can be changed without a build.
AIRLINES = {
    # THE JET OPERATOR IS NOT SHIPPED YET, and its absence is the whole entry.
    #
    # 2026-09-12: removed, a content gap rather than a design change. Neither jet has a model
    # - UAircraftType::Mesh is unset on both - so each falls back to
    # UAirsideContent::AgentMesh and lands as a Piper Meridian. That was invisible while
    # every aircraft wore that one mesh, and became visible the moment a second model
    # existed: an A320 offered on a 30 m runway now arrives as a Meridian in front of the
    # player.
    #
    # NOT an empty fleet, which was the first attempt and which
    # AirportOps.Content.AirlineDefinition.TheAssetManagerScansThem rightly failed: "an
    # airline with an empty fleet offers nothing, for ever, and says nothing about it". The
    # same test forbids OffersPerDay of zero, and for the same reason. An operator that
    # cannot fly is an operator that is not in the game, and this dict IS the shipped set -
    # the prune below deletes any airline asset it does not name.
    #
    # PUT IT BACK with the model: restore the line below, and wire the jets' meshes in
    # build_aircraft_looks.py, whose verify() fails while two types share one.
    #
    #   "DA_Airline_Meridian": ("Meridian", [A320, B738], 6.0),

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
    # With the jets grounded this is the whole of the game's traffic, and the set still
    # carries the capability lesson: the Meridian's 13.11 m span fits an 18 m strip and the
    # Twin Otter's 19.75 m needs 23 m, so widening a runway still changes what arrives.
    #
    # 2026-09-19: THE CESSNA 172 JOINS, AND IT IS NOT A THIRD CAPABILITY TIER. Its 497 m
    # field length sits within 3% of the Meridian's 510, so anywhere one offers the other
    # does; what it adds is 11.00 m of span against 13.11, and variety in the inbox. That was
    # a deliberate choice over making it the new floor - the honest POH figure is the
    # 50 ft-obstacle distance, the same thing every other type publishes, and reading its
    # 293 m ground roll instead would have bought a new tier by measuring a different
    # quantity in a shared field.
    #
    # NOTE THAT THE 172 DOES NOT OUT-STOL THE TWIN OTTER, which is the intuition to resist:
    # plane2 needs 366 m to this type's 497. A DHC-6 is a STOL aeroplane and a 172 is a
    # trainer, so the short-field floor stays where it was.
    #
    # 2026-09-21: THE KING AIR JOINS, AND IT IS THE FIRST MEMBER OF THIS FLEET THAT CANNOT
    # USE GRASS. Every other type Cumbria flies is a grass type, so its whole ladder has been
    # made of LENGTH and WIDTH; DA_Aircraft_Plane5 asks for TARMAC and 1,006 m. That is a
    # capability the player buys by PAVING, and the smallest paved runway that earns
    # anything - a Q400 wants 1,402 m on the same surface, so paving pays before lengthening
    # does for the first time.
    #
    # ON THIS AIRLINE AND NOT A NEW ONE, deliberately. A second operator is a second thing to
    # balance and the entry above records what the last empty one cost; Cumbria already
    # spans a 172 to a Twin Otter, and a King Air is the same kind of operation. The day the
    # jets get models, the Meridian line above comes back and the split is jets against
    # everything else.
    #
    # 2026-09-25: THE CARAVAN JOINS, AND IT IS THE GRASS HALF OF CODE B. Until now Cumbria's
    # only grass types were under 15 m (the 172, the Meridian) or the Twin Otter at 19.75 m;
    # the Caravan's 15.88 m sits between, on a 740 m grass field. What it adds is a Code B
    # type that widening alone admits, where the King Air needs paving too.
    #
    # 2026-09-25: THE CHEROKEE JOINS, AND LIKE THE 172 IT IS VARIETY, NOT A TIER. 500 m of
    # grass, within 1% of the 172's 497; what it adds is the narrowest span in the game.
    #
    # 2026-09-26: THE SR22 JOINS, VARIETY AGAIN. 570 m of grass - more than the 172 or the
    # Cherokee, because it lands faster, and still under the Caravan's 740.
    "DA_Airline_Cumbria": ("Cumbria Air",
                           [PIPER, PLANE1, PLANE2, PLANE5, PLANE10, PLANE12, PLANE15], 4.0),
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


def prune():
    """Delete airline assets this script does not name.

    THE DICT IS THE SHIPPED SET, so an airline it no longer lists must not survive on disk -
    a stale DA_Airline_Meridian with an empty fleet would keep failing
    TheAssetManagerScansThem, and one with its old fleet would keep offering jets that land
    as Meridians. Either way the file and the script would be two different claims about what
    ships.

    Safe here where it would not be for aircraft types: nothing references an airline. They
    are primary assets found by the asset manager - see the PrimaryAssetTypesToScan line in
    DefaultGame.ini - rather than pointed at by other content.
    """
    for path in unreal.EditorAssetLibrary.list_assets(ENTITY_DIR, recursive=False):
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if not isinstance(asset, unreal.AirlineDefinition):
            continue
        name = path.split("/")[-1].split(".")[0]
        if name in AIRLINES:
            continue

        # delete_LOADED_asset, and the RETURN CHECKED. list_assets above has already loaded
        # this one, and delete_asset refuses a loaded package - it returned false while this
        # logged "pruned", so the airline survived with an empty fleet and the suite kept
        # failing on an asset the script claimed to have removed. Honour the return of
        # anything that reports success.
        deleted = unreal.EditorAssetLibrary.delete_loaded_asset(asset)

        # AND THEN CHECK THE DISK, because the return is not evidence. delete_asset returned
        # false while this logged success; delete_loaded_asset returns TRUE and leaves the
        # .uasset exactly where it was, which a fresh session then loads again with its empty
        # fleet. Both were found by looking rather than by believing.
        package = os.path.join(
            unreal.Paths.project_content_dir(),
            path.split(".")[0].replace("/Game/", "") + ".uasset")
        if os.path.isfile(package):
            try:
                os.remove(package)
                unreal.log("MARKER: pruned {} by removing {} - delete_loaded_asset returned "
                           "{} and left the file".format(name, os.path.basename(package), deleted))
            except OSError as exc:
                unreal.log_error("MARKER: FAIL {} is still on disk and still ships: {}"
                                 .format(name, exc))
        else:
            unreal.log("MARKER: pruned {} - not in the shipped set".format(name))


build()
prune()
unreal.log("MARKER: airlines done")
