"""Authors DA_Aircraft_Plane8 and sets ABP_Plane8's per-model animation figures. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every build_plane<N>_type.py states it and
for the same reason: a figure typed here is a second opinion about an object the export
already answers for, and it does not move when the model does.

WHAT THIS TYPE IS FOR. plane6's 777 was the first Code E aeroplane and the argument that an
airport has to be REBUILT rather than extended. This is Code F - the last letter there is -
and what it adds is not a longer runway (its take-off field length is SHORTER than the 777's)
but WIDTH: 79.75 m of span against the 777's 64.78, 7.4 m of fuselage, 80 m stands, 25 m
taxiways. A Code F stand is the largest thing the game can lay, and this is the one aeroplane
that needs it.

THE PRIMARY SOURCE IS ON DISK FOR EVERYTHING, AND THAT IS NEW. plane6's header records that
there is no Boeing D6-58329 in its concept folder, so its performance block is published
figures with nothing to check them against. plane8/concept/ac_a380_1223.pdf is Airbus's
"A380 Aircraft Characteristics - Airport and Maintenance Planning", Rev 18 (Dec 2023), 320
pages, and every performance figure below cites its section. Two of them - the field lengths
- are read off CHARTS (3-3-1, 3-4-1), which extract as axes rather than curves, so they are
approximate and say so; the approach speed (3-5-0) and the steering angle (4-3-0) are stated
in the text and exact.

THREE RIG FIGURES ARE READ FROM THE MODELS REPO, NOT TYPED. plane8/scripts/rig_map.json
carries `retract_deg`, `truck_tilt_deg` and `door_close_deg` per bone, written by the same
build_rig.py that proved each against build_gear.py's fold. Tools/wire_plane8_anim.py reads
the same file for its multipliers, so the ABP's per-type angle and the graph's ratios cannot
disagree - CLAUDE.md's rule about lists that must agree.
"""
import json
import math
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane8"
ABP = "/Game/Aircraft/Plane8/ABP_Plane8"
# The ICAO type designator for an A380-800 - what a flight plan and a stand board call it.
SHORT_CODE = "A388"
MESH = "/Game/Aircraft/Plane8/SK_Plane8"

MODELS = r"C:\repos\AirportMgr2Models\plane8"
SOURCE = os.path.join(MODELS, "export", "plane8.glb")
RIG_MAP = os.path.join(MODELS, "scripts", "rig_map.json")

# The parts this type is measured from. NAMED HERE rather than assumed equal to plane6's,
# because a rig that renamed one should raise rather than default. The leg named is the WING
# gear's, the taller of the two main units; the fan is an inboard one, all four being one
# mesh placed four times.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "gear_wing_L"
PROP = "fan_L_in"

# TEN MAIN WHEEL BONES A SIDE ON TWO UNITS, and the naming is the game's rule rather than the
# rigger's preference: airside_anim.axle_anchor, AirframeAxlesTest::LeftMainWheel and this
# list all find a left main wheel by `wheel_L<n>`. Wing axles are 1-2 and body axles 3-5,
# either side; the unit is recoverable from the parent bone.
#
# ALL TEN ARE AVERAGED FOR THE FIXED AXLE, as plane6 averages its six: axle_centres_uu's
# mean is the point the whole main-gear group pivots about, which is what
# FAirframe::FixedAxleX means for the rolling-steer law. The TRACK is measured off the WING
# bogies alone - see axles_and_radius.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2", "wheel_L3", "wheel_L4", "wheel_L5")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2", "wheel_R3", "wheel_R4", "wheel_R5")
WING_WHEELS_L = MAIN_WHEELS_L[:2]
WING_WHEELS_R = MAIN_WHEELS_R[:2]

# TWENTY-FOUR BLADES - a GP7200 or a Trent 900, both 24, and plane8/scripts/build_fan.py
# arrays exactly that many. NOT cosmetic: UAirsideAgentAnim::PropStepDegrees resolves the
# frame rate against ONE BLADE REPEAT, 360/N degrees, so a 24-blade fan told it has three is
# allowed a step eight times too large and aliases backwards at any frame rate.
PROP_BLADE_COUNT = 24

