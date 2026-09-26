"""DA_Aircraft_Plane14 / ABP_Plane14 - the Embraer Phenom 300.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it.

WHAT THIS TYPE IS FOR. The first BUSINESS JET, and the first jet under Code C. Every jet before
it needed a Code C stand and 2 km of runway; the Phenom 300 is a Code B aeroplane - 15.91 m of
wing, 0.91 m over Code A's 15 m - that asks about 1,000 m of TARMAC. So it is the jet a small
paved field can take, beside the King Air it shares a letter with.

THE SOURCE DRAWINGS ARE ON DISK, but they are MARKETING drawings, not a manufacturer's
dimensioned 3-view: plane14/concept/drawing-*.png, and plane14/SPEC.md tabulates the model
against them - span 15.910 (15.91), length 15.635 (15.64), height 4.896 against the published
5.10, where the front GA and Embraer's figure disagree and the model follows the GA.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane11's and plane13's are
not. No Embraer AFM or Airport Planning Manual is in plane14/concept; the field lengths, speeds,
tiller lock and fan speed below are commonly quoted figures, rounded in the conservative
direction. Replace them from the Phenom 300 APM when it is to hand.

NO rig_map.json FOR THIS MODEL - plane14's pipeline is plane12's, which writes none. The angles
are constants in build_rig.py, which poses the fold with them; `RigScriptLine` below reads that
line rather than retyping it, so the ABP and the model still cannot drift.
"""
from build_aircraft_type import AircraftSpec, RigScriptLine

TYPE_NAME = "DA_Aircraft_Plane14"
ABP = "/Game/Aircraft/Plane14/ABP_Plane14"
MESH = "/Game/Aircraft/Plane14/SK_Plane14"
MODELS = r"C:\repos\AirportMgr2Models\plane14"
SOURCE = MODELS + r"\export\plane14.glb"
RIG_SCRIPT = MODELS + r"\scripts\build_rig.py"

# --- Published (approximately - see the header) for the Phenom 300, PW535E -----------------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 250, THE NARROWBODIES': a light jet's thrust-to-weight is an airliner's, not a
    # turboprop's 350 - it is the lower Vr that makes the roll short.
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=5397.0),   # Vr about 105 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=5911.0),   # Vref about 115 kn
}

# 60 DEGREES of tiller authority, a round figure for a light jet's nosewheel. On the measured
# ~5.9 m wheelbase that is about a 7 m tightest radius.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 10.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 10280.0,       # 200 kn
    "clear_altitude": 40000.0,    # 400 m, plane5's: an 8 s gear cycle finishes near 260 m
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # About 14,000 rpm at 100 % N1 - a 0.6 m fan turns fast; the figure is a tip speed of about
    # 450 m/s over the modelled disc, not a quoted one.
    "max_rpm": 14000.0,
    "spool_up_seconds": 6.0,
    "spool_down_seconds": 20.0,
}

# THE GEAR. Trailing-link mains fold inboard into the belly fairing and the nose folds forward
# (SPEC.md; the trunnions are INVENTED). 6 s of travel, a light jet's; only the nose has door
# bones, so the door stage is seen at the nose alone.
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 100000.0,   # 1,000 m, commonly quoted ~980 m
    "landing_field_length": 70000.0,    # 700 m, commonly quoted ~675 m
}

TURNAROUND_SECONDS = 900.0   # fifteen minutes: eight seats through one airstair door, plus fuel

# TWENTY BLADES, plane14/scripts/build_engine.py's N_BLADES.
PROP_BLADE_COUNT = 20


def get_spec():
    return AircraftSpec(
        key="plane14",
        # The ICAO type designator. ShortCode is a label, not a key.
        short_code="E55P",
        display_name="Phenom 300",
        code="B",
        type_name=TYPE_NAME,
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        # The winglets are part of the wing object (SPEC.md: the bend start is solved so the
        # outermost vertex is the published half-span), so the span is over them.
        wing="wing",
        stabiliser="stabiliser",
        gear_leg="maingear_L",
        # The MESH is honestly a fan; its BONE is prop_L so BONE_RULES' "prop" needle finds a
        # driver.
        prop="fan_L",
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "fan %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu, "
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
        # NO BONE IS RAKED - this rig's retired build script's RAKED was always correctly
        # empty, but its say() line insisted "ONE BONE IS RAKED ON PURPOSE" regardless, copied from
        # plane9's/plane12's script and never re-typed (Issue #294). nosewheel_steer measures
        # square on this rig; build_aircraft_anim.py's raked_line() derives the wording from
        # len(raked) so the two cannot disagree again.
        raked=(),
        pushback_need="VEHICLE_TUG",
        pushback_reason=(
            "a Phenom 300 has no thrust reversers, so it cannot back off a stand on its own the way "
            "the turboprops' reverse pitch lets them - it is towed, and the first jet a small field "
            "takes is also what first asks it for a tug"
        ),
        yard_label="Plane14 (Phenom 300)",
    )
