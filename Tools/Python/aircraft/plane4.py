"""DA_Aircraft_Plane4 / ABP_Plane4 - the Boeing 737-800W.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as plane2's and plane3's state it.

WHAT THIS TYPE IS FOR. plane2 is the STOL end (grass, 366 m), plane3 the regional step up that
makes a runway a decision (tarmac, 1,402 m). This is the JET: 2,316 m of runway, 189 seats and a
forty-minute turnaround, so an airport that can take one has been built for nothing else. It is
also the first airframe in the project whose gear retracts - see GEAR below and
docs/superpowers/specs/2026-09-19-gear-retraction-design.md.

WHY THIS TYPE AND NOT DA_Aircraft_B738, which already exists. That one is the PAPER 737: its
figures are hand-typed from the datasheet in UAircraftType::Build737, it carries no mesh, and
Solve/IcaoCode.cpp pins its nose and tail against the same constants. It exists so stand layout
and aerodrome-code classification can reason about a 737 without one being modelled. This type
is the aeroplane that actually flies, and its figures are MEASURED off SK_Plane4 - which is why
the two are separate assets rather than one asset with two authors. Since 2026-09-19 the two
AGREE on the nose and the gear, because the disagreement turned out to be Build737's mistake
rather than the model's; see the nose-overhang check below.

NOSE OVERHANG, AND THE ACCUSATION THAT WAS WITHDRAWN. This check used to report that the
export's gear assembly sat 1.11 m too far forward, on the strength of UAircraftType::Build737's
NoseX = 520. It was wrong, and it was wrong in the most expensive direction: it accused a
correct model and sent its author to Blender to break it.

Boeing D6-58325-7 Rev C section 2.2.6 - the drawing is in this model's own concept folder,
plane4/concept/737NG_REV_C.pdf page 2-14 - gives the 737-800W side view as 13 FT 5 IN (4.09 M)
from the nose to the nose gear and 51 FT 2 IN (15.60 M) from there to the mains. The export
measures 4.090 and 15.600. Both exact.

Build737 was carrying the A320's 5.07 m overhang rounded to 5.2, which is a metre of aeroplane
that does not exist on a 737. It has been corrected to 409, and its tail with it.

THE LESSON IS ABOUT PROVENANCE, not about arithmetic. A measured figure was checked against a
TYPED one and the typed one won, because it was in C++ and looked authoritative. The primary
source was on disk the whole time, sitting beside the .blend.

THE GEAR-CYCLE-VS-CLEAR-ALTITUDE CHECK NEVER RAN FOR THIS AIRCRAFT before this refactor -
plane3's script introduced it and plane4's script predates plane3's in this telling only by
never having been revisited to add it. It now runs here as it does for every other retracting
type, PASSing at the same figures this type has always shipped (cycle 9 s, top ~332 m, well
under the 500 m clear_altitude below).
"""
from build_aircraft_type import AircraftSpec, say_tightest_radius

TYPE_NAME = "DA_Aircraft_Plane4"
ABP = "/Game/Aircraft/Plane4/ABP_Plane4"
MESH = "/Game/Aircraft/Plane4/SK_Plane4"
SOURCE = r"C:\repos\AirportMgr2Models\plane4\export\plane4.glb"

# --- Published, from the Boeing 737-800W ----------------------------------------------
#
# A 79 t narrowbody jet. It rotates at 145 kn where plane3 rotates at 120 and plane2 at 60, and
# it wants 2,316 m of tarmac against plane3's 1,402 and plane2's 366.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=7453.0),   # Vr about 145 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7196.0),   # Vref about 140 kn
}

