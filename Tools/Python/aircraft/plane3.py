"""DA_Aircraft_Plane3 / ABP_Plane3 - the de Havilland Canada Dash 8-Q400.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as plane2's states it and for the same
reason: a figure typed here is a second opinion about an object the export already answers for.

WHAT THIS TYPE IS FOR, beyond being another aeroplane. plane2 is the STOL end of the range:
grass, 366 m, 19 seats. The Dash 8-Q400 is the step up that makes a runway a DECISION - it wants
tarmac and 1,402 m, nearly four times plane2's take-off field length, so an airport that can take
one is an airport that was built for one. The progression is the content, and RunwayAdmission is
what enforces it.

THE WHEEL RADIUS COMES OFF THE RIG, NOT OFF THE TYRE. The gear used to be one object per leg -
gearRear_L was tyre AND strut, 237 uu tall against a tyre nearer 90 - so a bounding box would
have claimed a wheel two and a half times too big and spun it at two fifths of the right rate.
The gear was separated on 2026-09-19 (three wheels, three legs), so a tyre box is now available,
but this still reads the BONE: build_export.py puts each wheel's origin ON its rotation axis,
and the hub's height above the contact plane IS the radius. The leg's box stays as a sanity
bound - a radius may not exceed the leg that carries it.

NO PROP_BLADE_COUNT WAS EVER AUTHORED FOR THIS TYPE. The Q400's Dowty R408 is a six-blade prop
and the ABP's class default is 3 - a real content gap, not a mechanism one, and outside this
refactor's scope (Issue #290 is about the SCRIPT, not about re-deriving the Q400's blade count).
Left at the dataclass default (3) here, matching the behaviour this aircraft has always had;
worth its own follow-up.
"""
from build_aircraft_type import AircraftSpec, Typed

TYPE_NAME = "DA_Aircraft_Plane3"
ABP = "/Game/Aircraft/Plane3/ABP_Plane3"
MESH = "/Game/Aircraft/Plane3/SK_Plane3"
SOURCE = r"C:\repos\AirportMgr2Models\plane3\export\plane3.glb"

# --- Published, from the de Havilland Canada Dash 8-Q400 ------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# A 29.5 t turboprop airliner, and every figure below is what separates it from the two light
# types already in the project. It rotates at 120 kn where plane2 rotates at 60, and climbs out
# at 170 against 85 - so it leaves the ground later, faster, and needs a strip plane2 would
# regard as an aerodrome in its own right.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=6168.0),   # Vr about 120 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=6425.0),   # Vref about 125 kn
}

# STEERING, published rather than measured.
#
# 70 DEGREES IS THE TILLER'S AUTHORITY, not a comfortable taxi angle, and on the measured
# 14.05 m wheelbase it makes the tightest followable radius about 15 m. That is a REQUIREMENT
# ON THE AIRPORT and the first thing to suspect if a Q400 refuses a route plane2 takes happily.
#
# 0.15 g is the airliner end of the range - plane2 corners at 0.2, the Meridian at 0.25.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 147.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 8738.0,        # 170 kn, the Q400's climb speed
    "clear_altitude": 40000.0,    # 400 m; it leaves the circuit higher than the light types
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    "max_rpm": 1020.0,            # PW150A driving a Dowty R408: 1,020 prop rpm at 100% Np
    "spool_up_seconds": 5.0,
    "spool_down_seconds": 10.0,
}

# THE GEAR, NEW ON 2026-09-21. plane3 shipped with a FIXED undercarriage for three months - not
# as a decision but because the model had no retract bones. build_gear_rig.py added three legs
# and four doors and build_gear_bays.py cut the bays, and this block is what makes the model's
# cycle run.
#
# EIGHT SECONDS, between plane5's six and plane4's seven-and-the-737's-bulk. A Dash 8's gear is
# electrically selected and hydraulically actuated with a published VLO of 200 KIAS. 1.5 SECONDS
# A DOOR, so a full cycle is 11.
GEAR = {
    "travel_seconds": 8.0,
    "door_seconds": 1.5,
    # ZERO, AND IT IS A FACT ABOUT THE AEROPLANE. A Q400 main leg carries a single axle with two
    # wheels side by side - no bogie beam, no tilt actuator, and no bone for one.
    "truck_tilt_seconds": 0.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

# THE DOOR ANGLE. Ninety degrees, where UAirsideAgentAnim defaults to plane4's measured 81.
# plane3/scripts/gear_pivots.json keys door_open_deg at 90.0, and build_gear_rig.py's
# pose_check() reports where each closed door's outer face lands against the skin it was
# measured off. Left at 81 the four doors stop 9 degrees short of shut.
BAY_DOOR_CLOSED_ANGLE_DEGREES = 90.0

# PUBLISHED FIELD LENGTHS, at MTOW, sea level, ISA.
REQUIREMENTS = {
    "takeoff_field_length": 140200.0,   # 1,402 m
    "landing_field_length": 128700.0,   # 1,287 m
}

TURNAROUND_SECONDS = 1500.0   # 78 seats through one airstair door: twenty-five minutes


def get_spec():
    return AircraftSpec(
        key="plane3",
        type_name=TYPE_NAME,
        short_code="DH8D",
        display_name="Dash 8-Q400",
        # Code C: the measured span is 28.2 m and Code C runs to 36. It shares the letter with
        # the A320 and the 737, which is exactly why ShortCode exists beside it.
        code="C",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing2",
        stabiliser="stabiliser",
        gear_leg="gearRear_L",
        prop="prop_L",
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "fixed axle %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f)"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        gear_checked_early=True,   # this spec's own verify() ordering, kept as authored
        # No gear_retracted angle authored - the class default (90) is this rig's own fold too.
        angles=Typed(door=BAY_DOOR_CLOSED_ANGLE_DEGREES),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        surface="TARMAC",
        # NOTHING ON THIS RIG IS DECLARED RAKED, AND THE FIRST DRAFT OF THIS LINE GOT IT WRONG.
        #
        # plane3's door hinges ARE slightly off-axis - they are fitted to the bay edge they
        # seal, and a fuselage belly curves - so all four were listed here on the strength of
        # the axes gear_pivots.json publishes. Then they were measured against the threshold
        # rather than against the word "raked":
        #
        #     door_main_L  axis (0, -0.99988, -0.01545)   0.89 deg off
        #     door_nose_L  axis (0,  0.99117, -0.1326 )   7.62 deg off
        #     resolve_axis's bound                        8.11 deg (squareness 0.99)
        #
        # Both are INSIDE it, so the strict check passes them unaided and a declaration would
        # have failed as stale - which is exactly what resolve_axis's raked-but-square branch
        # is for, and it caught this before the script was ever run. plane6's three bones at
        # 11.8 and 12.4 degrees remain the only ones in the fleet that need naming.
        #
        # THE NOSE DOORS ARE HALF A DEGREE INSIDE THE BOUND, which is worth knowing: re-fit
        # those hinges to a slightly deeper belly curve and they cross it, and the failure will
        # read as a broken rig rather than as a tolerance. The answer then is to declare them
        # here, not to widen the bound.
        raked=(),
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a Q400 turns out of a regional stand on its own props; the depot is the jets' tax"
        ),
        yard_label="Plane3 (Dash 8-Q400)",
    )