# IcaoCode.cpp's Code F row, as this script expects to find it. Code F is AUTHORED, not
# measured - the row's own comment says F "has no letter above it" - and this is the first
# type that tests it. The A380 measures 6775 uu from its stop mark to its tail and 7975 uu of
# span, inside both by 125 and 25 uu. The 25 cm of span margin is the same shape as the
# 777's 22 cm under Code E: a real aeroplane built to the letter's limit.
MAX_TAIL_AFT_F = 6900.0
MAX_SPAN_F = 8000.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def rig_angles():
    """The three per-type angles, from the file the rig proved them in."""
    with open(RIG_MAP) as handle:
        rig = json.load(handle)
    retract = rig["retract_deg"]
    truck = rig["truck_tilt_deg"]
    return {
        # THE WING'S 90, and the other units scale off it in wire_plane8_anim.py.
        "gear_retracted": retract["gear_wing_L"],
        # THE BODY BOGIE'S 54.72; the wing bogies scale to 0 in the wiring.
        "truck_tilted": max(truck.values()),
        "bay_door_closed": rig["door_close_deg"],
        "_retract": retract, "_truck": truck,
    }


def measure():
    """The footprint, the fan and the gear leg's height, off the export's own parts."""
    parts = part_bounds_uu(read_gltf(SOURCE))

    def find(stem):
        if stem not in parts:
            raise KeyError("no part named %s in %s - the rig was renamed. Parts: %s"
                           % (stem, SOURCE, ", ".join(sorted(parts))))
        return parts[stem]

    every = list(parts.values())
    nose_x = max(high[0] for _, high in every)
    tail_x = min(low[0] for low, _ in every)

    wing_lo, wing_hi = find(WING)
    stab_lo, stab_hi = find(STABILISER)
    prop_lo, prop_hi = find(PROP)
    leg_lo, leg_hi = find(GEAR_LEG)

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        # 79.75 m, the published figure to the centimetre, and the span extremity is the
        # WINGTIP FENCE rather than the wing - plane8/SPEC.md argues it. Code F runs to 80 m.
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The fan's widest sweep is its disc, across the airframe. The model's fan ring is traced
    # off the drawing's FRONT view - plane8/SPEC.md, "Front view draws the intake".
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane8's reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX - the bone is a statement about where the axle is,
    and the hub's HEIGHT above the contact plane IS the radius.

    TWENTY WHEELS, TWO TYRE SIZES, ONE RADIUS. The wing gear's tyres are 1.351 m and the
    body gear's 1.367 m (plane8/scripts/gear_pivots.json), 8 mm apart in radius; the mean is
    within a centimetre of either and the check below allows 1 uu, which is what proves they
    really are two sizes rather than one axle off the ground. The nose tyre is 1.270 m and
    is NOT averaged in, because MainWheelRadius is what WheelAngleDegrees divides GROUND
    SPEED by and every wheel takes the one angle - the nose pair will roll 6 % slow, which is
    the compromise plane6's three-axle bogie already makes against its own nose wheel.

    THE TRACK IS THE WING BOGIES', NOT THE MEAN OF ALL TWENTY. FAirframe::MainGearTrack is
    where tyre smoke lands at touchdown and where a tug lines up; the mean of a 12.5 m wing
    track and a 5.3 m body track is a 8.1 m figure that puts the smoke between the two units
    where there is no wheel at all. The OUTER pair is what a viewer sees.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        raise KeyError("no %s - run import_models.py first" % MESH)

    pose = mesh.get_editor_property("skeleton").get_reference_pose()
    at = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        bone = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        at[str(name)] = bone.translation

    wanted = ("nosewheel",) + MAIN_WHEELS_L + MAIN_WHEELS_R
    missing = [name for name in wanted if name not in at]
    if missing:
        raise KeyError("SK_Plane8 has no %s bone(s) - the rig was renamed. Bones: %s"
                       % (", ".join(missing), ", ".join(sorted(at))))

    left = [at[name] for name in MAIN_WHEELS_L]
    right = [at[name] for name in MAIN_WHEELS_R]
    both = left + right

    def mean(values):
        return sum(values) / float(len(values))

    mains_x = mean([v.x for v in both])
    radius = mean([v.z for v in both])
    track = abs(mean([at[n].y for n in WING_WHEELS_L])
                - mean([at[n].y for n in WING_WHEELS_R]))

    # FIVE AXLES A SIDE MUST BE AT FIVE STATIONS spanning the two units - a bogie mis-rigged
    # as stacked bones averages to a plausible wheelbase and rolls its tyres through each
    # other. The wing and body units are 2 m apart, so the spread is several radii.
    stations = sorted(v.x for v in left)
    spread = stations[-1] - stations[0]
    if spread < 3.0 * radius:
        raise ValueError("the five left main wheel bones span %.1f uu, less than three "
                         "wheel radii (%.1f) - two units cannot be that close" % (spread, radius))

    # AND AT ONE HEIGHT, because every one of them is on the ground. Two tyre sizes put the
    # hubs 0.8 uu apart, inside the 1 uu this allows; a hub a radius off is a wheel in the air.
    for name in MAIN_WHEELS_L + MAIN_WHEELS_R:
        if abs(at[name].z - radius) > 1.0:
            raise ValueError("%s's hub is at z=%.1f against the mains' mean %.1f - the twenty "
                             "main wheels are not on one plane" % (name, at[name].z, radius))

    if track <= radius:
        raise ValueError("the two wing bogies are %.1f uu apart, inside one wheel's %.1f uu "
                         "radius - the main gear bones are on top of each other" % (track, radius))

    if not 5.0 < radius < leg_height:
        raise ValueError("the main-gear hub sits at z=%.1f against a %.1f uu leg - the bone "
                         "is not on the axle, or the model is not on the ground plane"
                         % (radius, leg_height))

    return at["nosewheel"].x, mains_x, radius, track


