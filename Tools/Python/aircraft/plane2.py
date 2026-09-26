"""DA_Aircraft_Plane2 / ABP_Plane2 - the DHC-6 Twin Otter.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, and the split is the point. Every dimension is
measured AT RUN TIME off the export's own parts, because those describe the model that is
actually drawn, and a figure typed from a spec sheet would be a second opinion about the same
object. They were literals here until 2026-09-13, when the export moved its origin to the nose
gear and made all four position figures wrong at once. The speeds and distances come from the
DHC-6 Twin Otter the model is based on, because nothing in the mesh knows how fast it rotates.

THE WHEEL RADIUS MATTERS TWICE, and is easy to set in only one place. UAircraftType carries it
for the model; UAirsideAgentAnim carries its own copy for the animation, defaulted to the
Meridian's 21 uu. A Twin Otter wheel measures 68.6 uu, so an unset ABP spins the wheels at over
three times the right rate against ground speed - which reads as an aircraft skating rather than
rolling. Both are set from the one measurement.

THIS IS THE OLDEST SCRIPT IN THE FLEET, and the shared mechanism closes three gaps that
chronology left in it: `main_gear_track` was measured off the rig's own wheel_L/wheel_R bones
by every later aircraft but never asked of this one (author_type() wrote `fixed_axle_x` and
`propeller_diameter` onto the asset but nothing ever read either back), and the fixed gear was
never asserted the way plane1's is. None of the three is a new fact about the Twin Otter - the
bones and the fixed gear were always there - only a gap in how loudly a broken write would have
been caught.
"""
from build_aircraft_type import AircraftSpec

TYPE_NAME = "DA_Aircraft_Plane2"
ABP = "/Game/Aircraft/Plane2/ABP_Plane2"
MESH = "/Game/Aircraft/Plane2/SK_Plane2"
SOURCE = r"C:\repos\AirportMgr2Models\plane2\export\plane2.glb"

# --- Published, from the DHC-6 Twin Otter ---------------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# The Twin Otter is a STOL aeroplane and these are what make it one. Against the Meridian
# already in the project: it rotates at 60 kn where the Meridian needs 86, and climbs out at 85
# against 121. That is the whole character of the type - it gets off the ground early and
# slowly, and it is why it can use a strip the Meridian cannot.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the Meridian
    "takeoff": dict(accel=450.0, decel=400.0, speed_cap=3100.0),   # Vr about 60 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=3600.0),   # threshold about 70 kn
}

# STEERING, published rather than measured - nothing in the mesh knows how far the tiller turns
# or how hard a pilot will corner. Both are read only by the rolling-steer law, which this type
# uses because it carries axle figures.
#
# 60 degrees of lock on the measured 4.54 m wheelbase makes the tightest followable radius 5.2 m,
# well inside any taxiway bend. 0.2 g is between the Meridian's 0.25 and an airliner's 0.15: a
# Twin Otter on a quiet apron corners harder than a jet and less hard than a single.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,
    "climb_speed": 4400.0,        # 85 kn, the Twin Otter's best-rate speed
    "clear_altitude": 30000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 900.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    "max_rpm": 2200.0,            # PT6A-27, propeller rpm
    "spool_up_seconds": 4.0,
    "spool_down_seconds": 9.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes. The
# take-off roll these figures produce is about 17,800 uu (178 m), from FTakeoffRun::RequiredRoll.
# The values are the real aeroplane's: 1,200 ft to fifty feet on take-off and 1,050 ft landing -
# against the Meridian's 510 m and 400 m, the short field the type is famous for.
REQUIREMENTS = {
    "takeoff_field_length": 36600.0,   # 366 m
    "landing_field_length": 32000.0,   # 320 m
}

TURNAROUND_SECONDS = 900.0   # a 19-seat commuter turns in fifteen minutes, not thirty

# THREE BLADES - a Hamilton Standard/Hartzell three-blade prop is this airframe's usual fit, and
# is what the class default (3) already assumes; set explicitly rather than left implicit, so
# the ABP's blade count is authored and verified for this aircraft as it is for the other
# thirteen, not a value nobody wrote down.
PROP_BLADE_COUNT = 3


def get_spec():
    return AircraftSpec(
        key="plane2",
        type_name=TYPE_NAME,
        short_code="DHC6",
        display_name="DHC-6 Twin Otter",
        # Code B: the aerodrome reference letter. It shares the letter with plane5's King Air.
        code="B",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        # NO SEPARATE LEG MESH ON THIS RIG - the gear is a strut/nacelle assembly with nothing
        # named as one object, so the original script never had a leg-height sanity bound to
        # check the wheel radius against. `None` says the same thing here.
        gear_leg=None,
        prop="prop_L",
        # This is the one aircraft whose rig line never grew a wheelbase or a track phrase -
        # kept exactly as authored rather than backfilled with words nobody wrote.
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "fixed axle %(fixed_axle_x).1f uu"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=None,   # A DHC-6's gear is fixed - see the header.
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="GRASS",   # the type's whole point is strips that are neither paved nor lit.
        angles=None,
        # HAND-WIRED BEFORE THIS TOOLING EXISTED - no per-key wiring script, no measured
        # axis plan. See AircraftSpec.wire_script.
        wire_script=False,
    )
