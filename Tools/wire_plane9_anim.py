"""Authors ABP_Plane9's AnimGraph - thirteen bones - and reads it back to prove it.

  python Tools/wire_plane9_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane9_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane9_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane9_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. WHAT IS LEFT HERE is plane9's own: its Blueprint and
its thirteen bones in chain order - plane5's set, bone for bone.

ONE RETRACT ANGLE, SO THE MULTIPLIERS ARE THE CONVENTION AND NOTHING ELSE. plane8 needed its
multiplier column to carry per-leg ratios because its three units fold 77.66, 90 and 54.72.
plane9's mains and nose both fold 86 (plane9/scripts/gear_pivots.json - solved, not 90,
because past 86 the main leg's inboard end rises through the 0.32 m wing root), so ONE
GearRetractedAngleDegrees = 86 on ABP_Plane9 drives all three legs and every gear bone takes
the fleet's plain -1. build_plane9_type.py writes that 86 from rig_map.json.

THE -1 IS HANDEDNESS: Blender is right-handed and UE left-handed, so the model's positive is
the graph's negative on every travelling and spinning bone. See wire_plane6_anim.py's header
for the measurement, and AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe for the test that
pins the fold direction.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP: props; wheels (children of the legs); nosewheel_steer
# (parent of nosewheel, child of gear_nose); the legs; then the doors, which hang off root.
PLAN = [
    ("prop_L",          "PropAngleDegrees",    -1.0),
    ("prop_R",          "PropAngleDegrees",    -1.0),
    ("wheel_L",         "WheelAngleDegrees",   -1.0),
    ("wheel_R",         "WheelAngleDegrees",   -1.0),
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

MODEL = wire_anim_lib.Model("plane9", "/Game/Aircraft/Plane9/ABP_Plane9", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
