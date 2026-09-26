"""DA_Aircraft_Plane13 / ABP_Plane13 - the Boeing 757-300.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. It is the FIRST CODE D aeroplane, and the rung between C and E. Until it,
a field went from the 737/A320's 36 m to the 777/A350's 65 m in one step; the 757-300's 38.05 m
wing is two metres too wide for a Code C stand, so it is the type that makes a Code D stand worth
drawing. It must also ask less runway than the lighter Code E twin, the A350-1000 (2,750 m) -
otherwise a field could skip D entirely and nothing on the ladder would be lost.
AirportMgr.Content.FieldLengthsCoverTheRoll pins that as this row's ceiling.

THE SOURCE DRAWING IS ON DISK: plane13/concept/757-300.dxf (Boeing's own 3-view), and
plane13/SPEC.md tabulates the model against it: span 38.051 (38.05), length 54.72 (54.43 - the
drawing's side and top views disagree on the tail by 0.7 m, and the model keeps the top view's),
height 13.45 (13.56).

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane11's are not. No Boeing
757 ACAP document is in plane13/concept; the field lengths, speeds, tiller lock and fan speed
below are commonly quoted figures, rounded in the conservative direction. Replace them from
Boeing D6-58327 (757 Airplane Characteristics) when it is to hand.
"""
from build_aircraft_type import AircraftSpec, RigMap

TYPE_NAME = "DA_Aircraft_Plane13"
ABP = "/Game/Aircraft/Plane13/ABP_Plane13"
MESH = "/Game/Aircraft/Plane13/SK_Plane13"
MODELS = r"C:\repos\AirportMgr2Models\plane13"
SOURCE = MODELS + r"\export\plane13.glb"
RIG_MAP = MODELS + r"\scripts\rig_map.json"

# THE FOUR MAIN WHEEL BONES, AVERAGED - plane6's reason: the bogie's CENTRE is the point a
# multi-axle truck pivots about. The 757's is the fleet's first TWO-axle bogie, and the centre
# is between the axles, where no bone is.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2")

# --- Published (approximately - see the header) for the 757-300, PW2037 ---------------------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 240, BETWEEN THE A350's 230 AND THE NARROWBODIES' 250 - the -300 is the 757's heaviest
    # stretch (124 t) on the lower-thrust PW2037.
    "takeoff": dict(accel=240.0, decel=400.0, speed_cap=7453.0),   # Vr about 145 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7196.0),   # Vref about 140 kn
}

# 65 DEGREES of tiller authority, the figure commonly quoted for the 757. On the measured
# ~22.4 m wheelbase that is about a 25 m tightest radius, between the 737's 16 and the A350's 35.
STEERING = {
    "max_steer_degrees": 65.0,
    "max_lateral_accel_uu": 137.0,
}

CLIMB = {
    # 8 DEGREES, THE 777-300ER's, NOT THE NARROWBODIES' 9 - the -300 was given a tail skid for
    # tail strikes.
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 12.0,      # the A320's: a 757 climbs steeply
    "rotate_rate_deg_per_sec": 2.5,
    "climb_speed": 12850.0,           # 250 kn below 10,000 ft
    "clear_altitude": 50000.0,        # 500 m, plane4's and plane9's
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # About 4,500 rpm at 100 % N1 - a 2.0 m fan, between the CFM56's 5,000 and the Trent XWB's
    # 2,700.
    "max_rpm": 4500.0,
    "spool_up_seconds": 9.0,
    "spool_down_seconds": 28.0,
}

# THE GEAR. Four-wheel bogies folding inboard into the lower lobe, with main and nose doors:
# 10 s of travel, between the A320's 8 and the widebodies' 12, and 1.5 s of door.
GEAR = {
    "travel_seconds": 10.0,
    "door_seconds": 1.5,
    # ZERO: plane13/SPEC.md's rig has no truck bone, so the bogie stows untilted.
    "truck_tilt_seconds": 0.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

# The commonly quoted 757-300 take-off figure is about 2,550 m on the higher-thrust engines;
# 2,650 allows for the PW2037. Must stay under plane11's 2,750 - the first Code D rung has to
# come before E.
REQUIREMENTS = {
    "takeoff_field_length": 265000.0,   # 2,650 m
    "landing_field_length": 180000.0,   # 1,800 m at max landing weight
}

TURNAROUND_SECONDS = 3000.0   # fifty minutes: ~250 seats through two doors

# THIRTY-SIX BLADES - the PW2037's, and build_fan.py arrays that many.
PROP_BLADE_COUNT = 36


def get_spec():
    return AircraftSpec(
        key="plane13",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="B753",
        display_name="757-300",
        code="D",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        # Plain tips, no winglets (SPEC.md): the span is the wing object's.
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
            # 90, THE CLASS DEFAULT, AND WRITTEN ANYWAY - plane6's argument: a default that
            # HAPPENS to be right stops being right the day somebody retunes it.
            gear=lambda d: d["angles"]["GearRetractedAngleDegrees"],
            # 81 closes the doors exactly from their OPEN bind pose.
            door=lambda d: d["angles"]["BayDoorClosedAngleDegrees"],
        ),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
    )
