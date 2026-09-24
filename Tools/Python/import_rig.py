"""Imports the articulated fuel rig - truckCab1 (tractor) and tankTrailer1 (trailer) - as TWO
skeletal meshes. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

BUILT ON airside_import.py'S Spec/import_one MECHANISM, not a fourth hand-copy of
import_fueltruck.py's ~85% duplicate script. import_fueltruck.py predates the extraction and
its own header says "the existing three are left alone... if one of them is next re-imported
and the divergence bites, that is the moment to fold it in" - this is a NEW model, so it is the
moment to use the mechanism rather than grow a fourth copy.

TWO ASSETS, TWO SUBFOLDERS UNDER Content/Vehicles/Rig/, not one shared folder. clear_previous
wipes its WHOLE mesh_dir on every run (airside_import.py's own comment: "safe here and nowhere
else... everything under mesh_dir is this script's own output"). Importing both meshes into
one shared /Game/Vehicles/Rig/ would make the second import's clear_previous delete the first
one's mesh, Skeleton and PhysicsAsset - so each asset gets its own mesh_dir
(.../Rig/TruckCab1, .../Rig/TankTrailer1), matching Content/Vehicles/FuelTruck1's own
one-folder-per-asset shape. Both still land "under Content/Vehicles/Rig/", which is what the
task asked for.

ORIGINS AND SOCKETS (build_export.py's own header, truckCab1/scripts/build_export.py):
  truckCab1    origin = rear-axle ground point (already the Blender origin). Bone
               'fifth_wheel' sits at the kingpin coupling point.
  tankTrailer1 origin = TANDEM AXLE CENTRE ground point - the midpoint of axle 1 and axle 2,
               not either axle alone. Bone 'kingpin' sits at the coupling point; attaching is
               "snap trailer kingpin onto tractor fifth_wheel".

FIFTH_WHEEL AND KINGPIN ARE SOCKETS, NOT DRIVEN BONES - see build_rig_anim.py's own header for
why 'fifth_wheel' is a trap for airside_anim.bone_plan's substring rule ("wheel" matches inside
"fifth_wheel").
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402
from airside_import import Spec, fail, say  # noqa: E402

MODELS_ROOT = r"C:\repos\AirportMgr2Models\truckCab1\export"

SPECS = [
    Spec(
        key="truckCab1",
        source=os.path.join(MODELS_ROOT, "truckCab1.glb"),
        mesh_dir="/Game/Vehicles/Rig/TruckCab1",
        skel_name="SK_TruckCab1",
        # Mesh node stems (asset-prefixed, from build_export.py's applied_copy), not joint
        # names - see airside_import.Spec's own docstring on why the two must not be confused.
        front_nodes=("truckCab1_wheel_FL", "truckCab1_wheel_FR"),
        rear_nodes=("truckCab1_wheel_RL", "truckCab1_wheel_RR"),
        front_label="front wheel",
        rear_label="rear wheel",
        # The rear axle IS the Blender origin (build_export.py's header), so the rear wheel
        # centre should land within 10 uu of X=0 after import.
        origin_on="rear",
        note="the tractor unit. fifth_wheel is the kingpin SOCKET a trailer couples to - not "
             "a driven bone; see build_rig_anim.py.",
    ),
    Spec(
        key="tankTrailer1",
        source=os.path.join(MODELS_ROOT, "tankTrailer1.glb"),
        mesh_dir="/Game/Vehicles/Rig/TankTrailer1",
        skel_name="SK_TankTrailer1",
        front_nodes=("tankTrailer1_trailer_wheel_1L", "tankTrailer1_trailer_wheel_1R"),
        rear_nodes=("tankTrailer1_trailer_wheel_2L", "tankTrailer1_trailer_wheel_2R"),
        front_label="axle 1",
        rear_label="axle 2",
        # NEITHER 'front' NOR 'rear': the origin is the TANDEM CENTRE, the midpoint of axle 1
        # and axle 2, which Spec.origin_on cannot express (it checks one axle against 0, not a
        # midpoint). Skipped here and checked by report_tandem_origin below instead.
        origin_on=None,
        note="the trailer. kingpin is the coupling SOCKET a tractor's fifth_wheel takes - not "
             "a driven bone. Origin is the TANDEM AXLE CENTRE (axle 1 / axle 2 midpoint), "
             "checked separately below since Spec.origin_on only knows a single axle.",
    ),
]


def report_tandem_origin(spec, doc):
    """tankTrailer1's own origin convention: the midpoint of axle 1 and axle 2 is at X=0.

    Spec.origin_on checks a SINGLE axle against the origin (see Spec's own docstring); this
    rig's trailer origin is a midpoint, which is a different question and gets its own check
    rather than stretching Spec to answer something it was not shaped for.
    """
    front, rear = airside_import.axle_centres_uu(doc, spec.front_nodes, spec.rear_nodes)
    if front is None or rear is None:
        fail("%s: could not measure both axles for the tandem-centre check" % spec.key)
        return
    mid = (front + rear) * 0.5
    if abs(mid) > 10.0:
        fail("%s: the tandem axle centre is %.1f uu from the origin, not on it - the model's "
             "convention (build_export.py) is trailer_axle_centre at X=0" % (spec.key, mid))
    else:
        say("PASS %s: the tandem axle centre (axle1 %.1f, axle2 %.1f -> mid %.1f) is the "
            "origin, as the model's convention states" % (spec.key, front, rear, mid))


def main():
    say("=" * 78)
    say("importing the articulated rig: truckCab1 (tractor) + tankTrailer1 (trailer)")
    ok = True
    for spec in SPECS:
        result = airside_import.import_one(spec)
        ok = result and ok
        if result:
            doc = airside_import.read_gltf(spec.source)
            if spec.key == "tankTrailer1" and doc is not None:
                report_tandem_origin(spec, doc)

    say("-" * 70)
    say("rig import: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