_MEASURED = None


def measured():
    global _MEASURED
    if _MEASURED is None:
        footprint, prop, leg = measure()
        steer_x, fixed_x, radius, track = axles_and_radius(leg)
        _MEASURED = dict(footprint=footprint, prop_diameter=prop, wheel_radius=radius,
                         steer_axle_x=steer_x, fixed_axle_x=fixed_x, main_gear_track=track)
    return _MEASURED


def tightest_radius_uu():
    """R >= L / sin(lock), the rule FSpeedProfile applies, on the MEASURED wheelbase."""
    m = measured()
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    return wheelbase / math.sin(math.radians(STEERING["max_steer_degrees"]))


# --- Published, from Airbus AC A380 Rev 18 (plane8/concept/ac_a380_1223.pdf) ------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s. Figures are at MTOW
# 575 t / MLW 395 t, sea level, ISA, unless a line says otherwise.
GROUND = {
    # THE FLEET'S TAXI FIGURES, UNCHANGED: a taxi speed is a procedure, not a capability.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 180, BELOW plane6's 220, BECAUSE THE THRUST-TO-WEIGHT IS. Four ~70,000 lbf engines on
    # 575 t is 0.22 against the 777-300ER's 0.30, and it shows: an A380 take-off roll is
    # visibly leisurely. Vr about 158 kn (published, not in the AC document - it is an
    # operating figure). RequiredRoll works out to about 1,830 m against the ~3,000 m
    # published below.
    "takeoff": dict(accel=180.0, decel=400.0, speed_cap=8121.0),   # Vr about 158 kn
    # 138 kn EXACTLY: AC 3-5-0, "the final approach speed is 138 kt at a Maximum Landing
    # Weight of 395 000 kg", Approach Category C - the same category as a 737, which on a
    # 575 t aeroplane is the wing doing its job.
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7093.0),   # Vref 138 kn, AC 3-5-0
}

# STEERING. 70 DEGREES IS AIRBUS'S OWN FIGURE: AC 4-3-0 "Minimum Turning Radii" tabulates a
# steering angle of 70 (effective 69.5) and a nose-gear minimum turning radius of 32.66 m.
# On this model's measured wheelbase R >= L / sin(70) comes out within a metre of that, and
# author_type() says the computed figure so the agreement is checked rather than claimed.
# An A380 also steers its BODY gear (AC 2-9-0, "Body Wheel Steering") to reduce tyre scrub;
# the model does not, and the geometry above does not need it.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 118.0,     # 0.12 g, the widebodies' figure
}

