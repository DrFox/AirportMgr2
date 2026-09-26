"""DA_Aircraft_Plane5 / ABP_Plane5 - the Beechcraft King Air 350i.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as plane2's, plane3's and plane4's
state it.

WHAT THIS TYPE IS FOR. It is the rung the ladder did not have. The fleet goes plane1 (a 172,
grass, 497 m), plane2 (a Twin Otter, grass, 366 m but 19.75 m of span), plane3 (a Q400, TARMAC,
1,402 m) and plane4 (a 737, tarmac, 2,316 m). Between the Twin Otter and the Q400 the player's
next capability was a THOUSAND-metre tarmac runway, bought in one jump. The King Air asks for
tarmac and 1,006 m, which splits that jump in two - it is the first type in the game whose
requirement is a SURFACE rather than a length: it needs 2.7 times LESS runway than the Q400 and
the same surface. Widening no longer helps; paving does.

Code B on a 17.69 m span, and the only Code B in the fleet besides the Twin Otter.

THE PRIMARY SOURCE IS IN THE MODEL'S OWN CONCEPT FOLDER, and this cites it by page the way
plane4's cites Boeing D6-58325-7:

  plane5/concept/King Air 350i Specification and Description (FL-1161 and on).pdf
  (Beechcraft / Textron Aviation, August 2018, serials FL-1161 to TBD)

    p5  1.3 Approximate Dimensions    height 4.37 m, span 17.65 m, length 14.22 m,
                                      WHEELBASE 16 ft 3 in (4.95 m)
    p5  1.4 Design Weights            MTOW 15,000 lb (6,804 kg)
    p6  2. Performance                TAKEOFF FIELD LENGTH 3,300 ft (1,006 m) at MTOW;
                                      LANDING DISTANCE 2,692 ft (821 m) at max landing weight,
                                      flaps down, no prop reverse
    p8  7.1 Landing Gear              "The main landing gear hydraulically retracts FORWARD
                                      into each engine nacelle" - which is what the rig does
    p8  7.3 Brakes and Tires          NOSE 22 x 6.75-10, MAIN 19 x 6.75-8, mains DUAL
    p8  8.1 Powerplant                two PT6A-60A, 1,050 SHP each
    p9  8.2 Propellers                Hartzell 105-inch four blade, full feathering

  The PDF IS A SCAN with no text layer - pdftotext returns nothing at all from it. Render the
  pages (pymupdf, ~140 dpi) and read them. A grep that comes back empty here is the probe
  failing, not the document being silent.

TWO PLACES THE MODEL AND THE SPECIFICATION DISAGREE, reported and NOT corrected - see
`_wheelbase_note` below. WHEELBASE: the export measures about 4.60 m against the S&D's 16 ft
3 in (4.95 m) - 35 cm more. plane5/scripts/build_gear.py checked its own scaling against "14 ft
11 in", which is not this document's figure. MAIN TYRE: build_gear.py built the mains as
22 x 6.75-10 (0.56 m); section 7.3 gives that as the NOSE tyre and 19 x 6.75-8 (0.48 m) as the
main - the two are swapped.

NEITHER IS FIXED HERE, and that is the plane4 lesson applied rather than restated: a measured
figure was checked against a typed one and the typed one won because it looked authoritative.
So this MEASURES and REPORTS, and the decision about the .blend belongs to whoever owns it. What
the type carries is the model, because the animation rolls the wheel the PLAYER SEES.

THE STEERING LOCK IS A GAMEPLAY RULING, NOT A MEASUREMENT - see STEERING below.

THE GEAR-CYCLE-TOP FORMULA USES sin(radians(pitch)) RATHER THAN radians(pitch) DIRECTLY, unlike
every other retracting type in the fleet - `climb_rate_fn` below. This predates the rest of the
fleet settling on the small-angle form and is the true vertical-speed component; the two agree
to within 1% at these pitch angles, which is not worth re-deriving this aircraft's PASS line
over.
"""
import math

from build_aircraft_type import AircraftSpec, Typed, say_tightest_radius

TYPE_NAME = "DA_Aircraft_Plane5"
ABP = "/Game/Aircraft/Plane5/ABP_Plane5"
MESH = "/Game/Aircraft/Plane5/SK_Plane5"
SOURCE = r"C:\repos\AirportMgr2Models\plane5\export\plane5.glb"

# --- Published, from the Beechcraft King Air 350i --------------------------------------
#
# A 6.8 t twin turboprop between the Twin Otter and the Q400 in every dimension.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 2,100 SHP on 6.8 t accelerates HARD - between the Otter's 450 and the Q400's 250.
    "takeoff": dict(accel=350.0, decel=400.0, speed_cap=5140.0),   # Vr about 100 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=5300.0),   # Vref about 103 kn
}

