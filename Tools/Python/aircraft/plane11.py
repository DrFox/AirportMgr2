"""DA_Aircraft_Plane11 / ABP_Plane11 - the Airbus A350-1000.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. plane6's 777-300ER made Code E a rung; the A350-1000 is the other half of
it - plane9's argument one letter up: same letter, same stands, different aeroplanes. It is
lighter (319 t against 351) and asks less runway (2,750 m against 3,120), so it is the Code E
type a field can take BEFORE it can take the 777. AirportMgr.Content.FieldLengthsCoverTheRoll
pins that as this row's ceiling.

THE SOURCE DRAWING IS ON DISK: plane11/concept/A350-1000.dwg (Airbus's own 3-view), and
plane11/SPEC.md tabulates the model against it: 73.65 m long, 64.69 m span, 17.12 m high against
the published 73.79 / 64.75 / 17.08.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, unlike plane4's. No Airbus A350
Aircraft Characteristics PDF is in plane11/concept; the field lengths, speeds, tiller lock and
fan speed below are commonly quoted figures, rounded in the conservative direction. Replace them
from the AC document when it is to hand.
"""
from build_aircraft_type import AircraftSpec, RigMap

TYPE_NAME = "DA_Aircraft_Plane11"
ABP = "/Game/Aircraft/Plane11/ABP_Plane11"
MESH = "/Game/Aircraft/Plane11/SK_Plane11"
MODELS = r"C:\repos\AirportMgr2Models\plane11"
SOURCE = MODELS + r"\export\plane11.glb"
RIG_MAP = MODELS + r"\scripts\rig_map.json"

# THE SIX MAIN WHEEL BONES, AVERAGED - plane6's reason: the bogie's CENTRE is the point a
# three-axle truck pivots about.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2", "wheel_L3")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2", "wheel_R3")

# --- Published (approximately - see the header) for the A350-1000, Trent XWB-97 -----------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 230, A SHADE ABOVE THE 777's 220: 32 t lighter on similar thrust and a Vr about 15 kn
    # lower.
    "takeoff": dict(accel=230.0, decel=400.0, speed_cap=7710.0),   # Vr about 150 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7453.0),   # Vref about 145 kn
}

# 70 DEGREES of tiller authority, the 777's figure. On the measured ~32.5 m wheelbase that is
# about a 35 m tightest radius, the largest in the game beside the 777's.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 118.0,
}

CLIMB = {
    # 9 DEGREES, NOT THE 777-300ER's 8 - as long an aeroplane, but not the type famous for tail
    # strikes.
    "lift_angle_at_rotate_degrees": 9.0,
    "climb_pitch_degrees": 11.0,
    "rotate_rate_deg_per_sec": 2.5,
    "climb_speed": 12850.0,       # 250 kn below 10,000 ft
    "clear_altitude": 65000.0,    # 650 m, plane6's: a 16 s gear cycle finishes above 500 m
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # About 2,700 rpm at 100 % N1 - a big fan turns slowly, as the GE90's 2,355 does.
    "max_rpm": 2700.0,
    "spool_up_seconds": 10.0,
    "spool_down_seconds": 30.0,
}

# THE GEAR. plane6's cycle for plane6's reason: three-axle bogies into wells between the wing
# box and the belly fairing, behind big doors.
GEAR = {
    "travel_seconds": 12.0,
    "door_seconds": 2.0,
    # ZERO: plane11/SPEC.md - "No bogie bone (the bogie stows untilted)".
    "truck_tilt_seconds": 0.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

# ALSO plane13's CEILING, typed into that test's row: the 757-300 must ask less than this.
REQUIREMENTS = {
    "takeoff_field_length": 275000.0,   # 2,750 m
    "landing_field_length": 210000.0,   # 2,100 m at max landing weight
}

TURNAROUND_SECONDS = 5400.0   # ninety minutes, plane6's: ~370 seats through four doors

# TWENTY-TWO BLADES - the Trent XWB's, and build_fan.py arrays that many.
PROP_BLADE_COUNT = 22


def get_spec():
    return AircraftSpec(
        key="plane11",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="A35K",
        display_name="A350-1000",
        code="E",
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
        main_wheels_l=MAIN_WHEELS_L,
        main_wheels_r=MAIN_WHEELS_R,
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
            # 90, THE CLASS DEFAULT, AND WRITTEN ANYWAY - plane6's argument for its door angle:
            # a default that HAPPENS to be right stops being right the day somebody retunes it.
            gear=lambda d: d["angles"]["GearRetractedAngleDegrees"],
            # 81 closes the doors exactly from their OPEN bind pose.
            door=lambda d: d["angles"]["BayDoorClosedAngleDegrees"],
        ),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
        # NO BONE IS DELIBERATELY RAKED, unlike plane6, plane8 and plane9. rig_map.json puts
        # nosewheel_steer's head (axle) and tail (trunnion) at stations 4.6370 and 4.6366 - a
        # VERTICAL leg, because this one folds aft rather than having to lean past a nose cone.
        # The nose doors' hinge falls 1.7 degrees over its 2.2 m, inside resolve_axis's square
        # band. resolve_axis fails a declared-raked bone that turns out square, so this list
        # stays honest.
        raked=(),
        pushback_need="VEHICLE_TUG",
        pushback_reason=(
            "a 319 t A350-1000 needs the tug the 777 beside it does - authored rather than left to "
            "the class default for the Plane6 row's reason"
        ),
        yard_label="Plane11 (A350-1000)",
    )
