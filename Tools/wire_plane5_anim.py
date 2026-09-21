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
#
# THE GEAR AND DOOR BONES TAKE -1 SINCE 2026-09-21, AND THEY TOOK None BEFORE IT. That was a
# sign error, and it had been on screen in nobody's view: until PR #250's animation bench
# there was nothing in the editor that drove a retract, so the whole travelling half of this
# table had never been WATCHED. plane6's 777 is what exposed it.
#
# Blender is right-handed and UE is left-handed, so the import MIRRORS every bone-local
# rotation - the model's positive is the graph's negative. That is the same flip the rolling
# bones above have always carried, and "the rigs turn the other way about their own axis" was
# always a statement about the whole rig rather than about wheels. Measured off this rig's
# reference pose, at +GearAngleDegrees both main legs swung OUTBOARD, through the wing they
# retract into; at -1 they fold inboard, which is the pose the model keys.
#
# THE MIRRORING IS STILL IN THE BONE and this does not undo it: gear_L points one way and
# gear_R the other, so ONE GearAngleDegrees still folds the pair inboard. What -1 fixes is the
# handedness of the rig, not the symmetry of a pair. See Tools/wire_plane6_anim.py's header
# for the measurements and the control that established it, and
# AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe for the test that now pins it.
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

MODEL = wire_anim_lib.Model("plane5", "/Game/Aircraft/Plane5/ABP_Plane5", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
