"""DA_Aircraft_Plane18 / ABP_Plane18 - the Saab 340B.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. The REGIONAL TURBOPROP between the King Air and the Q400. Two 1,750 shp
CT7-9Bs, 34 seats, a 21.44 m wing - Code B, where the Q400's 28.4 m is Code C - on 1,300 m of
tarmac against the King Air's 1,006 and the Q400's 1,402. It is the first airliner a paved Code B
field can take: paving buys the King Air, and the same runway, a little longer, buys a 34-seater
without the widening the Q400 would also ask for.

THE SOURCE DRAWINGS ARE ON DISK BUT ARE NOT A MAKER'S DIMENSIONED THREE-VIEW: plane18/concept holds
a side view, a top/front drawing and photos, all hand-traced at ~30 px/m. plane18/SPEC.md
tabulates the model against the published figures and says which view won where they disagree
(nacelle tail, prop station, stab shape). None of that is re-litigated here.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane14's through plane17's are
not. concept/ holds drawings and photos only; the field lengths and speeds below are the 340B
figures as commonly quoted (29,000 lb MTOW, sea level, ISA), rounded in the conservative
direction. Replace them from the AFM performance section when it is to hand.

THE AEROPLANE SITS LEVEL ON ITS GEAR (SPEC.md: `geom.world` is identity), plane17's case, so the
bbox length IS the reference-line length. The propeller diameter goes through plane12's centroid
measure all the same: a four-blade disc's box reads short by up to cos 45 at whatever angle the
blades were left at, which is plane10's reason with more blades.

THE PROPS CO-ROTATE (SPEC.md: prop_R is prop_L TRANSLATED, not mirrored - both CT7s turn the same
way), so the fleet's one -1 on both is right and no `anim_multiplier` override is needed. That is
the difference from plane17, whose right prop is a mirror.

NO rig_map.json FOR THIS MODEL - plane18's rig is plane17's shape (SPEC.md: its pipeline is
plane17's, forked) with the mains folding FORWARD into the nacelles and four main bay doors
added; like plane17's its angles are constants in build_rig.py, and `RigScriptLine` reads that
line rather than retyping it.
"""
from build_aircraft_type import (AircraftSpec, RigScriptLine, say_published_table,
                                 say_tightest_radius)

# ONE MEASUREMENT, NOT TWO COPIES: plane12's centroid diameter reads a disc of any blade count
# at any blade angle.
from aircraft.plane12 import _disc_diameter_by_centroid

TYPE_NAME = "DA_Aircraft_Plane18"
ABP = "/Game/Aircraft/Plane18/ABP_Plane18"
MESH = "/Game/Aircraft/Plane18/SK_Plane18"
MODELS = r"C:\repos\AirportMgr2Models\plane18"
SOURCE = MODELS + r"\export\plane18.glb"
RIG_SCRIPT = MODELS + r"\scripts\build_rig.py"

# --- Published (approximately - see the header) for the Saab 340B, CT7-9B / Dowty R354 ------
#
# Speeds are uu per second: a knot is 51.4 uu/s.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 200 uu/s^2 - between the Q400's 250 and a heavier roll, 13 t on 3,500 shp. Vr^2 / 2a with
    # Vr = 5401 gives a 729 m ground roll, inside the 1,300 m field length over 35 ft.
    "takeoff": dict(accel=200.0, decel=400.0, speed_cap=5401.0),   # Vr about 105 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=5916.0),   # Vref about 115 kn
}

# 60 DEGREES, the tiller's authority - the Q400's is 70 on twice the wheelbase. On the measured
# 7.16 m wheelbase the tightest followable radius is printed below by say_tightest_radius; the
# lateral figure sits between the Q400's airliner 0.15 g and the King Air's 0.2.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 170.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 3.0,   # the Q400's: an airliner, not a light twin
    "climb_speed": 7973.0,            # 155 kn
    "clear_altitude": 40000.0,        # 400 m, the retracting types' circuit
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 900.0,            # the King Air's: a low wing, between plane12's and the Q400's
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # PROPELLER RPM, NOT ENGINE RPM: the CT7-9B's gearbox turns the R354 at 1,384 rpm at 100% Np.
    "max_rpm": 1384.0,
    "spool_up_seconds": 5.0,          # the Q400's: a free turbine
    "spool_down_seconds": 10.0,
}

# THE GEAR. Mains fold FORWARD into the nacelles, nose folds FORWARD (SPEC.md; the trunnions are
# INVENTED). SEVEN SECONDS, between the King Air's six and the Q400's eight. Doors at both ends,
# as the King Air and the Q400, so 1.5 s a door - the Q400's figure - and a 10 s full cycle.
GEAR = {
    "travel_seconds": 7.0,
    "door_seconds": 1.5,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 97000.0,   # 970 m = model roll 880 m x 1.1; published 1,300 m at MTOW
    "landing_field_length": 78000.0,   # 780 m = model landing x1.25 704 m x 1.1; published 1,050 m, ~1,030 quoted, rounded up
}

TURNAROUND_SECONDS = 1200.0   # 34 seats through one forward airstair door: twenty minutes

# FOUR BLADES a side - SPEC.md's 11 ft Dowty R354, plane2's blade x4.
PROP_BLADE_COUNT = 4

# Published figures only; SPEC.md's track and stab span are model measurements, not published,
# so they are not a check.
PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 21.44),
    ("length", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 19.73),
    ("wheelbase", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 7.14),
    ("propeller", lambda m: m["prop_diameter"], 3.353),
]


def _report(spec, m, say, fail):
    say_published_table(spec, m, say)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane18",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="SF34",
        display_name="Saab 340B",
        # Code B: measured span 21.44 m, Code B runs to 24 - the widest Code B in the fleet,
        # over the Twin Otter's 19.75.
        code="B",
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
        surface="TARMAC",
        published_table=PUBLISHED_TABLE,
        published_table_label="the published",
        report_extra=_report,
        raked=(),
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a Saab 340 turns out of a regional stand on its own props, the Q400's reason one "
            "letter down; the depot is the jets' tax"
        ),
        yard_label="Plane18 (Saab 340B)",
    )
