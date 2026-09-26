"""DA_Aircraft_Plane6 / ABP_Plane6 - the Boeing 777-300ER.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as plane2's, plane3's and plane4's
state it.

WHAT THIS TYPE IS FOR. plane2 is the STOL end (grass, 366 m), plane3 the regional step up that
makes a runway a decision (tarmac, 1,402 m), plane4 the narrowbody JET (2,316 m, 189 seats,
forty minutes). This is the WIDEBODY, and what it adds is not more of the same: it is the first
aeroplane in the game at any aerodrome code letter but B or C. A Code E stand is half again as
wide as a Code C one and its taxiways are 23 m rather than 15, so an airport that can take this
has been REBUILT rather than extended. Its forty-minute turnaround becomes ninety.

THE PRIMARY SOURCE IS ON DISK FOR THE SHAPE AND NOT FOR THE PERFORMANCE, and that asymmetry is
stated here rather than discovered later. plane6/concept/777-300ER.dxf is the three-view the
model is traced from, so every measured figure below is checkable against a drawing in the
repo. There is NO Boeing D6-58329 (777 Airplane Characteristics for Airport Planning) in that
folder - unlike plane4's D6-58325-7 and plane5's Specification and Description - so the field
lengths, speeds, spool times and gear timings are published figures WITHOUT a document here to
check them against. Anyone who adds D6-58329 to plane6/concept/ should re-read REQUIREMENTS and
GROUND against it first; that is the exact shape of the mistake plane4's header records under
"the accusation that was withdrawn", with the primary source sitting on disk the whole time.

WHY NO UAircraftType::Build777 TO GO WITH IT. plane4's header explains why DA_Aircraft_B738
exists beside DA_Aircraft_Plane4 - the paper 737 lets stand layout and code classification
reason about a 737 with no model. There is no paper 777 and there is no reason to add one: the
paper types exist because they predate the models, and a new type that arrives WITH its model
has nothing to be a placeholder for. IcaoCode's Code E row is therefore the first row in that
table measured against an ASSET rather than a C++ builder - see `_span_tail_report` below and
Airside.Content.MeasuredTypesFitTheirLettersRow for the judging (that judging itself, and the
check_letter()/MAX_*/MIN_SPAN machinery it used to run through in this script, moved into
Solve/IcaoCode.cpp under #292/PR #322 - this script only ever reports the measurement now).
"""
from build_aircraft_type import AircraftSpec, Typed, say_span_and_tail, say_tightest_radius

TYPE_NAME = "DA_Aircraft_Plane6"
ABP = "/Game/Aircraft/Plane6/ABP_Plane6"
MESH = "/Game/Aircraft/Plane6/SK_Plane6"
SOURCE = r"C:\repos\AirportMgr2Models\plane6\export\plane6.glb"

# THE MAIN WHEELS, AND THIS IS THE FIRST ROW IN THE FLEET THAT IS NOT A PAIR. A 777 main leg
# carries a THREE-AXLE BOGIE, so plane6/scripts/build_rig.py names six wheel bones - a bone
# turns about its own length, and one bone cannot roll three axles without the outer two
# orbiting it rather than spinning. THE BOGIE'S CENTRE IS THE AXLE, which is why these are
# averaged rather than one being picked.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2", "wheel_L3")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2", "wheel_R3")

# --- Published, from the Boeing 777-300ER ---------------------------------------------
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 220 IS BELOW plane4's 250 AND plane3's 250, WHICH IS THE POINT: a 777-300ER's thrust-to-
    # weight at MTOW is a shade under a loaded 737's, and it has to reach a Vr 20 kn higher.
    "takeoff": dict(accel=220.0, decel=400.0, speed_cap=8481.0),   # Vr about 165 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7659.0),   # Vref about 149 kn
}

# 70 DEGREES is the 777's tiller authority, the same as the Q400's. On the measured 31.22 m
# wheelbase that makes the tightest followable radius about 33 m - more than DOUBLE plane4's
# 16 m. 0.12 g is the gentlest in the fleet.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 118.0,
}

CLIMB = {
    # EIGHT DEGREES, LOWER THAN plane4's NINE - the -300ER is famously tail-strike-prone; the
    # geometry runs out at around 9 degrees with the gear compressed.
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 11.0,
    "rotate_rate_deg_per_sec": 2.0,
    "climb_speed": 12850.0,       # 250 kn, the standard climb speed below 10,000 ft
    # 650 m: the gear cycle below is 16 seconds and starts at 90 m; at 250 kn on an 11-degree
    # climb that is about 395 m of height before the last door shuts.
    "clear_altitude": 65000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # A GE90-115B turns about 2,355 rpm at 100 % N1 - a big fan turns SLOWLY.
    "max_rpm": 2355.0,
    "spool_up_seconds": 10.0,
    "spool_down_seconds": 30.0,
}

