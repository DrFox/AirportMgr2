"""DA_Aircraft_Plane17 / ABP_Plane17 - the Piper PA-34-200 Seneca I.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. The SECOND PISTON TWIN, and the lighter one: two 200 hp Lycomings on an
11.85 m wing, 4,200 lb against the Baron's 5,500. It stands on the same Code A stand as the
Baron but asks less of a field (500 m against 710), so it is the twin a short grass strip
takes first - the Cherokee's maker, one step up.

THE SOURCE DRAWING IS ON DISK AND IS PIPER'S OWN: plane17/concept/drawing.png is the
dimensioned three-view, and plane17/SPEC.md tabulates the model against its arrows and says
which view won where they disagree (fin stretched to the photos, nacelles on the 12'6" label).
None of that is re-litigated here.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane14's, plane15's and
plane16's are not. concept/ holds the three-view and photos only; the field lengths and speeds
below are the Seneca I figures as commonly quoted (4,200 lb gross, sea level, standard day),
rounded in the conservative direction. Replace them from the POH performance section when it
is to hand.

THE AEROPLANE SITS LEVEL ON ITS GEAR (SPEC.md: the drawing's ground line is level), unlike
plane12 and plane16, so the bbox length IS the reference-line length. The propeller diameter
still goes through plane12's centroid measure: a two-blade disc's box reads whatever angle
the blades were left at, which is plane10's reason in its sharpest form.

THE PROPS COUNTER-ROTATE (SPEC.md: the Seneca I is the L/LIO-360 pair, and prop_R is prop_L
MIRRORED). Both bones point the same way along the thrust line, so the fleet's one -1 on both
would spin the mirrored right-hand blades trailing-edge first. `anim_multiplier` flips prop_R
alone; it is the first rig in the fleet to need a per-bone override that is not plane8's gear.

NO rig_map.json FOR THIS MODEL - plane17's rig is plane16's shape (SPEC.md), with the mains
folding INBOARD rather than forward and no main bay doors, and like plane16's its angles are
constants in build_rig.py; `RigScriptLine` reads that line rather than retyping it.
"""
from build_aircraft_type import (AircraftSpec, RigScriptLine, say_published_table,
                                 say_tightest_radius)

# ONE MEASUREMENT, NOT TWO COPIES: plane12's centroid diameter reads a disc of any blade count
# at any blade angle.
from aircraft.plane12 import _disc_diameter_by_centroid

TYPE_NAME = "DA_Aircraft_Plane17"
ABP = "/Game/Aircraft/Plane17/ABP_Plane17"
MESH = "/Game/Aircraft/Plane17/SK_Plane17"
MODELS = r"C:\repos\AirportMgr2Models\plane17"
SOURCE = MODELS + r"\export\plane17.glb"
RIG_SCRIPT = MODELS + r"\scripts\build_rig.py"

# --- Published (approximately - see the header) for the Seneca I, IO-360-C1E6 / LIO-360 --
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 244 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 3858 gives 305 m against the quoted 1,000 ft (305 m).
    "takeoff": dict(accel=244.0, decel=400.0, speed_cap=3858.0),   # Vr 75 kn, over Vmc 70
    "landing": dict(accel=100.0, decel=400.0, speed_cap=4373.0),   # approach 85 kn
}

# 30 DEGREES, plane1's EFFECTIVE AUTHORITY, plane16's reason: the nosewheel steers off the
# rudder pedals and differential power does the rest, and nothing on disk gives a turning
# radius to derive a lock from.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,   # plane16's: a light twin, between singles and jets
    "climb_speed": 4681.0,            # 91 kn, two-engine Vy (105 mph)
    "clear_altitude": 30000.0,        # the light singles' circuit
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 800.0,            # plane12's: a low wing
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE: the IO-360 turns the propeller itself, so redline IS propeller rpm.
    "max_rpm": 2700.0,
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

# THE GEAR. Mains fold INBOARD into the wing root, nose folds FORWARD (SPEC.md; the trunnions
# are INVENTED). 6 s of travel, plane14's and plane16's light-aeroplane figure. Only the nose
# has bay-door bones; the strut doors ride the main legs, so the door stage is seen at the
# nose alone.
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 43000.0,   # 430 m = model roll 387 m x 1.1; published 500 m, ~1,600 ft over 50 ft rounded up
    "landing_field_length": 44000.0,   # 440 m = model landing x1.25 399 m x 1.1; published 580 m, ~1,900 ft over 50 ft rounded up
}

TURNAROUND_SECONDS = 720.0   # plane16's: six seats, front door and aft door, plus fuel

# TWO BLADES a side - SPEC.md's 6'4" props, plane2's blade x2.
PROP_BLADE_COUNT = 2

PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 11.852),
    ("length", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 8.712),
    ("wheelbase", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 2.137),
    ("track", lambda m: m["main_gear_track"], 3.375),
    ("stabiliser", lambda m: m["footprint"]["tailplane_span"], 4.133),
    ("propeller", lambda m: m["prop_diameter"], 1.930),
]


def _report(spec, m, say, fail):
    say_published_table(spec, m, say)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane17",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="PA34",
        display_name="Piper PA-34 Seneca",
        code="A",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stab",
        # ONE LEG, NOT THE PAIR: only its HEIGHT is taken, the sanity bound on the wheel radius.
        gear_leg="maingear_L",
        prop="prop_L",
        prop_diameter_fn=_disc_diameter_by_centroid,
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "prop %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu, "
                    "span %(wingspan).1f, wing line %(wing_x).1f"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        angles=RigScriptLine(
            RIG_SCRIPT, r"^GEAR_DEG, DOOR_DEG = ([0-9.]+), ([0-9.]+)",
            gear_group=1, door_group=2),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="GRASS",
        published_table=PUBLISHED_TABLE,
        published_table_label="SPEC.md's",
        report_extra=_report,
        # THE NOSE OLEO, MEASURED 20.2 DEG OFF VERTICAL in UE on 2026-09-27: build_rig.py lays
        # nosewheel_steer from the leg crown to the nose pivot, and the Seneca's nose leg rakes
        # forward to its forward-folding trunnion. plane6's ruling: a nose leg steers about its
        # STRUT, so the rake is right, not a rigging fault. A stale declaration fails both ways.
        raked=("nosewheel_steer",),
        # COUNTER-ROTATION - see the header. +1 undoes the fleet's -1 on the one mirrored prop.
        anim_multiplier={"prop_R": 1.0},
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a Seneca turns out of a stand on differential power and brake, plane16's reason - "
            "the class default of VehicleTug would gate a light twin behind the depot"
        ),
        yard_label="Plane17 (PA-34 Seneca)",
    )
