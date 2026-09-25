"""Authors ABP_Plane13's AnimGraph - fifteen bones - and reads it back to prove it.

  python Tools/wire_plane13_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane13_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane13_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane13_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. WHAT IS LEFT HERE is plane13's own: its Blueprint and
its fifteen bones in chain order - plane11's set with one axle fewer a side.

ONE RETRACT ANGLE, SO THE MULTIPLIERS ARE THE CONVENTION AND NOTHING ELSE. plane13's mains and
nose all fold 90 (rig_map.json's GearRetractedAngleDegrees), so every gear bone takes the
fleet's plain -1, as plane11's do. build_plane13_type.py writes that 90 onto ABP_Plane13.

THE -1 IS HANDEDNESS: Blender is right-handed and UE left-handed, so the model's positive is
the graph's negative on every travelling and spinning bone. See wire_plane6_anim.py's header
for the measurement, and AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe for the test that
pins the fold direction. The nose leg folding FORWARD (plane11's goes aft) is in the bone's
rest direction, not in this column.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP: fans; four main wheels (children of the legs);
# nosewheel; nosewheel_steer (parent of nosewheel, child of gear_nose); the legs; then the
# doors, which hang off root.
PLAN = [
    ("prop_L",          "PropAngleDegrees",    -1.0),
    ("prop_R",          "PropAngleDegrees",    -1.0),
    ("wheel_L1",        "WheelAngleDegrees",   -1.0),
    ("wheel_L2",        "WheelAngleDegrees",   -1.0),
    ("wheel_R1",        "WheelAngleDegrees",   -1.0),
    ("wheel_R2",        "WheelAngleDegrees",   -1.0),
    ("nosewheel",       "WheelAngleDegrees",   -1.0),
    ("nosewheel_steer", "SteerAngleDegrees",   None),
    ("gear_L",          "GearAngleDegrees",    -1.0),
    ("gear_R",          "GearAngleDegrees",    -1.0),
    ("gear_nose",       "GearAngleDegrees",    -1.0),
    ("door_main_L",     "BayDoorAngleDegrees", -1.0),
    ("door_main_R",     "BayDoorAngleDegrees", -1.0),
    ("door_nose_L",     "BayDoorAngleDegrees", -1.0),
    ("door_nose_R",     "BayDoorAngleDegrees", -1.0),
]

MODEL = wire_anim_lib.Model("plane13", "/Game/Aircraft/Plane13/ABP_Plane13", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
