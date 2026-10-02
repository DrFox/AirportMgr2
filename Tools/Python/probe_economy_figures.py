"""Prints the economy figures the game will actually play with, read from the ASSETS. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every line is prefixed MARKER: so it can be grepped out of the log.

WHY (2026-10-02): the scales of spec 2026-10-02-progression-and-fuel-supply §9 live in three places -
C++ constructor defaults (UScenario, UPricing), authored profile and entity rates
(build_cost_rates.py), and the scenario asset created from the CDO. A saved asset keeps any figure
that differed from the CDO when it was saved, so a changed constructor default proves nothing about
what is played. Pacing_model.py copies these figures; this reads them back from the assets it copied.
"""
import unreal


def say(msg):
    unreal.log_warning("MARKER: " + str(msg))


def main():
    scenario = unreal.EditorAssetLibrary.load_asset("/Game/Ops/DA_Scenario_Default")
    if scenario is None:
        say("FAIL no DA_Scenario_Default")
    else:
        say("scenario starting_balance %.0f" % scenario.get_editor_property("starting_balance"))
        for code, spec in scenario.get_editor_property("fuel_vehicles").items():
            say("scenario vehicle %s price %.0f upkeep %.0f" % (
                code, spec.get_editor_property("price"), spec.get_editor_property("upkeep_per_day")))
        for module, offer in scenario.get_editor_property("module_offers").items():
            say("scenario module %s price %.0f upkeep %.0f" % (
                module, offer.get_editor_property("price"), offer.get_editor_property("upkeep_per_day")))

    content = unreal.EditorAssetLibrary.load_asset("/Game/DA_AirsideContent")
    for prop in ("runway_profiles", "taxiway_profiles", "service_road_profiles"):
        for soft in content.get_editor_property(prop):
            profile = unreal.EditorAssetLibrary.load_asset(soft.get_path_name().split(".")[0])
            say("%s %s cost_per_metre %.1f upkeep %.3f" % (
                prop, profile.get_name(), profile.get_editor_property("cost_per_metre"),
                profile.get_editor_property("upkeep_per_metre_per_day")))

    for path in ("/Game/Entities/DA_Stand_CodeC", "/Game/Entities/DA_FuelDepot"):
        entity = unreal.EditorAssetLibrary.load_asset(path)
        say("%s placement_cost %.0f upkeep %.0f" % (
            path, entity.get_editor_property("placement_cost"), entity.get_editor_property("upkeep_per_day")))


main()
