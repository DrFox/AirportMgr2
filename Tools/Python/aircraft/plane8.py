"""DA_Aircraft_Plane8 / ABP_Plane8 - the Airbus A380-800.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. plane6's 777 was the first Code E aeroplane and the argument that an
airport has to be REBUILT rather than extended. This is Code F - the last letter there is - and
what it adds is not a longer runway (its take-off field length is SHORTER than the 777's) but
WIDTH: 79.75 m of span against the 777's 64.78, 7.4 m of fuselage, 80 m stands, 25 m taxiways. A
Code F stand is the largest thing the game can lay, and this is the one aeroplane that needs it.

THE PRIMARY SOURCE IS ON DISK FOR EVERYTHING, AND THAT IS NEW. plane6's header records that
there is no Boeing D6-58329 in its concept folder, so its performance block is published figures
with nothing to check them against. plane8/concept/ac_a380_1223.pdf is Airbus's "A380 Aircraft
Characteristics - Airport and Maintenance Planning", Rev 18 (Dec 2023), 320 pages, and every
performance figure below cites its section. Two of them - the field lengths - are read off
CHARTS (3-3-1, 3-4-1), which extract as axes rather than curves, so they are approximate and
say so; the approach speed (3-5-0) and the steering angle (4-3-0) are stated in the text and
exact.

THREE RIG FIGURES ARE READ FROM THE MODELS REPO, NOT TYPED. plane8/scripts/rig_map.json carries
`retract_deg`, `truck_tilt_deg` and `door_close_deg` per bone, written by the same build_rig.py
that proved each against build_gear.py's fold. Tools/wire_plane8_anim.py reads the same file for
its multipliers, so the ABP's per-type angle and the graph's ratios cannot disagree.

THIS RIG'S rig_map.json HAS A DIFFERENT SHAPE FROM plane9's/plane11's/plane13's, because plane8
genuinely has more than one gear unit and they do not: `retract_deg`/`truck_tilt_deg` are
per-bone dicts here, keyed by leg name, where the other three's `angles` object is flat. The
`RigMap` angle source below says where in THIS file each of the three angles lives, rather than
forcing one shape onto a file that does not have it.
"""
import json

from build_aircraft_type import AircraftSpec, RigMap, say_span_and_tail, say_tightest_radius

TYPE_NAME = "DA_Aircraft_Plane8"
ABP = "/Game/Aircraft/Plane8/ABP_Plane8"
MESH = "/Game/Aircraft/Plane8/SK_Plane8"
MODELS = r"C:\repos\AirportMgr2Models\plane8"
SOURCE = MODELS + r"\export\plane8.glb"
RIG_MAP = MODELS + r"\scripts\rig_map.json"

# TEN MAIN WHEEL BONES A SIDE ON TWO UNITS - wing axles are 1-2 and body axles 3-5, either side.
# ALL TEN ARE AVERAGED FOR THE FIXED AXLE, as plane6 averages its six. The TRACK IS THE WING
# BOGIES' ALONE - the mean of a 12.5 m wing track and a 5.3 m body track puts the smoke between
# the two units, where there is no wheel at all; the outer pair is what a viewer sees.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2", "wheel_L3", "wheel_L4", "wheel_L5")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2", "wheel_R3", "wheel_R4", "wheel_R5")
WING_WHEELS_L = MAIN_WHEELS_L[:2]
WING_WHEELS_R = MAIN_WHEELS_R[:2]

# --- Published, from Airbus AC A380 Rev 18 (plane8/concept/ac_a380_1223.pdf) ------------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 180, BELOW plane6's 220, BECAUSE THE THRUST-TO-WEIGHT IS: four ~70,000 lbf engines on
    # 575 t is 0.22 against the 777-300ER's 0.30.
    "takeoff": dict(accel=180.0, decel=400.0, speed_cap=8121.0),   # Vr about 158 kn
    # 138 kn EXACTLY: AC 3-5-0, Approach Category C - the same category as a 737.
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7093.0),   # Vref 138 kn, AC 3-5-0
}

# STEERING. 70 DEGREES IS AIRBUS'S OWN FIGURE: AC 4-3-0 tabulates a steering angle of 70
# (effective 69.5) and a nose-gear minimum turning radius of 32.66 m - checked against the
# geometry below rather than merely quoted.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 118.0,     # 0.12 g, the widebodies' figure
}

CLIMB = {
    # TEN DEGREES: unlike the 777-300ER the A380 is not tail-strike limited.
    "lift_angle_at_rotate_degrees": 10.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 2.5,
    "climb_speed": 12850.0,       # 250 kn, the standard climb speed below 10,000 ft
    # 800 m, DERIVED: the gear cycle below is 21 s and starts at 90 m.
    "clear_altitude": 80000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    "max_rpm": 2500.0,
    "spool_up_seconds": 10.0,
    "spool_down_seconds": 30.0,
}

