"""DA_Aircraft_Plane9 / ABP_Plane9 - the Airbus A320-200 (sharklets).

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it: a
figure typed here is a second opinion about an object the export already answers for.

WHAT THIS TYPE IS FOR. plane4's 737-800 made "the JET" a rung; the A320 is the other half of
Code C, and the pair is the point: same letter, same stands, different aeroplanes. It needs LESS
runway than the 737 (2,100 m against 2,316) and noses 1 m further past its gear (5.08 m against
4.09), which is exactly the pair of facts IcaoCode's Code C row already takes from two different
aeroplanes - MaxTailAft from the 737, MaxNoseFwd from the A320.

WHY THIS TYPE AND NOT DA_Aircraft_A320, which already exists - plane4's reason for not reusing
DA_Aircraft_B738. That one is the PAPER A320: UAircraftType::BuildA320 types its figures from the
datasheet, it carries no mesh, and it is the design aircraft of DA_Stand_CodeC, with the service
points the stand lays its roads to. This type is the aeroplane that flies, and its figures are
MEASURED off SK_Plane9. The two agree where both state a figure: nose 508 against 507, tail
-3249 against -3250.

THE SOURCE DRAWING IS ON DISK: plane9/concept/A320_sharklet-001.dwg (Airbus's own 3-view), and
plane9/SPEC.md tabulates the model against it. The one figure that differs from the brochure is
the SPAN, 35.49 m against 35.80, and that is the drawing's, left unscaled by ruling; it is Code C
either way.
"""
from build_aircraft_type import AircraftSpec, RigMap

TYPE_NAME = "DA_Aircraft_Plane9"
ABP = "/Game/Aircraft/Plane9/ABP_Plane9"
MESH = "/Game/Aircraft/Plane9/SK_Plane9"
MODELS = r"C:\repos\AirportMgr2Models\plane9"
SOURCE = MODELS + r"\export\plane9.glb"
RIG_MAP = MODELS + r"\scripts\rig_map.json"

# --- Published, from the Airbus A320-200 (CFM56-5B, sharklets) ----------------------------
#
# A 78 t narrowbody, the 737-800's peer. It rotates a few knots slower (about 142 against 145)
# and lands slower (about 136 against 140), which is most of why it wants less runway.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as plane4
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=7299.0),   # Vr about 142 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=6990.0),   # Vref about 136 kn
}

# 75 DEGREES of tiller authority - Airbus AC 4-3-0's figure for the A320 family. On the measured
# 12.64 m wheelbase that is about a 13 m tightest radius, tighter than the 737's 16 m.
STEERING = {
    "max_steer_degrees": 75.0,
    "max_lateral_accel_uu": 147.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 9.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 12850.0,       # 250 kn below 10,000 ft
    "clear_altitude": 50000.0,    # 500 m, plane4's
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    "max_rpm": 5000.0,             # the CFM56-5B's real 100 % N1, about 5,000 rpm
    "spool_up_seconds": 8.0,
    "spool_down_seconds": 25.0,
}

# The gear cycle. 8 s is the A320's usual quoted transit; a second of bay door either side, as
# plane4. The MAINS HAVE DOORS on this type (plane4's do not), so the door stage is seen at both
# ends of the aeroplane.
GEAR = {
    "travel_seconds": 8.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 210000.0,   # 2,100 m
    "landing_field_length": 150000.0,   # 1,500 m
}

TURNAROUND_SECONDS = 2400.0   # 180 seats through two doors - plane4's forty minutes

# TWENTY-FOUR BLADES, THE MODEL'S, NOT THE ENGINE'S 36. build_fan.py arrays 24 (at game
# distance the count does not read, the twist does) - what PropStepDegrees resolves against.
PROP_BLADE_COUNT = 24


def get_spec():
    return AircraftSpec(
        key="plane9",
        # The ICAO type designator - the paper type's too. ShortCode is a label, not a key.
        short_code="A320",
        display_name="A320-200",
        code="C",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        gear_leg="maingear_L",
        # The MESH is honestly a fan; its BONE is prop_L so BONE_RULES' "prop" needle finds a
        # driver.
        prop="fan_L",
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "fan %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu, "
                    "span %(wingspan).1f, wing line %(wing_x).1f"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        angles=RigMap(
            RIG_MAP,
            # 86, NOT THE CLASS DEFAULT 90: the main leg's inboard end rises through the 0.32 m
            # wing root past 86. The nose folds 86 too, so one figure serves all three legs.
            gear=lambda d: d["angles"]["GearRetractedAngleDegrees"],
            # 81 closes the doors exactly from their OPEN bind pose.
            door=lambda d: d["angles"]["BayDoorClosedAngleDegrees"],
        ),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
    )