# STEERING - THE REAL AEROPLANE STEERS BADLY. Section 7.2: "Nosewheel steering is mechanically
# actuated by the rudder pedals" - no tiller, worth about 12 degrees of lock, which on the
# measured 4.60 m wheelbase would be a 22 m tightest radius - worse than the 737's 16 m on the
# SMALLEST aeroplane in the fleet. RULED 2026-09-21: treat it as turning well, 60 degrees, the
# Twin Otter's lock, on a wheelbase within 6 cm of the Twin Otter's 4.54 m.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,
    "climb_speed": 8200.0,        # 160 kn, the 350's climb speed
    "clear_altitude": 40000.0,    # 400 m; higher than the light types' because it retracts
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 900.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # PROPELLER RPM, NOT ENGINE RPM. 1,700 is not in the S&D - it is the propeller governor
    # limit from the type's own limitations, the least well sourced number in this file.
    "max_rpm": 1700.0,
    "spool_up_seconds": 4.0,
    "spool_down_seconds": 9.0,
}

# THE SECOND RETRACTABLE GEAR IN THE PROJECT, and the first with doors at BOTH ends. Section
# 7.1: "The main landing gear hydraulically retracts forward into each engine nacelle." SIX
# SECONDS, against the 737's seven - VLO retraction is 166 KIAS and extension 184 (S&D p6).
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 100600.0,   # 1,006 m, 3,300 ft at MTOW, flaps approach
    "landing_field_length": 82100.0,    # 821 m, 2,692 ft at max landing weight, no reverse
}

TURNAROUND_SECONDS = 720.0   # twelve minutes: eleven seats through one airstair door

# FOUR BLADES, S&D p9.
PROP_BLADE_COUNT = 4

# NINETY DEGREES - build_rig.py's contract is "DOORS: 0 = hanging open, +90 = closed", and the
# pose check folds the gear and reports where each closed door's free edge lands.
BAY_DOOR_CLOSED_ANGLE_DEGREES = 90.0

# The S&D's wheelbase, p5: 16 ft 3 in.
SPEC_WHEELBASE_UU = 495.0


def _wheelbase_note(spec, m, say, fail):
    """THE WHEELBASE, CHECKED EVERY RUN AGAINST THE MANUFACTURER'S OWN TABLE. A NOTE and not a
    failure - see the module header for the plane4 accusation that had to be withdrawn."""
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    if abs(wheelbase - SPEC_WHEELBASE_UU) > 25.0:
        say("NOTE wheelbase %.0f uu against the S&D's %.0f (16 FT 3 IN, section 1.3) - "
            "%.2f m out. plane5/scripts/build_gear.py derived it from side.jpg and checked it "
            "against 14 ft 11 in, which is not this aeroplane's published figure. The type "
            "carries the MEASURED value on purpose; the .blend is where this is settled."
            % (wheelbase, SPEC_WHEELBASE_UU, (wheelbase - SPEC_WHEELBASE_UU) / 100.0))
    else:
        say("PASS wheelbase %.0f uu, within 25 uu of the S&D's %.0f (16 FT 3 IN)"
            % (wheelbase, SPEC_WHEELBASE_UU))


def _report(spec, m, say, fail):
    _wheelbase_note(spec, m, say, fail)
    say_tightest_radius(spec, m, say)


def _climb_rate(climb):
    return climb["climb_speed"] * math.sin(math.radians(climb["climb_pitch_degrees"]))


def get_spec():
    return AircraftSpec(
        key="plane5",
        type_name=TYPE_NAME,
        short_code="B350",
        display_name="King Air 350i",
        # Code B: measured span 17.69 m, shares the letter with the Twin Otter at 19.75 m.
        code="B",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        gear_leg="maingear_L",
        prop="prop_L",
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "fixed axle %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f)"),
        tightest_radius_line=(
            "tightest followable radius %(radius_uu).0f uu (%(radius_m).1f m) at %(lock).0f "
            "degrees of lock - a RULING, see STEERING; the real aeroplane's pedal steering "
            "would give about 22 m"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        climb_rate_fn=_climb_rate,
        angles=Typed(door=BAY_DOOR_CLOSED_ANGLE_DEGREES),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
        field_lengths_suffix=" - S&D p6",
        report_extra=_report,
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a King Air 350i beta-ranges off a stand on its PT6s' reverse pitch, plane10's Caravan "
            "reasoning one size up. ABSENT FROM THIS TABLE UNTIL ISSUE #293's coverage sweep found "
            "it, the same gap plane4's row closes - the class default of VehicleTug would otherwise "
            "have silently gated a King Air behind the depot"
        ),
        yard_label="Plane5 (King Air 350i)",
    )
