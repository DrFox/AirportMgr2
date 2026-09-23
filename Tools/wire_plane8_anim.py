"""Authors ABP_Plane8's AnimGraph - thirty-five bones - and reads it back to prove it.

  python Tools/wire_plane8_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane8_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane8_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane8_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. WHAT IS LEFT HERE is plane8's own: its Blueprint,
its thirty-five bones in chain order, and the one thing no earlier rig needed the multiplier
column for.

THE MULTIPLIER COLUMN CARRIES A MEASUREMENT ON THIS RIG, AND THAT IS NEW. On every rig before
this one it is -1.0 on the travelling and spinning bones and None on the steer bone: a
handedness convention, and wire_plane6_anim.py's header is careful to say so - "a multiplier
and not a negated axis on purpose - the axis column is measured and this is a convention".

The A380 has three retract angles - nose forward 77.66, wing inboard 90.00, body aft 54.72,
each solved against the skin in the models repo - and UAirsideAgentAnim drives every `gear`
bone from ONE GearRetractedAngleDegrees. ABP_Plane8 sets it to 90, the wing's, and the other
three bones take the -1 TIMES THEIR OWN RATIO: -0.6080 on the body gear and -0.8629 on the
nose. The angle the graph applies is (1 - GearDownFraction) x 90 x ratio, which is linear in
the fraction, so all five legs run on the model's one cycle and the body gear arrives at
54.72 exactly when the wing gear arrives at 90.

THE SAME TRICK CARRIES TWO TRUCK ANGLES. The wing bogies tilt 0 and the body bogies 54.72 -
their beams counter-rotate to lie level in the bay, which AirsideAgentAnim.h's "an A380 may
want two of these" anticipated. TruckTiltedAngleDegrees is 54.72; the body bogies take -1
and the wing bogies take 0, which drives them by nothing while keeping them in the chain and
the axis plan, so a rig that later gives them a real tilt changes one number here.

THE RATIOS ARE READ, NOT TYPED. plane8/scripts/rig_map.json carries `retract_deg` and
`truck_tilt_deg` per bone, written by the same build_rig.py that proved each angle against
build_gear.py's fold to 0.01 mm. A figure typed here would be a second copy of one of those,
and CLAUDE.md's rule about lists that must agree applies to a list of two.

THE ORDER IS THE HIERARCHY, BOTTOM UP, and this rig binds it harder than any other: four
legs are three deep (gear > bogie > wheels) and the nose is three deep (gear > steer >
wheel). Fans first; then every wheel; then the bogies, which are parents of wheels and
children of legs; then the steer bone; then the legs; then the doors, which hang off root.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

RIG_MAP = r"C:\repos\AirportMgr2Models\plane8\scripts\rig_map.json"

# THE ONE FIGURE EACH PER-TYPE PROPERTY IS SET TO on ABP_Plane8 - build_plane8_type.py writes
# them, from the same file. Named here because every ratio below is against them.
GEAR_RETRACTED_DEG = 90.0
TRUCK_TILTED_DEG = 54.72


def ratios():
    """(gear bone -> multiplier, bogie bone -> multiplier), each -1 x angle / reference.

    A reference of 0 would divide by it; the truck reference is non-zero because the body
    bogies tilt, and a wing bogie's 0 / 54.72 is the 0 the header describes.
    """
    with open(RIG_MAP) as handle:
        rig = json.load(handle)
    gear = {bone: -1.0 * deg / GEAR_RETRACTED_DEG
            for bone, deg in rig["retract_deg"].items()}
    # `or 0.0` TURNS -0.0 INTO 0.0. A wing bogie's -1 x 0 / 54.72 is IEEE negative zero,
    # which "%f" prints as "-0.000000" - and wire_anim_lib.verify compares the pin value it
    # reads back from the editor as a STRING, so a graph that is exactly right would fail on
    # the sign of nothing.
    truck = {bone: (-1.0 * deg / TRUCK_TILTED_DEG) or 0.0
             for bone, deg in rig["truck_tilt_deg"].items()}
    return gear, truck


GEAR, TRUCK = ratios()

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
PLAN = [
    ("prop_L_in",       "PropAngleDegrees",      -1.0),
    ("prop_L_out",      "PropAngleDegrees",      -1.0),
    ("prop_R_in",       "PropAngleDegrees",      -1.0),
    ("prop_R_out",      "PropAngleDegrees",      -1.0),
    ("wheel_L1",        "WheelAngleDegrees",     -1.0),
    ("wheel_L2",        "WheelAngleDegrees",     -1.0),
    ("wheel_L3",        "WheelAngleDegrees",     -1.0),
    ("wheel_L4",        "WheelAngleDegrees",     -1.0),
    ("wheel_L5",        "WheelAngleDegrees",     -1.0),
    ("wheel_R1",        "WheelAngleDegrees",     -1.0),
    ("wheel_R2",        "WheelAngleDegrees",     -1.0),
    ("wheel_R3",        "WheelAngleDegrees",     -1.0),
    ("wheel_R4",        "WheelAngleDegrees",     -1.0),
    ("wheel_R5",        "WheelAngleDegrees",     -1.0),
    ("nosewheel",       "WheelAngleDegrees",     -1.0),
    ("bogie_wing_L",    "TruckTiltAngleDegrees", TRUCK["bogie_wing_L"]),
    ("bogie_wing_R",    "TruckTiltAngleDegrees", TRUCK["bogie_wing_R"]),
    ("bogie_body_L",    "TruckTiltAngleDegrees", TRUCK["bogie_body_L"]),
    ("bogie_body_R",    "TruckTiltAngleDegrees", TRUCK["bogie_body_R"]),
    ("nosewheel_steer", "SteerAngleDegrees",     None),
    ("gear_wing_L",     "GearAngleDegrees",      GEAR["gear_wing_L"]),
    ("gear_wing_R",     "GearAngleDegrees",      GEAR["gear_wing_R"]),
    ("gear_body_L",     "GearAngleDegrees",      GEAR["gear_body_L"]),
    ("gear_body_R",     "GearAngleDegrees",      GEAR["gear_body_R"]),
    ("gear_nose",       "GearAngleDegrees",      GEAR["gear_nose"]),
    ("door_nose_L",     "BayDoorAngleDegrees",   -1.0),
    ("door_nose_R",     "BayDoorAngleDegrees",   -1.0),
    ("door_wing_L_in",  "BayDoorAngleDegrees",   -1.0),
    ("door_wing_L_out", "BayDoorAngleDegrees",   -1.0),
    ("door_wing_R_in",  "BayDoorAngleDegrees",   -1.0),
    ("door_wing_R_out", "BayDoorAngleDegrees",   -1.0),
    ("door_body_L_in",  "BayDoorAngleDegrees",   -1.0),
    ("door_body_L_out", "BayDoorAngleDegrees",   -1.0),
    ("door_body_R_in",  "BayDoorAngleDegrees",   -1.0),
    ("door_body_R_out", "BayDoorAngleDegrees",   -1.0),
]

MODEL = wire_anim_lib.Model("plane8", "/Game/Aircraft/Plane8/ABP_Plane8", PLAN)


if __name__ == "__main__":
    for bone, mul in sorted(list(GEAR.items()) + list(TRUCK.items())):
        print("ratio %-14s x %+.4f" % (bone, mul))
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
