"""Authors ABP_Plane7's AnimGraph - twelve bones - and reads it back to prove it.

  python Tools/wire_plane7_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane7_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane7_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane7_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. Read that file for the four decisions a wiring run
makes - chain order, measured axis, node modes, and one door angle driving four doors. WHAT
IS LEFT HERE is plane7's own: its Blueprint and its twelve bones in chain order.

THE ASSET THIS REPLACES HAD NO SCRIPT. ABP_PiperMeridian was wired by hand in the editor
before any of this tooling existed, so nothing recorded which axis it drove, in which order,
or why - and re-deriving it from the old graph would have been inheriting an undocumented
answer about a mesh that is being deleted anyway. Everything below is measured off SK_Plane7
or argued here.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP, and on this rig it binds twice:
#
#   prop             - no children, no constraint, first because it is the simplest
#   wheels           - children of the retract bones, so they MUST precede them
#   nosewheel_steer  - parent of nosewheel, child of gear_nose: strictly between the two
#   retract bones    - parents of everything above them
#   doors            - children of root, unconstrained; last because they are independent
#
# ONE prop, NOT A PAIR, and that is the only structural difference from plane5's thirteen. A
# Meridian is single-engine. The variable is unchanged: PropAngleDegrees drives however many
# propeller bones a rig has, and the count has never been part of the contract.
#
# THE -1 ON THE ROLLING AND SPINNING BONES matches every such bone in the fleet:
# UAirsideAgentAnim accumulates an ABSOLUTE angle and the rigs turn the other way about their
# own axis. It is a multiplier and not a negated axis on purpose - the axis column is
# measured and this is a convention, and mixing the two would make the measurement unreadable.
#
# THE GEAR AND DOOR BONES TAKE None. Their sense is already carried by the rest direction the
# axis column reports: GearAngleDegrees runs 0 (down) to +90 (retracted) and
# BayDoorAngleDegrees 0 (open) to +90 (shut). On THIS rig both main legs fold INBOARD and the
# nose leg folds AFT off one positive number, because gear_L points +Y, gear_R points -Y and
# gear_nose points +X - the mirroring is in the bone, never in the figure. plane5's King Air
# folds FORWARD instead, and needs no different row here for it. See
# plane7/scripts/build_rig.py, and UAirsideAgentAnim::GearAnglesFrom for which fraction is
# inverted and why.
PLAN = [
    ("prop",            "PropAngleDegrees",    -1.0),
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

MODEL = wire_anim_lib.Model("plane7", "/Game/Aircraft/Plane7/ABP_Plane7", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
