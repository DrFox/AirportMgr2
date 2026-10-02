"""Authors the money figures onto the shipped road profiles and entity definitions. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

ONE RATE PER SQUARE METRE, AND THE PER-METRE FIGURE IS DERIVED FROM IT. URoadProfile carries
CostPerMetre because that is what a build quote needs (BuildCost::ForSegment multiplies it by
a length), but authoring five taxiway widths by hand at five different per-metre figures is
five chances to break the one property that actually matters: a wider taxiway costs more per
metre than a narrower one, in proportion to how much pavement it is. So the rate per square
metre is the authored number and the per-metre figure is width times rate.

THE ORDERING IS THE PART TO DEFEND when these are tuned, and it is stated here rather than
left to be inferred: runway pavement is the thickest and dearest per square metre, apron sits
between taxiway and runway, and a service road is the cheapest thing that can be laid. The
magnitudes are a first pass and unplayed - see the spec's §7.

UPKEEP IS A FIXED FRACTION OF THE BUILD COST, for the same reason: a per-day figure typed
independently at each asset would drift away from what the thing cost to build, and the
relationship - owning it costs about a thousandth of building it, every day - is the thing
being authored, not the number.
"""
import unreal

# Rate per SQUARE METRE. See the module docstring on why this, and not the per-metre figure,
# is what is authored.
#
# x0.4 ON 2026-10-02 (were 13 / 25 / 5, stand 40,000, depot 120,000): the pacing model put
# building at ~5x what earning paid for against a paved runway at ~2 real hours; landing fees went
# x5 and builds x0.4 between them. Spec 2026-10-02-progression-and-fuel-supply section 9.
BUILD_SCALE = 0.4
TAXIWAY_RATE = 13.0 * BUILD_SCALE
RUNWAY_RATE = 25.0 * BUILD_SCALE
SERVICE_ROAD_RATE = 5.0 * BUILD_SCALE

# A day of owning it, as a fraction of what it cost to lay.
UPKEEP_FRACTION = 0.001

UU_PER_METRE = 100.0

# WHICH PROFILES ARE PRICED IS THE CONTENT SET'S, not a list here (2026-09-28). This was a
# typed list of eleven asset paths, and when the road tiers went from one to three
# (build_road_profiles.py, 2026-09-24) the Standard and Wide tiers were never added to it -
# so both shipped free while every test stayed green. The content set's lists are what the
# tools offer a player, so they are what must carry a price.
# ENFORCED BY: Airside.Content.EveryProfileIsPriced.
CONTENT_SET = "/Game/DA_AirsideContent"
PROFILE_LISTS = [
    ("taxiway_profiles", TAXIWAY_RATE),
    ("runway_profiles", RUNWAY_RATE),
    ("service_road_profiles", SERVICE_ROAD_RATE),
]


def shipped_profiles():
    """(path, rate per square metre) for every profile the content set offers."""
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_SET)
    if content is None:
        fail("%s not found - nothing to price" % CONTENT_SET)
        return []
    out = []
    for prop, rate in PROFILE_LISTS:
        for soft in content.get_editor_property(prop):
            # "/Game/X.X" - load_asset takes the package path, before the dot.
            out.append((soft.get_path_name().split(".")[0], rate))
    return out

# What placing one costs. A stand and a depot are single objects rather than lengths, so
# these are authored directly - there is no width to derive them from.
ENTITIES = [
    ("/Game/Entities/DA_Stand_CodeC", 40000.0 * BUILD_SCALE),
    ("/Game/Entities/DA_FuelDepot", 120000.0 * BUILD_SCALE),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def total_width_uu(profile):
    """The profile's cross-section, summed from its bands.

    READ FROM THE BANDS rather than from a table here, so a profile whose width is retuned in
    build_road_profiles.py is repriced by re-running this - two tables of widths would be two
    things to keep in agreement, and the second would be the one nobody updated.
    """
    total = 0.0
    for band in profile.get_editor_property("bands"):
        total += band.get_editor_property("width")
    return total


def main():
    priced = 0

    for path, rate_per_square_metre in shipped_profiles():
        profile = unreal.EditorAssetLibrary.load_asset(path)
        if profile is None:
            fail("%s does not exist - author the profiles first" % path)
            continue

        width_metres = total_width_uu(profile) / UU_PER_METRE
        if width_metres <= 0.0:
            fail("%s has no bands, so it has no width to price" % path)
            continue

        cost_per_metre = width_metres * rate_per_square_metre
        profile.set_editor_property("cost_per_metre", cost_per_metre)
        profile.set_editor_property("upkeep_per_metre_per_day", cost_per_metre * UPKEEP_FRACTION)

        # FORCED. A plain save_asset writes nothing for an asset the editor does not think is
        # dirty, and set_editor_property does not always mark it - the failure is silent, and
        # the figures would simply not be in the .uasset.
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        say("%s: %.1f m wide, %.0f per metre, %.2f per metre per day"
            % (path, width_metres, cost_per_metre, cost_per_metre * UPKEEP_FRACTION))
        priced += 1

    for path, placement_cost in ENTITIES:
        definition = unreal.EditorAssetLibrary.load_asset(path)
        if definition is None:
            fail("%s does not exist" % path)
            continue

        definition.set_editor_property("placement_cost", placement_cost)
        definition.set_editor_property("upkeep_per_day", placement_cost * UPKEEP_FRACTION)
        unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
        say("%s: %.0f to place, %.0f per day"
            % (path, placement_cost, placement_cost * UPKEEP_FRACTION))
        priced += 1

    say("priced %d asset(s)" % priced)


main()
