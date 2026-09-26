"""DA_Aircraft_Plane10 / ABP_Plane10 - the Cessna 208B Grand Caravan.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it: a
figure typed here is a second opinion about an object the export already answers for.

WHAT THIS TYPE IS FOR. It is the fleet's UTILITY SINGLE: a Code B span (15.875 m, over the 15 m
line the 172 and the Meridian stay under) on a GRASS strip. Every other Code B type either wants
tarmac (plane5's King Air) or is a twin (plane2's Twin Otter); this one is what a grass field
earns once it is widened for Code B, before it is paved. It asks more runway than the Meridian
(740 m against 510) and less than the King Air (1,006 m).

THE PRIMARY SOURCE IS ON DISK: plane10/concept/Cessna 208 Diagram and Dimensions-Master.PDF, POH
section 1, figure 1-1 and its notes. Geometry and the turning radius come from there. The
PERFORMANCE figures do NOT - those pages are not in the PDF - and are the 208B's commonly quoted
brochure/POH section 5 figures at MTOW, sea level, ISA.

THE GEAR IS FIXED, so this type declares no gear cycle - plane1's ruling.

THE PROPELLER DIAMETER IS NOT MEASURED OFF A BOUNDING BOX, and a 3-BLADE PROP IS WHY - see
`_disc_diameter_by_hub_axis` below. With one blade straight up the box is 1.5 r tall and 1.73 r
wide, so max(width, height) reads 2.244 m for this 2.54 m Hartzell - 12% short. An even blade
count puts two tips on one diameter and the box is right, which is why plane1's two blades and
plane5's four never showed it. Measured 254.0 uu on 2026-09-25, the POH's 100" to the millimetre.
The HUB AXIS is the spinner's box centre across the airframe - a surface of revolution, so its
box centre IS its axis.
"""
import math

from build_aircraft_type import (AircraftSpec, glb_mesh_positions, read_glb,
                                say_published_table, say_tightest_radius)
from airside_import import part_bounds_uu

TYPE_NAME = "DA_Aircraft_Plane10"
ABP = "/Game/Aircraft/Plane10/ABP_Plane10"
MESH = "/Game/Aircraft/Plane10/SK_Plane10"
SOURCE = r"C:\repos\AirportMgr2Models\plane10\export\plane10.glb"

# POH FIGURE 1-1, NOTE 6: minimum turning radius, pivot point to outboard wing tip strobe,
# 33'-8" for 208B0404 and on. The earlier airframes' 32'-8 5/8" is not the one taken - a current
# Grand Caravan is the later serial range.
POH_TURN_RADIUS_UU = (33 * 12 + 8) * 2.54
LOCK_TOLERANCE_DEGREES = 2.0

# --- Published, from the Cessna 208B Grand Caravan ------------------------------------
#
# A 3,969 kg turboprop single on fixed gear. It rotates at 70 kn, between the 172's 55 and the
# King Air's 100, which is where its field length sits too.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 150 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 3598 gives 432 m against the quoted 1,405 ft (428 m).
    "takeoff": dict(accel=150.0, decel=400.0, speed_cap=3598.0),   # Vr 70 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=4009.0),   # Vref 78 kn, flaps full
}

# 60 DEGREES, DERIVED FROM THE POH RATHER THAN QUOTED - see `_poh_lock_check`. 0.2 g as plane2:
# a utility single, not a trainer.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,    # plane2's: a big tail, a heavy single
    "climb_speed": 5346.0,             # 104 kn, Vy at sea level
    "clear_altitude": 30000.0,         # the same circuit as plane1 and plane2
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 900.0,             # plane2's: the eye sits as high over the mains
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # PT6A-114A, PROPELLER rpm behind its reduction gear.
    "max_rpm": 1900.0,
    "spool_up_seconds": 4.0,
    "spool_down_seconds": 9.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 74000.0,   # 740 m, 2,420 ft rounded up
    "landing_field_length": 54000.0,   # 540 m, 1,740 ft rounded up
}

TURNAROUND_SECONDS = 720.0   # twelve minutes, plane5's: between the 172's ten and the Otter's 15

# THREE BLADES - the 100" Hartzell POH figure 1-1 names.
PROP_BLADE_COUNT = 3

PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 15.875),
    ("length", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 12.675),
    ("wheelbase", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 4.051),
    ("track", lambda m: m["main_gear_track"], 3.556),
    ("tailplane", lambda m: m["footprint"]["tailplane_span"], 6.248),
    ("propeller", lambda m: m["prop_diameter"], 2.540),
]


def _disc_diameter_by_hub_axis(prop_lo, prop_hi, spec):
    """Twice the farthest blade vertex's distance from the hub's axis, uu - NOT the bounding
    box; see the module header."""
    doc, binary = read_glb(spec.source)
    hub_lo, hub_hi = part_bounds_uu(doc)[spec.spinner]
    axis_y = (hub_lo[1] + hub_hi[1]) * 0.5
    axis_z = (hub_lo[2] + hub_hi[2]) * 0.5
    farthest = 0.0
    for x, y, z in glb_mesh_positions(doc, binary, spec.prop):
        # glTF -> UE as part_bounds_uu has it: y is UE Z, z is UE Y, metres to uu.
        farthest = max(farthest, math.hypot(z * 100.0 - axis_y, y * 100.0 - axis_z))
    if farthest <= 0.0:
        raise KeyError("no vertices under a mesh named %s in %s" % (spec.prop, spec.source))
    return 2.0 * farthest


def _poh_lock_check(spec, m, say, fail):
    """The nosewheel angle the POH's turning radius implies on THIS model, degrees. The POH
    measures from the turn's pivot to the OUTBOARD wing tip, so the pivot sits (radius - half
    span) off the centreline, on the main axle line; the nosewheel must then point square to the
    line from that pivot: atan(wheelbase / offset)."""
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    offset = POH_TURN_RADIUS_UU - m["footprint"]["wingspan"] * 0.5
    lock = math.degrees(math.atan2(wheelbase, offset))
    want = spec.steering["max_steer_degrees"]
    if abs(lock - want) > LOCK_TOLERANCE_DEGREES:
        fail("the POH's 33'-8\" turn implies %.1f deg of lock on this model, against the %.0f "
             "typed - re-derive STEERING" % (lock, want))
    else:
        say("PASS the POH's 33'-8\" turn implies %.1f deg of lock; %.0f typed" % (lock, want))


def _report(spec, m, say, fail):
    say_published_table(spec, m, say)
    _poh_lock_check(spec, m, say, fail)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane10",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="C208",
        display_name="Cessna 208B Grand Caravan",
        code="B",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        # ONE LEG, NOT THE PAIR: only its HEIGHT is taken, the sanity bound on the wheel radius.
        gear_leg="maingear_L",
        prop="prop",
        # The spinner is a separate mesh skinned to the same `prop` bone; the DISC is the
        # blades', and the spinner gives the hub axis.
        spinner="spinner",
        prop_diameter_fn=_disc_diameter_by_hub_axis,
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
        published_table_label="the POH's",
        report_extra=_report,
        # NOTHING IS RAKED. plane10's build_rig.py stands nosewheel_steer vertical, dir
        # (0, 0, 1). A stale declaration fails both ways, so if a re-export rakes the bone,
        # resolve_axis says so and this tuple gains it.
        raked=(),
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a Caravan reverses off a stand on its own prop - the PT6's reverse pitch - and the "
            "class default of VehicleTug would gate a grass-strip single behind the depot"
        ),
        yard_label="Plane10 (Grand Caravan)",
    )
