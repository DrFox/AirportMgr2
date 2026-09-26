"""DA_Aircraft_Plane12 / ABP_Plane12 - the Piper PA-28-180 Cherokee.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. The Piper PA-28-180 Cherokee is plane1's LOW-WING TWIN: the same
four-seat, 180 hp, fixed-gear trainer class as the 172, the same Code A, the same grass. It is
NOT a new capability tier and does not pretend to be one - it is variety at the bottom of the
ladder, the way the 172 was variety beside the Meridian. What it does differently is SHAPE:
9.14 m of span against the 172's 11.00, so it is the narrowest aeroplane in the game.

THE PRIMARY SOURCE IS ON DISK: plane12/concept/drawing.png, Piper's three-view. Span, length,
stabilator, track, wheelbase and propeller come from its dimensions (inches). PERFORMANCE does
NOT - the drawing carries none - and is the PA-28-180 owner's handbook figures as commonly
quoted, at 2,400 lb gross, sea level, standard day.

THE AEROPLANE SITS 5 DEGREES NOSE-UP ON ITS GEAR, and the mesh is exported that way:
plane12/README.md "The drawing, and the static attitude". Nothing here corrects for it. The
footprint is a PLAN shadow of the pitched mesh, and it is LONGER than the drawing's 282":
nose-up swings the 2.27 m fin top about 0.2 m aft (2.27 sin 5), so the plan length measured
7.271 m on 2026-09-25 against 7.163 along the reference line. The published table prints both
and does not call the difference an error.

THE PROPELLER DIAMETER IS NOT MEASURED OFF A BOUNDING BOX - see `_disc_diameter_by_centroid`
below. plane10's three blades read 12% short in a box; this one has two, which a box would get
right if the disc stood square - but it does not, because the whole aeroplane is pitched 5
degrees, so the disc is tilted and its box height is D*cos(5). The CENTROID of a symmetric blade
set lies on the hub axis in the disc plane, and the 3D distance from it ignores which way the
disc faces.

THE GEAR IS FIXED, so this type declares no gear cycle - plane1's ruling.
"""
import math

from build_aircraft_type import (AircraftSpec, glb_mesh_positions, read_glb,
                                say_published_table, say_tightest_radius)

TYPE_NAME = "DA_Aircraft_Plane12"
ABP = "/Game/Aircraft/Plane12/ABP_Plane12"
MESH = "/Game/Aircraft/Plane12/SK_Plane12"
SOURCE = r"C:\repos\AirportMgr2Models\plane12\export\plane12.glb"

# --- Published, from the Piper PA-28-180 Cherokee ----------------------------------------
#
# The 172's class, and within a few knots of it everywhere: it rotates at 52 kn against 55 and
# climbs at the same 74.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 160 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 2673 gives 223 m against the quoted 720 ft (219 m).
    "takeoff": dict(accel=160.0, decel=400.0, speed_cap=2673.0),   # Vr 52 kn (60 mph)
    "landing": dict(accel=100.0, decel=400.0, speed_cap=3341.0),   # approach 65 kn (75 mph)
}

# 30 DEGREES, plane1's EFFECTIVE AUTHORITY, for plane1's reason: a PA-28 steers its nosewheel
# off the rudder pedals and turns the rest on differential brake, and the drawing gives no
# turning radius to derive one from as plane10's POH did.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 5.0,   # plane1's: a light elevator - here a stabilator
    "climb_speed": 3804.0,            # 74 kn (85 mph), best rate
    "clear_altitude": 30000.0,        # the same circuit as plane1 and plane2
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 800.0,            # plane1's, and lower if anything: a low wing
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE: the O-360-A3A turns the propeller itself, so redline IS propeller rpm.
    "max_rpm": 2700.0,
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 50000.0,   # 500 m, 1,625 ft rounded up
    "landing_field_length": 36000.0,   # 360 m, 1,150 ft rounded up
}

TURNAROUND_SECONDS = 600.0   # four seats and ONE door: ten minutes, plane1's

# TWO BLADES - the drawing's 76" propeller for the -180.
PROP_BLADE_COUNT = 2

PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 9.144),
    ("length*", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 7.163),
    ("wheelbase", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 1.897),
    ("track", lambda m: m["main_gear_track"], 3.048),
    ("stabilator", lambda m: m["footprint"]["tailplane_span"], 3.048),
    ("propeller", lambda m: m["prop_diameter"], 1.930),
]


def _disc_diameter_by_centroid(prop_lo, prop_hi, spec):
    """Twice the farthest blade vertex's distance from the blades' own centroid, uu - NOT the
    bounding box; see the module header."""
    doc, binary = read_glb(spec.source)
    points = list(glb_mesh_positions(doc, binary, spec.prop))
    if not points:
        raise KeyError("no vertices under a mesh named %s in %s" % (spec.prop, spec.source))
    centre = [sum(p[k] for p in points) / len(points) for k in range(3)]
    farthest = max(math.sqrt(sum((p[k] - centre[k]) ** 2 for k in range(3))) for p in points)
    return 2.0 * farthest * 100.0


def _report(spec, m, say, fail):
    say_published_table(spec, m, say)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane12",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="P28A",
        display_name="Piper PA-28-180 Cherokee",
        code="A",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        # STABILATOR, NOT STABILISER - the PA-28's all-moving tailplane, and the export names it
        # for what it is.
        stabiliser="stabilator",
        # ONE LEG, NOT THE PAIR: only its HEIGHT is taken, the sanity bound on the wheel radius.
        gear_leg="maingear_L",
        prop="prop",
        prop_diameter_fn=_disc_diameter_by_centroid,
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "prop %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu, "
                    "span %(wingspan).1f, wing line %(wing_x).1f"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=None,   # THE GEAR IS FIXED - see the header.
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="GRASS",
        angles=None,
        published_table=PUBLISHED_TABLE,
        published_table_label="the drawing's",
        report_extra=_report,
        # THE NOSE OLEO, MEASURED 8.7 DEG OFF VERTICAL in UE on 2026-09-25 - its drawn rake
        # plus the aeroplane's 5.02 deg static pitch - just past resolve_axis's ~8 deg square
        # band. plane6's ruling: a nose leg steers about its STRUT, so the rake is right, not
        # a rigging fault. The prop is pitched the same 5.02 and stays inside the band. A
        # stale declaration fails both ways.
        raked=("nosewheel_steer",),
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a Cherokee is pulled off a stand by hand with a tow bar on the nosewheel - plane1's "
            "reason, and the class default of VehicleTug would gate a trainer behind the depot"
        ),
        yard_label="Plane12 (PA-28-180 Cherokee)",
    )
