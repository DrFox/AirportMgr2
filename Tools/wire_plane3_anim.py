"""Authors ABP_Plane3's AnimGraph - thirteen bones - and reads it back to prove it.

  python Tools/wire_plane3_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane3_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane3_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane3_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. Read that file for the four decisions a wiring run
makes - chain order, measured axis, node modes, and one door angle driving four doors.

THIS REBUILDS A GRAPH THAT WAS ALREADY WORKING, which no other wire_plane<N>_anim.py does
except plane4's. ABP_Plane3 has driven the Q400's props, wheels and nosewheel steering since
the model was imported; it is rebuilt rather than added to because the rig UNDERNEATH it
changed shape on 2026-09-21 - plane3/scripts/build_gear_rig.py re-parented wheel_L and
wheel_R beneath new retract bones - and chain order is a property of the hierarchy, not of
the six nodes that happened to be there. A graph patched in place would have kept the old
order and spun the wheels inside the nacelle. Run `--read` first if you want the old chain
in the log.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP, AND IT IS NOT THE ORDER THIS GRAPH USED TO HAVE.
# Until 2026-09-21 wheel_L and wheel_R hung off root and could be driven in any order at all;
# they now hang off gear_L and gear_R, and nosewheel_steer has moved under gear_nose. So:
#
#   props            - no children, no constraint, first because they are the simplest
#   wheels           - children of the retract bones, so they MUST precede them
#   nosewheel_steer  - parent of nosewheel, child of gear_nose: strictly between the two
#   retract bones    - parents of everything above them
#   doors            - children of root, unconstrained; last because they are independent
#
# ALL THREE LEGS FOLD FORWARD off one positive GearAngleDegrees, because all three retract
# bones point -X. That is the Q400 and not an inherited convention: plane5's nose leg folds
# aft and plane6's mains fold inboard, and neither needs a different row here, because the
# direction lives in the bone's rest orientation rather than in this table.
#
# THE GEAR AND DOOR BONES TAKE -1, as every rig in the fleet has since 2026-09-21. Blender is
# right-handed and UE is left-handed, so the import MIRRORS every bone-local rotation: the
# model's positive is the graph's negative. build_gear_rig.py keys the cycle with
# `rotation_euler = (0, radians(deg), 0)` and positive deg is the retracted leg and the shut
# door, so the graph needs the negative. plane3 is the first rig wired AFTER that was
# understood rather than before - see Tools/wire_plane6_anim.py's header for the
# measurements, and AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe for the test that
# refuses a leg folding out through the wing.
#
# THE -1 ON THE ROLLING AND SPINNING BONES is the same flip and has always been there:
# UAirsideAgentAnim accumulates an ABSOLUTE angle and the rigs turn the other way about their
# own axis.
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

MODEL = wire_anim_lib.Model("plane3", "/Game/Aircraft/Plane3/ABP_Plane3", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