# THE GEAR. Five units, four bogies, ten doors: the biggest undercarriage in the game.
GEAR = {
    "travel_seconds": 14.0,
    "door_seconds": 2.5,
    # TWO SECONDS, AND THIS IS THE FIRST NON-ZERO FIGURE IN THE FLEET. plane8's body bogies
    # counter-rotate 54.72 degrees to lie level in the bay, the rig has bones for it, and the
    # ABP drives them.
    "truck_tilt_seconds": 2.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

# THE TAKE-OFF FIELD LENGTH IS SHORTER THAN THE 777-300ER's 3,120 m, which is the whole reason
# this type's gameplay claim is WIDTH rather than length.
REQUIREMENTS = {
    "takeoff_field_length": 300000.0,   # ~3,000 m at 575 t, AC 3-3-1
    "landing_field_length": 205000.0,   # ~2,050 m at 395 t, AC 3-4-1
}

TURNAROUND_SECONDS = 7200.0   # two hours: 525 seats on two decks through eight doors

# TWENTY-FOUR BLADES - a GP7200 or a Trent 900, both 24, and build_fan.py arrays that many.
PROP_BLADE_COUNT = 24


def _anim_multiplier():
    """Per-bone gear/truck multipliers for Tools/wire_plane_anim.py's PLAN, read off THE SAME
    plane8/scripts/rig_map.json the ABP's `angles=RigMap` below reads - one file, read twice,
    rather than a second copy of 90.0/54.72 typed into a wiring script the way
    Tools/wire_plane8_anim.py used to (Issue #294).

    THE A380 HAS THREE RETRACT ANGLES - nose forward 77.66, wing inboard 90.00, body aft
    54.72 - and UAirsideAgentAnim drives every `gear` bone from ONE GearRetractedAngleDegrees
    (the wing's 90, set on ABP_Plane8 by the `angles=RigMap` gear lambda below). The other
    four legs take -1 TIMES THEIR OWN RATIO against that reference, so the angle the graph
    applies - (1 - GearDownFraction) x 90 x ratio - is linear in the fraction and every leg
    runs on the model's one cycle, arriving folded at its own solved angle exactly when the
    wing gear arrives at 90.

    THE SAME TRICK CARRIES TWO TRUCK ANGLES: the wing bogies tilt 0 and the body bogies
    54.72 (the counter-rotation that lies them level in the bay). TruckTiltedAngleDegrees is
    54.72 (the RigMap truck lambda's max()), so the body bogies take -1 and the wing bogies
    take -1 x 0 / 54.72 - nothing - while staying in the chain and the axis plan, so a rig
    that later gives them a real tilt changes one number here.

    `or 0.0` TURNS -0.0 INTO 0.0. A wing bogie's -1 x 0 / 54.72 is IEEE negative zero, which
    "%f" prints as "-0.000000" - and wire_anim_lib.verify compares the pin value it reads back
    from the editor as a STRING, so a graph that is exactly right would fail on the sign of
    nothing.
    """
    with open(RIG_MAP) as handle:
        rig = json.load(handle)
    gear_ref = rig["retract_deg"]["gear_wing_L"]
    truck_ref = max(rig["truck_tilt_deg"].values())
    gear = {bone: -1.0 * deg / gear_ref for bone, deg in rig["retract_deg"].items()}
    truck = {bone: (-1.0 * deg / truck_ref) or 0.0
             for bone, deg in rig["truck_tilt_deg"].items()}
    return dict(gear, **truck)


def _report(spec, m, say, fail):
    say_span_and_tail(spec, m, say)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane8",
        type_name=TYPE_NAME,
        # The ICAO type designator for an A380-800.
        short_code="A388",
        display_name="A380-800",
        # CODE F, THE FIRST AND ONLY - checked against the measurement, not asserted.
        code="F",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        # The WING gear's leg (the taller of the two main units) - only its height is taken.
        gear_leg="gear_wing_L",
        # An inboard fan; all four are one mesh placed four times.
        prop="fan_L_in",
        main_wheels_l=MAIN_WHEELS_L,
        main_wheels_r=MAIN_WHEELS_R,
        track_wheels_l=WING_WHEELS_L,
        track_wheels_r=WING_WHEELS_R,
        # plane8's five-a-side spans TWO gear units 2 m apart, so its stations must clear three
        # wheel radii rather than one to prove they are not stacked on a single station.
        station_spread_multiplier=3.0,
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "fan %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu"),
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "main-gear centre %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f), "
                 "wing track %(main_gear_track).1f"),
        tightest_radius_line=(
            "tightest followable radius %(radius_uu).0f uu (%(radius_m).1f m) at %(lock).0f "
            "degrees of lock; Airbus AC 4-3-0 states 32.66 m for the nose gear"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        angles=RigMap(
            RIG_MAP,
            # THE WING'S 90, and the other units scale off it in wire_plane8_anim.py.
            gear=lambda d: d["retract_deg"]["gear_wing_L"],
            door=lambda d: d["door_close_deg"],
            # THE BODY BOGIE'S 54.72; the wing bogies scale to 0 in the wiring.
            truck=lambda d: max(d["truck_tilt_deg"].values()),
            note=lambda d: ("NOTE per-bone retract %s and truck tilt %s ride the wiring "
                           "multipliers - see Tools/wire_plane8_anim.py"
                           % (d["retract_deg"], d["truck_tilt_deg"])),
        ),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
        report_extra=_report,
        # THE BONES THAT ARE DELIBERATELY NOT SQUARE TO THE AIRFRAME - plane6's three, for
        # plane6's reasons, measured on this rig rather than assumed to carry over:
        #
        #   * nosewheel_steer - raked 12.3 degrees: the leg runs from the axle at station
        #     4.972 up to the trunnion at 5.672, and a nose leg steers about its STRUT. Its
        #     local axis is (0, 0.21, 0.98), 0.977 along UE's up - under resolve_axis's 0.99
        #     line by design.
        #   * door_nose_L / door_nose_R - the nose bay doors hinge on the line build_bays.py
        #     FITS to the belly under a 4.2 m panel, which drops 0.75 m along its length; the
        #     axis is (0, 0.989, -0.148), 8.5 degrees off level. Every OTHER door's fitted axis
        #     is within 3.1 degrees of level and 0.9985 or better along UE's axis, which is
        #     square.
        #
        # A stale declaration is how a guard stops guarding: resolve_axis FAILS a bone named
        # here that turns out square, and one not named here that turns out raked.
        raked=("nosewheel_steer", "door_nose_L", "door_nose_R"),
        anim_multiplier=_anim_multiplier(),
    )