CLIMB = {
    # TEN DEGREES: unlike the 777-300ER the A380 is not tail-strike limited - a short
    # fuselage for its span and tall gear give it geometry to spare - and it rotates to a
    # conventional attitude at a conventional rate.
    "lift_angle_at_rotate_degrees": 10.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 2.5,
    "climb_speed": 12850.0,       # 250 kn, the standard climb speed below 10,000 ft
    # 800 m, DERIVED: the gear cycle below is 21 s and starts at 90 m; at 250 kn on a
    # 12-degree climb that is about 565 m of height before the last door shuts, which
    # plane6's 650 m would not clear. author_type() asserts the relation.
    "clear_altitude": 80000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # THE REAL FAN SPEED, approximately: a GP7270's fan turns about 2,500 rpm at 100 % N1.
    # NOT in the AC document (it carries no engine performance) and stated here as the
    # order of magnitude the view needs rather than a certified figure - UAirsideAgentAnim::
    # PropDisplayCapRPM chooses what is drawn, and this only has to be honest about which
    # side of that cap a big fan sits.
    "max_rpm": 2500.0,
    "spool_up_seconds": 10.0,
    "spool_down_seconds": 30.0,
}

# THE GEAR. Five units, four bogies, ten doors: the biggest undercarriage in the game.
GEAR = {
    # 14 SECONDS, against the 777's 12: four bogies into three bays, and the body units
    # travel further than any other leg here.
    "travel_seconds": 14.0,
    # 2.5 s A DOOR - the nose pair are 4.2 m long.
    "door_seconds": 2.5,
    # TWO SECONDS, AND THIS IS THE FIRST NON-ZERO FIGURE IN THE FLEET. FGearPerformance::
    # TruckTiltSeconds was added for plane6 and left at zero because plane6 rigged no truck
    # bone; plane8's body bogies counter-rotate 54.72 degrees to lie level in the bay, the
    # rig has bones for it, and the ABP drives them - so the stage in the cycle is real.
    "truck_tilt_seconds": 2.0,
    "retract_above_height": 9000.0,   # ~295 ft, the fleet's procedure
    "extend_below_height": 15000.0,   # above FinalAltitude, so arrivals are born down
}

# PUBLISHED FIELD LENGTHS - AND THEY ARE THE TWO FIGURES READ OFF CHARTS. AC 3-3-1 (take-off
# weight limitation, ISA, sea level, Trent 900 / GP7200 figures agree within the chart's
# width) and AC 3-4-1 (landing field length, dry). The charts extract as axes only, so these
# are read by eye to about 50 m and Airbus's own note applies: "given for information only".
#
# THE TAKE-OFF FIELD LENGTH IS SHORTER THAN THE 777-300ER's 3,120 m, which is the whole
# reason this type's gameplay claim is WIDTH rather than length: a Code E runway of 3,120 m
# already admits it, and what refuses it is a 45 m runway, a 23 m taxiway and a Code E stand.
REQUIREMENTS = {
    "takeoff_field_length": 300000.0,   # ~3,000 m at 575 t, AC 3-3-1
    "landing_field_length": 205000.0,   # ~2,050 m at 395 t, AC 3-4-1
}

# TWO HOURS. 525 seats on two decks through eight doors, and a long-haul uplift; plane6's
# ninety minutes was the longest in the game and this is meant to be longer.
TURNAROUND_SECONDS = 7200.0