# 78 DEGREES is the 737's tiller authority. On the measured 15.60 m wheelbase that makes the
# tightest followable radius about 16 m, a REQUIREMENT ON THE AIRPORT. 0.15 g matches plane3.
STEERING = {
    "max_steer_degrees": 78.0,
    "max_lateral_accel_uu": 147.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 9.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 12850.0,       # 250 kn, the standard climb speed below 10,000 ft
    # HIGHER THAN plane3's 400 m: the gear cycle below starts at 90 m and takes 9 seconds, and
    # an agent removed at 400 m would vanish mid-retraction on a slow climb.
    "clear_altitude": 50000.0,    # 500 m
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # THE REAL FAN SPEED, not a display figure. A CFM56-7B turns 5,175 rpm at 100 % N1.
    "max_rpm": 5175.0,
    "spool_up_seconds": 8.0,
    "spool_down_seconds": 25.0,
}

# THE FIRST RETRACTABLE GEAR IN THE PROJECT. 7 seconds is the real 737-800 transit, with a
# second of nose bay door either side of it, so a full cycle is 9. The mains have no doors and
# want none: their wheels sit in a well behind a fixed fairing.
GEAR = {
    "travel_seconds": 7.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 231600.0,   # 2,316 m
    "landing_field_length": 163400.0,   # 1,634 m
}

TURNAROUND_SECONDS = 2400.0   # 189 seats through two doors: forty minutes

# TWENTY-FOUR BLADES, a CFM56-7B fan's - UAirsideAgentAnim::PropStepDegrees resolves the frame
# rate against ONE BLADE REPEAT, so a 24-blade fan told it has 3 aliases backwards at any frame
# rate this game runs at.
PROP_BLADE_COUNT = 24


def _nose_overhang_check(spec, m, say, fail):
    """THE NOSE OVERHANG, checked every run against the DRAWING rather than against another
    piece of code - see the module header for what happened when it was the other way round.
    Boeing D6-58325-7 Rev C section 2.2.6: 13 FT 5 IN, 409 uu."""
    overhang = m["footprint"]["nose_x"] - m["steer_axle_x"]
    if abs(overhang - 409.0) > 25.0:
        say("NOTE nose sits %.0f uu ahead of the nose gear against Boeing's 409 (13 FT 5 IN, "
            "D6-58325-7 section 2.2.6) - %.2f m out. Check the export before anything else; "
            "this figure has been exact since the model was built."
            % (overhang, (overhang - 409.0) / 100.0))
    else:
        say("PASS nose overhang %.0f uu, within 25 uu of Boeing's 409 (13 FT 5 IN)" % overhang)


def _report(spec, m, say, fail):
    _nose_overhang_check(spec, m, say, fail)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane4",
        type_name=TYPE_NAME,
        short_code="B738",
        display_name="737-800",
        # Code C: measured span 35.79 m (over the winglets), and the LARGEST aeroplane the
        # letter admits, so a Code C stand sized for this one is sized for everything below it.
        code="C",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        gear_leg="maingear_L",
        # THE FAN, honestly named fan_L in the mesh, ridden on a bone called prop_L so
        # BONE_RULES' "prop" needle finds a driver.
        prop="fan_L",
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "fan %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu"),
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "fixed axle %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f)"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        angles=None,   # no door/gear angle authored for this rig; class defaults apply.
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
        report_extra=_report,
        # Nothing on this rig is deliberately off-axis - unlike plane6, whose raked nose leg
        # and slanted bay hinges are named. Left empty and NOT omitted, so a rig that grows a
        # raked bone has an obvious place to declare it rather than a check to weaken.
        raked=(),
        # ABP_Plane4 predates this tooling - duplicated from ABP_Plane2 in the editor on
        # 2026-09-19 and retargeted by hand. build_aircraft_anim.py measures it rather than
        # creating one, and fails loudly if it is missing. See AircraftSpec.create_abp.
        create_abp=False,
        pushback_need="VEHICLE_TUG",
        pushback_reason=(
            "a 737-800 needs the tug its paper twin DA_Aircraft_B738 does, and the modelled A320 "
            "beside it does too - a jet with no reverse-pitch prop, plane9's and plane14's "
            "reasoning. ABSENT FROM THIS TABLE UNTIL ISSUE #293's coverage sweep found it - "
            "EveryAircraftType() is what noticed, not a human re-reading the list"
        ),
        yard_label="Plane4 (737-800W)",
    )
