"""DA_Aircraft_Plane15 / ABP_Plane15 - the Cirrus SR22.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. The SR22 is the FAST end of the four-seat single: 310 hp against the
172's and the Cherokee's 180, a composite airframe, and the same Code A and fixed gear. Like the
Cherokee it is variety at the bottom of the ladder rather than a new tier - but it is the one
light single that asks noticeably MORE runway than its siblings (570 m against the 172's 497 and
the Cherokee's 500), because it comes in faster. Its 11.68 m span sits between the 172 and the
Meridian.

THE SOURCE DRAWINGS ARE ON DISK, but they are WEB drawings, not a maker's dimensioned 3-view:
plane15/concept/*.jpg, and plane15/SPEC.md tabulates the model against the published figures -
span 11.680, length 7.920, height 2.720, all to the millimetre; track 3.29 and prop 1.981 (78")
published; wheelbase 2.289 DRAWN (Cirrus publishes none). SPEC.md's "Disagreements" section says
which drawing won where they differ, and none of that is re-litigated here.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane14's are not. No Cirrus
POH is in plane15/concept; the field lengths and speeds below are the SR22 G6 POH figures as
commonly quoted (3,600 lb gross, sea level, standard day), rounded in the conservative
direction. Replace them from the POH when it is to hand.

THE AEROPLANE IS LEVEL ON ITS GEAR (SPEC.md: `geom.PITCH_DEG = 0`), unlike plane12, so the
footprint is the reference-line length and needs no pitched-plan note.

THE PROPELLER DIAMETER IS NOT MEASURED OFF A BOUNDING BOX, for plane10's reason: three blades,
and a three-blade disc's box is short of its diameter across whichever axis no blade happens to
point along. The prop is plane10's builder with plane2's blade (SPEC.md), so it is measured
plane10's way, by the same function rather than a copy of it.

THE NOSEWHEEL CASTERS. An SR22 has no nosewheel steering at all - it turns on differential
brake and the nose follows - and its rig steers the nose spat about a VERTICAL axis at the leg
foot (SPEC.md: steering about the 34 deg leg would tilt the wheel into the ground). So nothing
is raked, unlike plane12's oleo.

THE GEAR IS FIXED, so this type declares no gear cycle - plane1's ruling.
"""
from build_aircraft_type import AircraftSpec, say_published_table, say_tightest_radius

# ONE MEASUREMENT, NOT TWO COPIES: plane10's hub-axis diameter reads any level multi-blade
# disc, and this prop is plane10's builder.
from aircraft.plane10 import _disc_diameter_by_hub_axis

TYPE_NAME = "DA_Aircraft_Plane15"
ABP = "/Game/Aircraft/Plane15/ABP_Plane15"
MESH = "/Game/Aircraft/Plane15/SK_Plane15"
SOURCE = r"C:\repos\AirportMgr2Models\plane15\export\plane15.glb"

# --- Published (approximately - see the header) for the SR22 G6, IO-550-N ----------------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 200 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 3601 gives 324 m against the quoted 1,082 ft (330 m).
    "takeoff": dict(accel=200.0, decel=400.0, speed_cap=3601.0),   # Vr 70 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=4115.0),   # approach 80 kn, full flap
}

# 30 DEGREES, plane1's EFFECTIVE AUTHORITY. A castering nose has no lock of its own to quote -
# the limit is how hard a pilot brakes one main - and no turning radius is on disk to derive one
# from as plane10's POH did, so the fleet's light-single figure stands.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 5.0,   # plane1's: a light single's elevator
    "climb_speed": 5556.0,            # 108 kn, Vy - the fleet's fastest-climbing single
    "clear_altitude": 30000.0,        # the same circuit as plane1, plane2 and plane12
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 800.0,            # plane12's: a low wing
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE: the IO-550-N turns the propeller itself, so redline IS propeller rpm.
    "max_rpm": 2700.0,
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 57000.0,   # 570 m, 1,868 ft over 50 ft rounded up
    "landing_field_length": 71000.0,   # 710 m, 2,325 ft over 50 ft rounded up
}

TURNAROUND_SECONDS = 600.0   # four seats, two gull-wing doors: ten minutes, plane1's

# THREE BLADES - SPEC.md's 78" Hartzell.
PROP_BLADE_COUNT = 3

PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 11.680),
    ("length", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 7.920),
    ("wheelbase*", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 2.289),
    ("track", lambda m: m["main_gear_track"], 3.290),
    ("propeller", lambda m: m["prop_diameter"], 1.981),
]


def _report(spec, m, say, fail):
    say_published_table(spec, m, say)
    say("  * wheelbase is SPEC.md's DRAWN figure; Cirrus publishes none")
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane15",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="SR22",
        display_name="Cirrus SR22",
        code="A",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        # A ONE-PIECE TAPERED STAB through the tail cone (SPEC.md), and the export names it
        # `stab`.
        stabiliser="stab",
        # ONE LEG, NOT THE PAIR: only its HEIGHT is taken, the sanity bound on the wheel radius.
        gear_leg="maingear_L",
        prop="prop",
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
        published_table_label="SPEC.md's",
        report_extra=_report,
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "an SR22 is pulled off a stand by hand with a tow bar on its castering nosewheel - "
            "plane1's reason, and the class default of VehicleTug would gate a four-seat single "
            "behind the depot"
        ),
        yard_label="Plane15 (Cirrus SR22)",
    )
