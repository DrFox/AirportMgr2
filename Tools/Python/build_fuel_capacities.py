"""Gives every aircraft type its fuel tank, in litres. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

WHY (spec 2026-09-28-fuel-litres): a flight's fuel load is drawn at the offer as 50-90% of its
tank, pumped at the fuel vehicle's flow rate, in as many trips as the vehicle's own tank needs.
Published capacities, rounded to the litre; the paper A320/B738 types share their modelled
twins' figures.

IN PLACE, never delete-and-recreate - see build_pushback_needs.py for why. THE MARKER LINES ARE
NOT THE EVIDENCE: Airside.Content.FuelCapacitiesAuthored reads the assets back in a fresh
editor, and if it and this table ever disagree, believe the test.
"""
import unreal

CAPACITIES = {
    "DA_Aircraft_Plane1": 212.0,      # Cessna 172 Skyhawk
    "DA_Aircraft_Plane2": 1466.0,     # DHC-6 Twin Otter
    "DA_Aircraft_Plane3": 6526.0,     # Dash 8-Q400
    "DA_Aircraft_Plane4": 26020.0,    # 737-800
    "DA_Aircraft_Plane5": 2040.0,     # King Air 350i
    "DA_Aircraft_Plane6": 181283.0,   # 777-300ER
    "DA_Aircraft_Plane7": 454.0,      # PA-46-500TP Meridian
    "DA_Aircraft_Plane8": 320000.0,   # A380-800
    "DA_Aircraft_Plane9": 24210.0,    # A320-200
    "DA_Aircraft_Plane10": 1268.0,    # Cessna 208B Grand Caravan
    "DA_Aircraft_Plane11": 158987.0,  # A350-1000
    "DA_Aircraft_Plane12": 189.0,     # Piper PA-28-180 Cherokee
    "DA_Aircraft_Plane13": 43490.0,   # 757-300
    "DA_Aircraft_Plane14": 3020.0,    # Phenom 300
    "DA_Aircraft_Plane15": 348.0,     # Cirrus SR22
    "DA_Aircraft_Plane16": 734.0,     # Beechcraft Baron 58
    "DA_Aircraft_Plane17": 466.0,     # Piper PA-34 Seneca
    "DA_Aircraft_Plane18": 3220.0,    # Saab 340B
    "DA_Aircraft_A320": 24210.0,
    "DA_Aircraft_B738": 26020.0,
}


def run():
    done = 0
    for name, litres in sorted(CAPACITIES.items()):
        path = "/Game/Entities/%s" % name
        if not unreal.EditorAssetLibrary.does_asset_exist(path):
            unreal.log_error("MARKER: FAIL %s absent" % name)
            continue
        asset = unreal.EditorAssetLibrary.load_asset(path)
        asset.set_editor_property("fuel_capacity_litres", litres)
        if abs(asset.get_editor_property("fuel_capacity_litres") - litres) > 0.5:
            unreal.log_error("MARKER: FAIL %s did not take %s" % (name, litres))
            continue
        if not unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False):
            unreal.log_error("MARKER: FAIL %s save reported failure" % name)
            continue
        unreal.log_warning("MARKER: %s %.0f L" % (name, litres))
        done += 1
    unreal.log_warning("MARKER: %d of %d type(s) given a tank; now run Airside.Content.FuelCapacitiesAuthored"
                       % (done, len(CAPACITIES)))


run()