# THE GEAR. plane4 was the first retractable type, plane5 the first with doors at both ends;
# this is the slowest and the largest, and the first whose cycle forced ClearAltitude up.
GEAR = {
    "travel_seconds": 12.0,
    "door_seconds": 2.0,
    # ZERO, AND IT IS A FACT ABOUT THIS AEROPLANE RATHER THAN AN OMISSION. A real 777 main truck
    # DOES tilt, and FGearPerformance::TruckTiltSeconds was added for this model - as were
    # BONE_RULES' `truck` and `bogie` needles - but plane6 shipped with its bogies RIGID and no
    # bone for a tilt to drive. plane8's A380 is where the field starts earning its place.
    "truck_tilt_seconds": 0.0,
    "retract_above_height": 9000.0,
    "extend_below_height": 15000.0,
}

REQUIREMENTS = {
    "takeoff_field_length": 312000.0,   # 3,120 m - more than double the Q400's
    "landing_field_length": 186000.0,   # 1,860 m at max landing weight
}

TURNAROUND_SECONDS = 5400.0   # ninety minutes: 396 seats through four doors, plus long-haul turn

# TWENTY-TWO BLADES - a GE90-115B, and build_fan.py arrays exactly that many.
PROP_BLADE_COUNT = 22

# EIGHTY-ONE DEGREES, which is ALSO UAirsideAgentAnim's default - and it is set anyway.
# plane6/scripts/door_pivots.json keys open_deg at 81.0, so this rig happens to agree with the
# figure the default was measured from (plane4's). A default that HAPPENS to be right is not a
# declaration: it goes on being right only until somebody retunes the default for a different
# aeroplane, and then this rig's doors stop 9 degrees from shut with nothing saying why.
BAY_DOOR_CLOSED_ANGLE_DEGREES = 81.0


def _report(spec, m, say, fail):
    say_span_and_tail(spec, m, say)
    say_tightest_radius(spec, m, say)


def get_spec():
    return AircraftSpec(
        key="plane6",
        type_name=TYPE_NAME,
        # The ICAO type designator for a 777-300ER, which is what a flight plan and a stand
        # board call it. B773 is the non-ER -300, a different weight and field length.
        short_code="B77W",
        display_name="777-300ER",
        # CODE E, AND IT IS THE FIRST TYPE IN THE GAME AT ANY LETTER BUT B OR C. The measured
        # span is 64.78 m against Code E's 65 m ceiling - 22 cm inside it, with no headroom at
        # all for a re-export that grows the wing.
        code="E",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        gear_leg="maingear_L",
        prop="fan_L",
        main_wheels_l=MAIN_WHEELS_L,
        main_wheels_r=MAIN_WHEELS_R,
        export_line=("measured off the export: wheel radius %(wheel_radius).1f, "
                    "fan %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu"),
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "bogie centre %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f), "
                 "track %(main_gear_track).1f"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=GEAR,
        angles=Typed(door=BAY_DOOR_CLOSED_ANGLE_DEGREES),
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="TARMAC",
        report_extra=_report,
        # THE BONES THAT ARE DELIBERATELY NOT SQUARE TO THE AIRFRAME.
        #
        # resolve_axis asserts that every driven bone's rotation axis lies along one of UE's
        # own axes, because on the six rigs before this one it did, and a bone the rigger
        # failed to align is a change to look at. plane6 is the first aeroplane here where that
        # premise is FALSE BY DESIGN, so the three exceptions are declared with their reasons
        # rather than the guard being dropped off the other fourteen bones:
        #
        #   * nosewheel_steer - raked 12.4 degrees. The leg HAS to be raked to fold forward
        #     past the nose cone (plane6/scripts/build_gear.py's header argues it), and a nose
        #     leg steers about its STRUT rather than about the vertical. plane4's is vertical
        #     and its steer bone is +Z; this one runs axle-to-trunnion. A steer bone forced
        #     square would turn the wheel about a line the strut does not lie on, which reads
        #     as the tyre scrubbing sideways.
        #   * door_nose_L / door_nose_R - the nose bay doors hinge on the slanted line the
        #     bay's own edge fits, because the bay is cut into a curving belly ahead of a raked
        #     leg. A square hinge would swing them into the fuselage.
        #
        # SQUARENESS WAS NEVER A CORRECTNESS REQUIREMENT. A Transform (Modify) Bone in Bone
        # Space turns a bone about its OWN axes, so the graph never asks what the world
        # thinks. These three are wired exactly like the other fourteen; the only difference
        # is that the check reports their angle instead of refusing them, and would fail if
        # one of them were straightened without this list being updated.
        raked=("nosewheel_steer", "door_nose_L", "door_nose_R"),
        pushback_need="VEHICLE_TUG",
        pushback_reason=(
            "a 350 t 777-300ER is the far end of the same argument - and it is here rather than "
            "left to the class default on purpose: an unset field that happens to agree with the "
            "default is indistinguishable from an asset nobody authored, which is the point the "
            "Plane7 row already makes from the other side"
        ),
        yard_label="Plane6 (777-300ER)",
    )