# PUSHBACK IS NOT SET HERE - Tools/Python/build_pushback_needs.py owns EPushbackNeed for
# every type, for the reason plane6's script gives.


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    # CODE F, THE FIRST AND ONLY. Checked against the measurement below, not asserted.
    asset.set_editor_property("code", unreal.Name("F"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("A380-800"))

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        fail("no %s to point the type at" % MESH)
    else:
        asset.set_editor_property("mesh", mesh)

    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s to point the type at" % ABP)
    elif abp.generated_class() is None:
        fail("%s has no generated class; compile it before running this" % ABP)
    else:
        asset.set_editor_property("anim_class", abp.generated_class())

    m = measured()
    say("measured off the export: wheel radius %.1f, fan %.1f, nose %.1f, tail %.1f uu"
        % (m["wheel_radius"], m["prop_diameter"], m["footprint"]["nose_x"],
           m["footprint"]["tail_x"]))
    say("measured off the rig: steer axle %.1f, main-gear centre %.1f uu (wheelbase %.1f), "
        "wing track %.1f" % (m["steer_axle_x"], m["fixed_axle_x"],
                             abs(m["fixed_axle_x"] - m["steer_axle_x"]), m["main_gear_track"]))

    span = m["footprint"]["wingspan"]
    if span > MAX_SPAN_F:
        fail("the measured span is %.1f uu, past Code F's %.0f - there is no letter above F."
             % (span, MAX_SPAN_F))
    else:
        say("PASS span %.1f uu is inside Code F's %.0f by %.1f uu (%.2f m)"
            % (span, MAX_SPAN_F, MAX_SPAN_F - span, (MAX_SPAN_F - span) / 100.0))

    tail_aft = -(m["footprint"]["tail_x"] - m["steer_axle_x"])
    if tail_aft > MAX_TAIL_AFT_F:
        fail("the tail reaches %.1f uu aft of the stop mark against IcaoCode's Code F row of "
             "%.0f - raise the row in Solve/IcaoCode.cpp and this constant with it."
             % (tail_aft, MAX_TAIL_AFT_F))
    else:
        say("PASS the tail reaches %.1f uu aft of the stop mark, inside Code F's %.0f"
            % (tail_aft, MAX_TAIL_AFT_F))

    # AGAINST AIRBUS'S OWN 32.66 m, AC 4-3-0. Said out loud because it is the one performance
    # figure here that the geometry DERIVES and the document also STATES, so a disagreement
    # would be a wheelbase or a lock angle that is wrong rather than a figure to tune.
    say("tightest followable radius %.0f uu (%.1f m) at %.0f degrees of lock; Airbus AC 4-3-0 "
        "states 32.66 m for the nose gear"
        % (tightest_radius_uu(), tightest_radius_uu() / 100.0, STEERING["max_steer_degrees"]))

    asset.set_editor_property("steer_axle_x", m["steer_axle_x"])
    asset.set_editor_property("fixed_axle_x", m["fixed_axle_x"])
    asset.set_editor_property("main_gear_track", m["main_gear_track"])
    asset.set_editor_property("main_wheel_radius", m["wheel_radius"])
    asset.set_editor_property("propeller_diameter", m["prop_diameter"])
    asset.set_editor_property("turnaround_seconds", TURNAROUND_SECONDS)

    footprint = asset.get_editor_property("footprint")
    for field, value in m["footprint"].items():
        footprint.set_editor_property(field, value)
    asset.set_editor_property("footprint", footprint)

    ground = asset.get_editor_property("ground")
    for name, values in GROUND.items():
        set_regime(ground, name, values)
    for field, value in STEERING.items():
        ground.set_editor_property(field, value)
    asset.set_editor_property("ground", ground)

    for group, values in (("climb", CLIMB), ("approach", APPROACH), ("engine", ENGINE),
                          ("gear", GEAR)):
        block = asset.get_editor_property(group)
        for field, value in values.items():
            block.set_editor_property(field, value)
        asset.set_editor_property(group, block)

    # THE GEAR CYCLE MUST FINISH BEFORE THE AGENT IS REMOVED - and on this type the cycle has
    # a truck-tilt stage at each end, which plane6's copy of this check did not count because
    # its figure was zero. Counted here: two doors, two tilts, one travel.
    cycle = GEAR["door_seconds"] * 2.0 + GEAR["truck_tilt_seconds"] * 2.0 + GEAR["travel_seconds"]
    rate = CLIMB["climb_speed"] * math.radians(CLIMB["climb_pitch_degrees"])
    top = GEAR["retract_above_height"] + rate * cycle
    if top >= CLIMB["clear_altitude"]:
        fail("the %.0f s gear cycle finishes at about %.0f uu but the agent is cleared at "
             "%.0f - it would vanish mid-retraction." % (cycle, top, CLIMB["clear_altitude"]))
    else:
        say("PASS the %.0f s gear cycle finishes at about %.0f uu, below the %.0f uu the "
            "agent is cleared at" % (cycle, top, CLIMB["clear_altitude"]))

    requirements = asset.get_editor_property("requirements")
    for field, value in REQUIREMENTS.items():
        requirements.set_editor_property(field, value)
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults.

    FIVE FIGURES WHERE plane6 SETS THREE: the wheel radius, the blade count and the door angle
    as before, plus GearRetractedAngleDegrees and TruckTiltedAngleDegrees. The first is 90,
    which is also the class default, and is written anyway - a default that happens to be
    right is not a declaration. The second is 54.72 and is the first non-zero value of that
    property in the fleet. Both come from rig_map.json, the same file the wiring script
    reads, so the per-type angle and the per-bone ratios cannot drift apart.
    """
    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s" % ABP)
        return
    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class; compile it first" % ABP)
        return

    angles = rig_angles()
    want = {
        "main_wheel_radius": measured()["wheel_radius"],
        "prop_blade_count": PROP_BLADE_COUNT,
        "bay_door_closed_angle_degrees": angles["bay_door_closed"],
        "gear_retracted_angle_degrees": angles["gear_retracted"],
        "truck_tilted_angle_degrees": angles["truck_tilted"],
    }
    defaults = unreal.get_default_object(generated)
    for prop, value in want.items():
        defaults.set_editor_property(prop, value)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class())
    for prop, value in want.items():
        got = after.get_editor_property(prop)
        if abs(float(got) - float(value)) > 0.01:
            fail("ABP %s read back as %s, expected %s" % (prop, got, value))
        else:
            say("PASS ABP_Plane8 %s = %s" % (prop, got))
    say("NOTE per-bone retract %s and truck tilt %s ride the wiring multipliers - see "
        "Tools/wire_plane8_anim.py" % (angles["_retract"], angles["_truck"]))


def verify(path):
    """Read back from disk."""
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("%s did not survive the save" % path)
        return

    m = measured()
    checks = [
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"), m["wheel_radius"]),
        ("fixed_axle_x", asset.get_editor_property("fixed_axle_x"), m["fixed_axle_x"]),
        ("main_gear_track", asset.get_editor_property("main_gear_track"), m["main_gear_track"]),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"), TURNAROUND_SECONDS),
    ]
    for name, got, want in checks:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %s = %.2f" % (name, got))

    footprint = asset.get_editor_property("footprint")
    span = footprint.get_editor_property("wingspan")
    if abs(span - m["footprint"]["wingspan"]) > 0.1:
        fail("wingspan read back as %.1f" % span)
    else:
        say("PASS footprint wingspan %.1f uu (%.1f m)" % (span, span / 100.0))

    for prop, want in (("short_code", SHORT_CODE), ("code", "F")):
        got = str(asset.get_editor_property(prop))
        if got != want:
            fail("%s read back as %s, expected %s" % (prop, got, want))
        else:
            say("PASS %s = %s" % (prop, got))

    say("NOTE pushback_need = %s (authored by build_pushback_needs.py)"
        % asset.get_editor_property("pushback_need"))

    for prop in ("mesh", "anim_class"):
        got = asset.get_editor_property(prop)
        if got is None:
            fail("%s is unset - the agent would fall back to the game-wide default mesh" % prop)
        else:
            say("PASS %s = %s" % (prop, got.get_name()))

    gear = asset.get_editor_property("gear")
    for field, want in GEAR.items():
        got = gear.get_editor_property(field)
        if abs(got - want) > 0.01:
            fail("gear.%s read back as %s, expected %s" % (field, got, want))
        else:
            say("PASS gear.%-21s = %.1f" % (field, got))

    requirements = asset.get_editor_property("requirements")
    takeoff = requirements.get_editor_property("takeoff_field_length")
    landing = requirements.get_editor_property("landing_field_length")
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m)"
        % (takeoff, takeoff / 100.0, landing, landing / 100.0))

    ground = asset.get_editor_property("ground")
    for name in ("taxi", "takeoff", "landing"):
        regime = ground.get_editor_property(name)
        cap = regime.get_editor_property("speed_cap")
        say("  ground.%-8s accel=%.0f decel=%.0f cap=%.0f uu/s (%.0f kn)"
            % (name, regime.get_editor_property("accel"),
               regime.get_editor_property("decel"), cap, cap / 51.4))


def run():
    if unreal.EditorAssetLibrary.load_asset(MESH) is None:
        fail("no %s - run import_models.py first" % MESH)
        say("DONE")
        return
    path = author_type()
    if path is None:
        say("DONE")
        return
    say("authored %s" % path)
    verify(path)
    set_anim_defaults()
    say("DONE")


run()
