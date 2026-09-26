"""DA_Aircraft_Plane16 / ABP_Plane16 - the Beechcraft Baron 58.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. The first PISTON TWIN, and the first Code A aeroplane whose gear
retracts. Two 300 hp IO-550s on an 11.53 m wing - narrower than the SR22 - so it stands on a
light single's stand, but asks a longer field than any of them (710 m) and still less than the
Caravan's 740. It is the twin a grass field takes before it is paved for the King Air.

THE SOURCE DRAWINGS ARE ON DISK, but no maker's dimensioned 3-view: plane16/concept holds an R/C
plan of the 58P (the fuselage authority), a Baron 55 three-view (the wing group, shifted 0.254 m
aft for the 58's cabin stretch) and a POH dimension page. plane16/SPEC.md tabulates the model
against the published figures and says which drawing won where they disagree; none of that is
re-litigated here.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane14's and plane15's are
not. The POH page in concept/ is dimensions only; the field lengths and speeds below are the
Baron 58 figures as commonly quoted (5,500 lb gross, sea level, standard day), rounded in the
conservative direction. Replace them from the POH performance section when it is to hand.

THE AEROPLANE SITS 2.44 DEGREES NOSE-UP ON ITS GEAR (SPEC.md, drawing.png's sloped ground line),
plane12's case at half the angle. The footprint is a plan shadow of the pitched mesh, so plan
length reads the bbox 9.166 against the 9.093 along the reference line; the table below compares
against the bbox figure and says so. The propeller discs are tilted with it, so the diameter is
measured by plane12's centroid method, which ignores which way a disc faces - and a three-blade
disc's box would be short anyway (plane10's reason).

NO rig_map.json FOR THIS MODEL - plane16's rig is plane14's shape (SPEC.md), and like plane14's
its angles are constants in build_rig.py; `RigScriptLine` reads that line rather than retyping
it, so the ABP and the model cannot drift.
"""
from build_aircraft_type import (AircraftSpec, RigScriptLine, say_published_table,
                                 say_tightest_radius)

# ONE MEASUREMENT, NOT TWO COPIES: plane12's centroid diameter reads a tilted disc of any
# blade count, and this aeroplane is pitched as plane12's is.
from aircraft.plane12 import _disc_diameter_by_centroid

TYPE_NAME = "DA_Aircraft_Plane16"
ABP = "/Game/Aircraft/Plane16/ABP_Plane16"
MESH = "/Game/Aircraft/Plane16/SK_Plane16"
MODELS = r"C:\repos\AirportMgr2Models\plane16"
SOURCE = MODELS + r"\export\plane16.glb"
RIG_SCRIPT = MODELS + r"\scripts\build_rig.py"

# --- Published (approximately - see the header) for the Baron 58, IO-550-C ---------------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 225 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 4372 gives 425 m against the quoted 1,400 ft (427 m).
    "takeoff": dict(accel=225.0, decel=400.0, speed_cap=4372.0),   # Vr 85 kn, just over Vmc
    "landing": dict(accel=100.0, decel=400.0, speed_cap=4938.0),   # approach 96 kn
}

# 30 DEGREES, plane1's EFFECTIVE AUTHORITY: the Baron steers its nosewheel off the rudder
# pedals and turns the rest on differential brake and power, and nothing on disk gives a
# turning radius to derive a lock from as plane10's POH did.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,   # between the singles' 5 and the jets' 3
    "climb_speed": 5401.0,            # 105 kn, two-engine Vy
    "clear_altitude": 30000.0,        # the light singles' circuit
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 800.0,            # plane12's: a low wing
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE: the IO-550-C turns the propeller itself, so redline IS propeller rpm.
    "max_rpm": 2700.0,
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

# THE GEAR. Mains fold FORWARD into the nacelles, nose folds AFT into the nose (SPEC.md; the
# trunnions are INVENTED). 6 s of travel, plane14's light-aeroplane figure; the bay doors on
# all three legs are bones, so the door stage is seen everywhere.
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 71000.0,   # 710 m, 2,300 ft over 50 ft rounded up
    "landing_field_length": 77000.0,   # 770 m, 2,500 ft over 50 ft rounded up
}

TURNAROUND_SECONDS = 720.0   # six seats through a front door and the aft double door, plus fuel

# THREE BLADES a side - SPEC.md's 78" props, plane2's blade x3.
PROP_BLADE_COUNT = 3

PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 11.532),
    ("length*", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 9.166),
    ("wheelbase", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 2.718),
    ("track", lambda m: m["main_gear_track"], 2.921),
    ("stabiliser", lambda m: m["footprint"]["tailplane_span"], 4.851),
    ("propeller", lambda m: m["prop_diameter"], 1.981),
]


def _report(spec, m, say, fail):
    say_published_table(spec, m, say)
    say("  * length is the PITCHED bbox (SPEC.md 9.166); 9.093 along the reference line")
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane16",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="BE58",
        display_name="Beechcraft Baron 58",
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
    )
