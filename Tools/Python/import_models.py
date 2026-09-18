"""Imports the models that have a current export and no assets yet. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. A commandlet writing .uasset files the open editor has loaded
fails with a sharing violation, and some of the calls here report success while writing
nothing. Every result line is prefixed MARKER: so it can be grepped out of
Saved/Logs/AirportMgr.log, because print() goes to the log rather than stdout.

THIS FILE IS THE TABLE AND NOTHING ELSE. The mechanism - the Interchange pipeline-on-disk
dance, the rename-and-move-up, the bounds/rig/material checks - is in airside_import.py, so
that adding a fifth model is a Spec and not a fourth copy of a 450-line script. Read that
file's header for why the three existing import_*.py scripts are left as they are.

WHAT IS DELIBERATELY NOT HERE:

- plane1. Raw Tripo output: no skin, two materials, one of them still called
  tripo_mat_badc5a69. It would import, and would look like what it is beside plane2 and
  plane3. Superseded rather than pending.
- accessories/chainlink. Two fence POSTS are exported and no panel, so nothing here can
  assemble a fence line. Worth importing when the panel lands.
- baggageCart1, FuelDepot1, concepts. Concept art only; no export exists.
- boeing737-900/737.fbx. 10 KB, dated 2024, beside a .blend that is clearly the real work.
  A stub, not a model.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript. The commandlet
# executes the file without adding its folder the way `python foo.py` would, so the import
# below raises ModuleNotFoundError and the run dies before a single MARKER: line - which
# reads as "the commandlet did nothing" rather than as a missing path. Put it on first.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import Spec, import_one, rebuild_fleet_materials, say  # noqa: E402


MODELS = r"C:\repos\AirportMgr2Models"


# THE WHEEL NODES ARE MESH NAMES, NOT JOINT NAMES, and plane3 is why the distinction is
# spelled out in Spec: its rig declares joints called nosewheel/wheel_L/wheel_R exactly like
# plane2's, while the geometry those joints drive is called gearFront/gearRear_L/gearRear_R
# with Blender's duplicate suffix on the end. Matching on joint names would find nothing and
# report "cannot tell which way it faces" for a model that is perfectly correct.
VEHICLE_FRONT = ["wheel_FL", "wheel_FR"]
VEHICLE_REAR = ["wheel_RL", "wheel_RR"]


SPECS = [
    Spec(
        key="plane3",
        source=MODELS + r"\plane3\export\plane3.glb",
        mesh_dir="/Game/Aircraft/Plane3",
        skel_name="SK_Plane3",
        front_nodes=["gearFront"],
        rear_nodes=["gearRear_L", "gearRear_R"],
        front_label="nose gear",
        rear_label="main gear",
        # NOSE GEAR, which is UAircraftType's documented local space and what plane2 follows.
        #
        # IT WAS THE MAIN GEAR UNTIL 2026-09-18 and the export was changed rather than this
        # spec, because the deviation bought nothing: the two things a main-gear origin was
        # argued for - the taxi pivot and the flare pitch - are already declared fields of
        # their own (FAirframe::PitchPivotX, BodyCentreX), and a main-gear origin would have
        # moved the offset into SteerAxleX instead of removing it, parking the aircraft 14 m
        # off every stand mark until somebody noticed. plane3/scripts/build_export.py carries
        # the full argument at UE_ORIGIN.
        #
        # Measured off the re-export: nose gear at 0.0 uu, mains at -1382.0 uu. The main
        # AXLE is at -1404.5 uu - the mesh bbox centre above includes the strut - which is
        # the figure FAirframe::FixedAxleX wants when this type is authored.
        origin_on="front",
        note="de Havilland Dash 8-Q400. 32.5 m long, 28.2 m span - the real aircraft's own "
             "figures. Origin on the NOSE gear, 14.045 m forward of the mains.",
    ),
    Spec(
        key="tug1",
        source=MODELS + r"\tug1\export\tug1.glb",
        mesh_dir="/Game/Vehicles/Tug1",
        skel_name="SK_Tug1",
        front_nodes=VEHICLE_FRONT,
        rear_nodes=VEHICLE_REAR,
        front_label="front axle",
        rear_label="rear axle",
        # Rear axle, the convention fueltruck1's README states and for its reason: that is
        # what a front-steered vehicle pivots about. Measured at 0.0 uu before importing.
        origin_on="rear",
        note="Goldhofer BISON D 620 pushback tractor - TOWBAR, not towbarless; the rear "
             "channel is a walkway, not a nose-gear well. Four steer bones, so all four "
             "wheels turn.",
    ),
    Spec(
        key="utility1",
        source=MODELS + r"\utility1\export\utility1.glb",
        mesh_dir="/Game/Vehicles/Utility1",
        skel_name="SK_Utility1",
        front_nodes=VEHICLE_FRONT,
        rear_nodes=VEHICLE_REAR,
        front_label="front axle",
        rear_label="rear axle",
        origin_on="rear",
        note="Airside utility vehicle. 3.0 m long. Front wheels steer; one beacon bone.",
    ),
    Spec(
        key="gpu1",
        source=MODELS + r"\gpu1\export\gpu1.glb",
        mesh_dir="/Game/Vehicles/GPU1",
        skel_name="SK_GPU1",
        front_nodes=VEHICLE_FRONT,
        rear_nodes=VEHICLE_REAR,
        front_label="front axle",
        rear_label="rear axle",
        origin_on="rear",
        # TOWED, not driven, and the rig says so: drawbar_steer and drawbar_lift are the two
        # bones that matter and there are no steer_ bones on the wheels at all. It lives under
        # Vehicles/ because that is where the wheeled ground equipment is, not because
        # anything drives it.
        note="Ground power unit - a TOWED trailer, not a vehicle. The rig's drawbar_steer "
             "and drawbar_lift are its moving parts; the wheels only roll.",
    ),
]


def main():
    results = []
    for spec in SPECS:
        results.append((spec.key, import_one(spec)))

    # A ROLL-UP, because four imports produce several hundred log lines and "did it work" is
    # otherwise a question answered by scrolling. Named results, not a count: a count tells
    # you something failed and not which, which is the same mistake the automation runner
    # made before Run-AirsideTests.ps1 started diffing started against completed.
    say("=" * 78)
    for key, ok in results:
        say("%-10s %s" % (key, "PASS" if ok else "FAIL"))
    bad = [key for key, ok in results if not ok]
    if bad:
        unreal.log_error("MARKER: FAIL %d of %d model(s) had failing checks: %s"
                         % (len(bad), len(results), ", ".join(bad)))
    else:
        say("all %d model(s) imported and passed every check" % len(results))
    # An import regenerates per-asset materials and reassigns every slot, silently
    # undoing the shared set. Rebuilt here so no import can leave it undone.
    rebuild_fleet_materials()
    say("DONE")


main()
