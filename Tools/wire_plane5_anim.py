"""Authors ABP_Plane5's AnimGraph - thirteen bones - and reads it back to prove it.

  python Tools/wire_plane5_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane5_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane5_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane5_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM MOVED TO Tools/wire_anim_lib.py ON 2026-09-21, when plane7 wanted it and the
alternative was a third copy of four hundred lines. Read that file for the four decisions a
wiring run makes - chain order, measured axis, node modes, and one door angle driving four
doors - and for why wire_fueltruck_anim.py was left as it is. WHAT IS LEFT HERE is plane5's
own: its Blueprint and its thirteen bones in chain order.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP. Read it as three groups:
#
#   props            - no children, no constraint, first because they are the simplest
#   wheels           - children of the retract bones, so they MUST precede them
#   nosewheel_steer  - parent of nosewheel, child of gear_nose: strictly between the two
#   retract bones    - parents of everything above them on this rig
#   doors            - children of root, unconstrained; last because they are independent
#
# THE -1 ON THE ROLLING AND SPINNING BONES matches every such bone in the fleet:
# UAirsideAgentAnim accumulates an ABSOLUTE angle and the rigs turn the other way about their
# own axis. It is a multiplier and not a negated axis on purpose - the axis column is
# measured and this is a convention, and mixing the two would make the measurement unreadable.
#
# THE GEAR AND DOOR BONES TAKE None. Their sense is already carried by the rest direction the
# axis column reports: GearAngleDegrees runs 0 (down) to +90 (retracted) and
# BayDoorAngleDegrees 0 (open) to +90 (shut), and a leg that folds aft has its axis pointing
# the other way rather than its number negated. See plane5/scripts/build_rig.py, and
# UAirsideAgentAnim::GearAnglesFrom for which fraction is inverted and why.
PLAN = [
    ("prop_L",          "PropAngleDegrees",    -1.0),
    ("prop_R",          "PropAngleDegrees",    -1.0),
    ("wheel_L",         "WheelAngleDegrees",   -1.0),
    ("wheel_R",         "WheelAngleDegrees",   -1.0),
    ("nosewheel",       "WheelAngleDegrees",   -1.0),
    ("nosewheel_steer", "SteerAngleDegrees",   None),
    ("gear_L",          "GearAngleDegrees",    None),
    ("gear_R",          "GearAngleDegrees",    None),
    ("gear_nose",       "GearAngleDegrees",    None),
    ("door_main_L",     "BayDoorAngleDegrees", None),
    ("door_main_R",     "BayDoorAngleDegrees", None),
    ("door_nose_L",     "BayDoorAngleDegrees", None),
    ("door_nose_R",     "BayDoorAngleDegrees", None),
]

MODEL = wire_anim_lib.Model("plane5", "/Game/Aircraft/Plane5/ABP_Plane5", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
