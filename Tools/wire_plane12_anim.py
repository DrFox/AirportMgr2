"""Authors ABP_Plane12's AnimGraph - five bones - and reads it back to prove it.

  python Tools/wire_plane12_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane12_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane12_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane12_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. WHAT IS LEFT HERE is plane12's own: its Blueprint and
its five bones in chain order - plane10's set, bone for bone.

FIXED GEAR, SO NO RETRACT ROWS. The Cherokee's legs are welded on; its rig has no gear or door
bone, and DA_Aircraft_Plane12 declares no gear cycle. The graph is the prop, the three tyres
and the steer.

THE -1 IS HANDEDNESS: Blender is right-handed and UE left-handed, so the model's positive is
the graph's negative on every spinning bone. See wire_plane6_anim.py's header for the
measurement. The steer takes the angle unnegated, as ABP_Plane2's shipped graph does.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP: the prop; the wheels; then nosewheel_steer, the
# parent of nosewheel, so rotating it last carries the already-rolled tyre round with it.
PLAN = [
    ("prop",            "PropAngleDegrees",    -1.0),
    ("wheel_L",         "WheelAngleDegrees",   -1.0),
    ("wheel_R",         "WheelAngleDegrees",   -1.0),
    ("nosewheel",       "WheelAngleDegrees",   -1.0),
    ("nosewheel_steer", "SteerAngleDegrees",   None),
]

MODEL = wire_anim_lib.Model("plane12", "/Game/Aircraft/Plane12/ABP_Plane12", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
