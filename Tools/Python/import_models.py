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

- accessories/chainlink. Imported by build_fence_content.py instead: two static FBX posts and a
  texture pair, none of which this glTF vehicle table's machinery applies to.
- FuelDepot1, concepts. Concept art only; no export exists. (baggageCart1 was on this line
  until 2026-09-25, when it gained an export and a row below.)
- truckCab1, tankTrailer1, fuelTrailer1: feature/articulated-rig's import_rig.py and
  import_fueltrailer1.py. rigidCab1 on its own: a bare chassis the bodies are built on, not a
  vehicle anything dispatches.
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

from airside_import import (  # noqa: E402
    Spec, content_file, import_one, rebuild_fleet_materials, say,
    sweep_orphan_materials)


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
        note="Cessna 172S Skyhawk. 8.233 m long, 11.00 m span, 2.659 m to the fin tip - the "
             "real aircraft's own figures to within 6 cm. Origin on the NOSE gear; the main "
             "axle is at -163.0 uu, which is the figure FChassis::FixedAxleX wants when "
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
        # the figure FChassis::FixedAxleX wants when this type is authored.
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
             "is at -1560.0 uu, which is the figure FChassis::FixedAxleX wants when this "
             "type is authored. Twelve joints: the gear retracts and the nose bay doors "
             "hinge, but nothing in the engine drives either yet.",
    ),
    Spec(
        key="plane5",
        source=MODELS + r"\plane5\export\plane5.glb",
        mesh_dir="/Game/Aircraft/Plane5",
        skel_name="SK_Plane5",
        # THE SAME THREE NAMES AGAIN, and by now that IS a convention rather than a
        # coincidence - plane5/scripts/build_export.py names them deliberately to match
        # plane3's and plane4's. The LEGS are nosegear/maingear_L/maingear_R and are not
        # named here for the reason plane4's row gives: a leg's centre is not an axle.
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
        front_label="nose gear",
        rear_label="main gear",
        # NOSE GEAR. build_export.py's UE_ORIGIN is (0, 1.20, 0) in Blender world space -
        # nosewheel_steer's head dropped to the tarmac - so the convention is declared at
        # the source, as plane3's and plane4's are.
        origin_on="front",
        # THE FIRST RIG IN THE FLEET WITH MAIN-GEAR DOORS. plane4 retracts and hinges, but
        # only its NOSE bay is modelled; this one adds door_main_L/_R, so all four doors take
        # the same BayDoorAngleDegrees and the sequencing the model already drives - doors
        # open, gear travels, doors close - is visible at both ends of the aeroplane.
        note="Beechcraft King Air 350i. 14.00 m long, 17.69 m span, 4.40 m to the fin tip "
             "against the Beechcraft Specification and Description's 14.22 / 17.65 / 4.37. "
             "Origin on the NOSE gear; the main axle is at about -460 uu, which is the figure "
             "FChassis::FixedAxleX wants when this type is authored - and it is 35 cm SHORT "
             "of the S&D's 16 ft 3 in (4.95 m) wheelbase, because build_gear.py worked from "
             "14 ft 11 in. Fourteen joints, the most of any aeroplane here: the gear retracts "
             "forward into each nacelle, and BOTH bays have doors.",
    ),
    Spec(
        key="plane6",
        source=MODELS + r"\plane6\export\plane6.glb",
        mesh_dir="/Game/Aircraft/Plane6",
        skel_name="SK_Plane6",
        # THE SAME NOSE NAME A FIFTH TIME; THE MAINS ARE THE FIRST ROW THAT COULD NOT FOLLOW.
        # Every aeroplane above this one has ONE main wheel a side and calls it wheel_L /
        # wheel_R. A 777's main leg carries a SIX-WHEEL BOGIE - three axles a side - so
        # plane6/scripts/build_rig.py names them wheel_L1..L3 and wheel_R1..R3, and there is
        # no bone called wheel_L for this row to name.
        #
        # ALL SIX, NOT THE MIDDLE PAIR, and that is a decision rather than an inclusive
        # reflex. axle_centres_uu AVERAGES what it matches, so six wheels give the BOGIE'S
        # CENTRE - which is the point a multi-axle truck actually pivots about and therefore
        # the point FChassis::FixedAxleX wants. It reads -3123.0 uu against the middle
        # axle's own -3122.0, a centimetre apart because the bogie is not quite evenly
        # spaced (1.45 m forward, 1.48 m aft); Boeing's published 31.22 m wheelbase is
        # measured to that middle axle. Naming the middle pair here would measure correctly
        # today and silently stop the day a bogie is re-spaced or gains an axle; averaging
        # the truck cannot.
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L1", "wheel_L2", "wheel_L3",
                    "wheel_R1", "wheel_R2", "wheel_R3"],
        front_label="nose gear",
        rear_label="main bogies",
        # NOSE GEAR, and on this airframe the convention needed a ruling that plane3, plane4,
        # plane5 and plane7 never had to make. Those four have a vertical nose leg, so "the
        # steer axis on the tarmac" and "the contact patch" are the same point. PLANE6'S NOSE
        # LEG IS RAKED 12.4 DEGREES and they are 124 mm apart. build_export.py's UE_ORIGIN is
        # the CONTACT PATCH, (0, 5.890, 0) in Blender world space, argued at the source: it
        # is the point that stops on the mark painted on the stand, and it is the datum the
        # published 31.22 m wheelbase is measured from.
        origin_on="front",
        note="Boeing 777-300ER. 73.883 m long, 64.780 m span, 18.290 m to the fin tip "
             "against Boeing's published 73.86 / 64.80 / 18.5 - the span exact to 2 cm, "
             "which matters because 64.78 m is 22 cm inside Code E's 65 m ceiling and this "
             "is the first airframe in the project at any letter but B or C. Origin on the "
             "NOSE gear; the main bogie centre is at -3123.0 uu, which is the figure "
             "FChassis::FixedAxleX wants when this type is authored. Eighteen joints, the "
             "most of any model here: six main wheels on two three-axle bogies, and all "
             "four bay doors, as plane5's and plane7's have. NO TRUCK BONES - the bogies "
             "are rigid on their legs, so FGearPerformance::TruckTiltSeconds stays zero and "
             "the A380 is where that field starts earning its place.",
    ),
    Spec(
        key="plane7",
        source=MODELS + r"\plane7\export\plane7.glb",
        mesh_dir="/Game/Aircraft/Plane7",
        skel_name="SK_Plane7",
        # THE SAME THREE NAMES A FOURTH TIME. plane7/scripts/build_rig.py names them to match
        # plane3, plane4 and plane5 deliberately; the LEGS are nosegear/maingear_L/maingear_R
        # and are not named here for the reason plane4's row gives - a leg's centre is not an
        # axle.
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
        front_label="nose gear",
        rear_label="main gear",
        # NOSE GEAR, and on THIS model that is the whole point of the exercise rather than a
        # convention being followed quietly. plane7 REPLACES SM_PiperMeridian, whose import
        # measured it about the MAIN-gear axle and made UAircraftType::BuildPiperMeridian the
        # one type in the game declaring an origin deviation. The .glb's UE_ORIGIN is the
        # nose-gear contact patch, (0, -2.378, 0) in Blender world space, so the deviation is
        # retired at the source the way plane3's was - see plane7/README.md, which argues it
        # from the stand: the nose gear is the point that stops on the painted mark, so a
        # parked aircraft shares the stand's pose with no offset to compose.
        origin_on="front",
        # THE PLACEHOLDER IS GONE WITH IT. piperMeridian/ was a downloaded, baked, textured
        # asset - 19,418 tris and a 2048 BaseColor/Normal/MetallicRoughness set - and the one
        # aircraft in Content that wore its own materials rather than the shared fleet set.
        # plane7 is a fleet-style rebuild of the same aeroplane: 6,672 tris, no textures at
        # all, seven flat-colour slots whose values are lifted verbatim from plane3.glb and
        # plane4.glb so the by-value merge in build_fleet_materials.py folds them together
        # rather than logging drift.
        note="Piper PA-46-500TP Meridian, replacing the placeholder SM_PiperMeridian. 8.712 m "
             "long, 13.110 m span, 3.413 m to the fin tip against a published 9.02 / 13.11 / "
             "3.44 - the length is 3.4% short DELIBERATELY, see the model's README, and the "
             "wingspan (the figure stands and gates are sized from) is exact by construction. "
             "Origin on the NOSE gear; the main axle is at -237.8 uu, which is the figure "
             "FChassis::FixedAxleX wants. Thirteen joints: the gear retracts and BOTH bays "
             "have doors, as plane5's does.",
    ),
    Spec(
        key="plane8",
        source=MODELS + r"\plane8\export\plane8.glb",
        mesh_dir="/Game/Aircraft/Plane8",
        skel_name="SK_Plane8",
        # ONE MESH PER WHEEL, TWENTY-TWO OF THEM, and these are the MESH node stems rather
        # than the bones - plane8's wheel MESHES are named by unit and index
        # (wheel_wing_L_0..3, wheel_body_L_0..5) while its wheel BONES are numbered per side
        # (wheel_L1..L5) for the game's lookups. This row wants the meshes.
        #
        # ALL TWENTY, NOT ONE UNIT: axle_centres_uu averages what it matches, and the mean of
        # the wing bogies (station 33.55) and the body bogies (36.87) is the point the whole
        # main-gear group pivots about, which is what FChassis::FixedAxleX means. plane6
        # averages its six for the same reason.
        front_nodes=["wheel_nose_0", "wheel_nose_1"],
        rear_nodes=["wheel_wing_L_%d" % i for i in range(4)]
                   + ["wheel_wing_R_%d" % i for i in range(4)]
                   + ["wheel_body_L_%d" % i for i in range(6)]
                   + ["wheel_body_R_%d" % i for i in range(6)],
        front_label="nose gear",
        rear_label="main gear (wing and body bogies)",
        # NOSE GEAR, the CONTACT PATCH - plane6's ruling, for plane6's reason. This leg is
        # raked too: trunnion at station 5.672, axle at 4.972, so the steer axis meets the
        # ground 0.7 m aft of where the tyre does, and build_export.py's UE_ORIGIN is the
        # tyre, (0, 4.972, 0) in Blender world space, read from gear_pivots.json.
        origin_on="front",
        note="Airbus A380-800. 72.70 m long, 79.75 m span, 24.10 m to the fin tip against "
             "Airbus's published 72.72 / 79.75 / 24.09 - all three within 2 cm, and the "
             "span is the figure that puts this aeroplane at Code F, 25 cm inside its 80 m "
             "ceiling. Origin on the NOSE gear. Thirty-six joints, twice plane6's eighteen: "
             "four fans, ten main axles on four BOGIES with their own tilt bones - the "
             "first rig to use BONE_RULES' truck/bogie needles - five retract bones and "
             "ten bay doors. Every one carries a mesh; build_rig.py asserts it.",
    ),
    Spec(
        key="plane9",
        source=MODELS + r"\plane9\export\plane9.glb",
        mesh_dir="/Game/Aircraft/Plane9",
        skel_name="SK_Plane9",
        # plane5's FOURTEEN JOINTS AND plane4's NAMES: one main wheel a side, so wheel_L /
        # wheel_R, and the legs (maingear_L/_R, nosegear) are not named for the reason
        # plane4's row gives - a leg's centre is not an axle.
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
        front_label="nose gear",
        rear_label="main gear",
        # NOSE GEAR, the CONTACT PATCH - plane6's ruling. This nose leg is raked too (axle at
        # station 5.084, trunnion at 5.367), and build_export.py puts the origin under the
        # tyre, station 5.084 on the ground.
        origin_on="front",
        note="Airbus A320-200 with sharklets. 37.57 m long, 35.49 m span, 11.88 m to the fin "
             "tip against Airbus's published 37.57 / 35.80 / 11.76 - the span is the "
             "drawing's, 0.9 % short, and still Code C either way. Origin on the NOSE gear. "
             "Fourteen joints, plane5's set: the gear folds 86 degrees (solved against the "
             "wing root, not 90) and BOTH bays have doors.",
    ),
    Spec(
        key="plane10",
        source=MODELS + r"\plane10\export\plane10.glb",
        mesh_dir="/Game/Aircraft/Plane10",
        skel_name="SK_Plane10",
        # plane1's SIX JOINTS AND plane1's NAMES: root, prop, wheel_L/_R, nosewheel_steer,
        # nosewheel. Fixed gear, so no leg is a bone; maingear_L/_R and nosegear are meshes,
        # the last skinned to nosewheel_steer so the fork turns with the tyre.
        front_nodes=["nosewheel"],
        rear_nodes=["wheel_L", "wheel_R"],
        front_label="nose gear",
        rear_label="main gear",
        # NOSE GEAR, the contact patch - plane10/scripts/build_export.py's UE_ORIGIN is
        # (0, NOSE_S, 0), the nose axle's station on the ground.
        origin_on="front",
        note="Cessna 208B Grand Caravan, cargo pod. 12.675 m long, 15.875 m span, 4.707 m "
             "to the fin tip against the POH three-view's 12.675 / 15.875 / 4.712. Origin "
             "on the NOSE gear. Six joints and FIXED gear, plane1's shape: nothing retracts, "
             "so the type declares no gear cycle. One 3-blade prop the spinner rides.",
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
    # THE rigidCab1 FAMILY'S SECOND BODY. catering1 has no export folder of its own - it is
    # built inside rigidCab1/rigidCab1.blend beside fueltruck1 (catering1/README.md) - so its
    # source is rigidCab1/export/. Its mesh node names carry the asset prefix, as fueltruck1's
    # do.
    Spec(
        key="catering1",
        source=MODELS + r"\rigidCab1\export\catering1.glb",
        mesh_dir="/Game/Vehicles/Catering1",
        skel_name="SK_Catering1",
        front_nodes=["catering1_wheel_FL", "catering1_wheel_FR"],
        rear_nodes=["catering1_wheel_RL", "catering1_wheel_RR"],
        front_label="front axle",
        rear_label="rear axle",
        origin_on="rear",
        note="Narrowbody catering high-loader on the rigidCab1 chassis: a 4.80 m box on a "
             "two-stage scissor lift, floor 1.30 -> 4.00 m, and a platform that runs out "
             "1.80 m. Carries two baked clips, Lift and Platform; Interchange brings them in "
             "as AnimSequences beside the mesh.",
    ),
    Spec(
        key="baggageCart1",
        source=MODELS + r"\baggageCart1\export\baggageCart1.glb",
        mesh_dir="/Game/Vehicles/BaggageCart1",
        skel_name="SK_BaggageCart1",
        front_nodes=VEHICLE_FRONT,
        rear_nodes=VEHICLE_REAR,
        front_label="front axle",
        rear_label="rear axle",
        origin_on="rear",
        note="Covered baggage cart - TOWED, a turntable front axle on a towbar. Couples "
             "tow_eye onto the tower's hitch at z 0.308 (baggageCart1/README.md).",
    ),
    # TOWED BY truckCab1, WHICH IS NOT ON THIS BRANCH - it arrives with feature/articulated-rig.
    # Imported now because it is built in truckCab1's .blend and exported beside the tanker, and
    # standing it in the yard needs nothing from the tractor.
    Spec(
        key="curtainTrailer1",
        source=MODELS + r"\truckCab1\export\curtainTrailer1.glb",
        mesh_dir="/Game/Vehicles/CurtainTrailer1",
        skel_name="SK_CurtainTrailer1",
        front_nodes=["curtainTrailer1_curtain_wheel_1L", "curtainTrailer1_curtain_wheel_1R"],
        rear_nodes=["curtainTrailer1_curtain_wheel_3L", "curtainTrailer1_curtain_wheel_3R"],
        front_label="first axle",
        rear_label="third axle",
        # THE TRI-AXLE'S CENTRE, which is neither labelled axle - wheel_2L/2R sit on it at
        # x 0.0. Reported rather than asserted, as Spec's own docstring allows.
        origin_on=None,
        note="Curtain-side semi-trailer, tri-axle, kingpin 7.70 m ahead of the axle group's "
             "centre. For truckCab1 (feature/articulated-rig).",
    ),
]


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

    # AND THEN SWEPT, for the same reason and in the same breath. The rebuild above ORPHANS
    # the per-asset materials Interchange just generated; leaving them is ~48 KB of dead
    # uber-graph per flat colour, referenced by nothing and invisible to every test.
    #
    # ONLY THE FOLDERS THIS RUN IMPORTED INTO. A sweep of all of /Game/Aircraft and
    # /Game/Vehicles is what Tools/Python/clean_orphan_materials.py is for and is the right
    # tool when tidying up after the fact; running that breadth automatically would let an
    # import delete an unreferenced material somebody had parked beside a model they had not
    # wired up yet. A run that imports nothing sweeps nothing.
    #
    # IT WAS A SEPARATE SCRIPT AND HAD TO BE REMEMBERED until 2026-09-21, and it was not:
    # plane7 reached main in PR #225 carrying all seven of its generated materials.
    # results is one row per Spec, in order, so they zip. A SKIP contributes nothing; a FAIL
    # is swept anyway, because the sweep deletes only what nothing references and a
    # half-imported model's orphans are exactly as dead as a whole one's.
    swept = [spec.mesh_dir for spec, (_, ok) in zip(SPECS, results) if ok is not None]
    if swept:
        say("-" * 70)
        sweep_orphan_materials(swept)

    say("DONE")


main()
