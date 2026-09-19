"""Imports the models that have a current export and no assets yet. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. A commandlet writing .uasset files the open editor has loaded
fails with a sharing violation, and some of the calls here report success while writing
nothing. Every result line is prefixed MARKER: so it can be grepped out of
Saved/Logs/AirportMgr.log, because print() goes to the log rather than stdout.

A MODEL ALREADY IN CONTENT IS SKIPPED, which is what the first line above has always
claimed and what nothing enforced until 2026-09-19. See already_imported() for what a
re-run used to do instead.

THIS FILE IS THE TABLE AND NOTHING ELSE. The mechanism - the Interchange pipeline-on-disk
dance, the rename-and-move-up, the bounds/rig/material checks - is in airside_import.py, so
that adding a fifth model is a Spec and not a fourth copy of a 450-line script. Read that
file's header for why the three existing import_*.py scripts are left as they are.

WHAT IS DELIBERATELY NOT HERE:

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


# THE WHEEL NODES ARE MESH NAMES, NOT JOINT NAMES, and Spec spells the distinction out
# because the two namespaces are independent - a .glb may hold a joint and a mesh node of the
# same name, and for plane2 and plane3 it does. Matching on joints would find no geometry at
# all and report "cannot tell which way it faces" for a model that is perfectly correct.
#
# plane3 USED TO BE THE AWKWARD CASE: joints nosewheel/wheel_L/wheel_R against geometry called
# gearFront/gearRear_L/gearRear_R, because each leg was ONE mesh holding both a rolling wheel
# and a static strut. On 2026-09-19 the gear was separated into three wheels and three legs -
# the leg halves kept gearFront/gearRear_* and the WHEELS took their bones' names - because
# skinning a combined mesh per vertex was rotating the legs in engine. So these rows now name
# the wheels, which is what an AXLE measurement wants: a leg's centre is not an axle.
VEHICLE_FRONT = ["wheel_FL", "wheel_FR"]
VEHICLE_REAR = ["wheel_RL", "wheel_RR"]


SPECS = [
    Spec(
        key="plane1",
        source=MODELS + r"\plane1\export\plane1.glb",
        mesh_dir="/Game/Aircraft/Plane1",
        skel_name="SK_Plane1",
        # THE MESH NAMES MATCH THE FLEET SINCE 2026-09-19, and this row used to be the
        # exception. The nose tyre was `wheel_front_1` where plane2, plane3 and plane4 all
        # call it `nosewheel` after its bone, and the FORK was `wheel_front` - a name in the
        # tyre's namespace for a part that is not a tyre, whose box centre sits 8 cm behind
        # the axle. Naming that one here would have published a 1.71 m wheelbase for an
        # aircraft whose wheelbase is 1.63 m. The export now says nosewheel / nosegear /
        # maingear, so no reader has to know the difference.
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
        front_label="nose gear",
        rear_label="main gear",
        origin_on="front",
        # THE RIG WAS FIXED AT THE EXPORT, THREE TIMES, ON 2026-09-19, and the alternative
        # each time was a special case in this project that would have outlived the model:
        #
        #   * ONE `wheel` bone drove BOTH mains, and it sat at y +137.0 - the right wheel's
        #     outer FACE, not either axle. Rolling it would have swung the left wheel through
        #     a 2.54 m arc. Split into wheel_L/wheel_R on the axles at y -+127.0, which is
        #     what plane2, plane3 and plane4 have and what build_plane1_type.py reads to get
        #     the track. With one bone there is no track to measure.
        #   * `nosewheel` sat at y +12.0 against a tyre centred on y 0.0 and only -+7.4 wide,
        #     so the steering axis was outside the wheel it steers.
        #   * The wing and the tailplane were inside `fuselage`, leaving nothing for the
        #     footprint's wingspan and tailplane_span to measure. Now separate `wing`,
        #     `stabiliser` and `fin` objects.
        #
        # Same call plane3's origin got: change the export, not the reader. A fourth round
        # the same day added the livery and the flat base colours - see reimport_plane1.py,
        # which is what applies that one, because by then the asset existed and this script
        # is a FIRST-import tool.
        note="Cessna 172S Skyhawk. 8.233 m long, 11.00 m span, 2.72 m to the fin tip - the "
             "real aircraft's own figures to within 5 cm. Origin on the NOSE gear; the main "
             "axle is at -163.0 uu, which is the figure FAirframe::FixedAxleX wants when "
             "this type is authored. Six joints, and the gear is FIXED - a 172 has nothing "
             "to retract, so this is the first modelled type since plane2 with no gear "
             "cycle at all.",
    ),
    Spec(
        key="plane3",
        source=MODELS + r"\plane3\export\plane3.glb",
        mesh_dir="/Game/Aircraft/Plane3",
        skel_name="SK_Plane3",
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
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
        key="plane4",
        source=MODELS + r"\plane4\export\plane4.glb",
        mesh_dir="/Game/Aircraft/Plane4",
        skel_name="SK_Plane4",
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
        front_label="nose gear",
        rear_label="main gear",
        # THE SAME THREE NODE NAMES AS plane3 AND THAT IS NOT A COINCIDENCE.
        # plane4/scripts/build_export.py records renaming mainwheel_L/R -> wheel_L/wheel_R on
        # 2026-09-19 "to match their bones, which is plane2's and plane3's convention"; the
        # LEGS keep nosegear/maingear_L/maingear_R, because a leg's centre is not an axle and
        # these two lists want the WHEEL meshes. On THIS export the two happen to agree to
        # the millimetre - both boxes centre on x 0.000 and -15.600 - so naming the legs
        # would measure correctly today and silently stop the day a strut is raked or a
        # fairing joins the leg mesh. Measured off the thing the number is about.
        origin_on="front",
        # NOSE GEAR, the convention UAircraftType documents and plane3's export was changed
        # to meet. build_export.py's UE_ORIGIN is (0, -15.66, 0) in Blender space - the
        # nosewheel_steer head, i.e. the nose-gear contact patch - so this is declared at the
        # source rather than corrected here.
        note="Boeing 737-800W. 39.3 m long, 35.8 m span over the winglets, 12.6 m to the fin "
             "tip - the real aircraft's own figures. Origin on the NOSE gear; the main axle "
             "is at -1560.0 uu, which is the figure FAirframe::FixedAxleX wants when this "
             "type is authored. Twelve joints: the gear retracts and the nose bay doors "
             "hinge, but nothing in the engine drives either yet.",
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


def content_file(package):
    """The .uasset a /Game/ package path names, as an absolute file on disk."""
    root = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())
    return os.path.join(root, package[len("/Game/"):].replace("/", os.sep) + ".uasset")


def already_imported(spec):
    """True when this model's mesh is already in Content, so this run must leave it alone.

    THE FIRST LINE OF THIS FILE HAS ALWAYS SAID "a current export AND NO ASSETS YET", and
    until now nothing enforced it. import_one() begins by CLEARING its target folder, and
    clear_previous() cannot clear a folder that anything outside it references - M_ModelYard
    places every one of these meshes. So on 2026-09-18 a re-run half-imported all four rows
    at once: the delete failed, the registry cheerfully reported "cleared 3 asset(s)" while
    the .uasset sat on disk, the rename then collided, and what was left was the OLD mesh
    under the real name plus a stray under SkeletalMeshes/ with the Skeleton and PhysicsAsset
    renamed over the originals. Nothing about that reads as a failed delete.

    Re-importing is therefore the separate, deliberate act reimport_plane2.py and
    reimport_plane3.py exist for: UInterchangeManager.reimport_asset updates the UObject in
    place, so the package path, GUID and every reference survive by construction.

    ASKED OF THE FILE AS WELL AS THE REGISTRY, and in that order of trust. The registry is
    the thing that lied about the delete; answering this question from it alone would consult
    the same source that got the last one wrong.
    """
    package = "%s/%s" % (spec.mesh_dir, spec.skel_name)
    if os.path.isfile(content_file(package)):
        return True
    return unreal.EditorAssetLibrary.does_asset_exist(package)


def main():
    results = []
    for spec in SPECS:
        if already_imported(spec):
            # A SKIP IS NOT A PASS AND NOT A FAIL, and the roll-up says so in its own word.
            # Folding it into PASS would make "all 5 model(s) imported" true of a run that
            # imported one.
            say("%s: SKIP - %s/%s is already in Content. Re-import through a reimport_*.py, "
                "which updates the asset in place; a clear-and-import from here would strand "
                "every reference M_ModelYard holds."
                % (spec.key, spec.mesh_dir, spec.skel_name))
            results.append((spec.key, None))
            continue
        results.append((spec.key, import_one(spec)))

    # A ROLL-UP, because four imports produce several hundred log lines and "did it work" is
    # otherwise a question answered by scrolling. Named results, not a count: a count tells
    # you something failed and not which, which is the same mistake the automation runner
    # made before Run-AirsideTests.ps1 started diffing started against completed.
    say("=" * 78)
    for key, ok in results:
        say("%-10s %s" % (key, "SKIP" if ok is None else "PASS" if ok else "FAIL"))
    bad = [key for key, ok in results if ok is False]
    ran = [key for key, ok in results if ok is not None]
    if bad:
        unreal.log_error("MARKER: FAIL %d of %d model(s) had failing checks: %s"
                         % (len(bad), len(ran), ", ".join(bad)))
    elif ran:
        say("all %d model(s) imported and passed every check (%d already in Content)"
            % (len(ran), len(results) - len(ran)))
    else:
        say("nothing to do - every model in the table is already in Content")
    # An import regenerates per-asset materials and reassigns every slot, silently
    # undoing the shared set. Rebuilt here so no import can leave it undone.
    rebuild_fleet_materials()
    say("DONE")


main()
