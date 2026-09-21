"""Authors ABP_Plane6's AnimGraph - seventeen bones - and reads it back to prove it.

  python Tools/wire_plane6_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane6_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane6_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane6_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. Read that file for the four decisions a wiring run
makes - chain order, measured axis, node modes, and one door angle driving four doors. WHAT
IS LEFT HERE is plane6's own: its Blueprint and its seventeen bones in chain order.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP, and on this rig it binds harder than on any other:
#
#   fans             - no children, no constraint, first because they are the simplest
#   main wheels      - SIX of them, three per leg, children of the retract bones
#   nosewheel        - child of nosewheel_steer
#   nosewheel_steer  - parent of nosewheel, child of gear_nose: strictly between the two
#   retract bones    - parents of everything above them
#   doors            - children of root, unconstrained; last because they are independent
#
# SIX MAIN WHEELS, NOT TWO, and that is the only structural difference from plane5's and
# plane7's thirteen. A 777's main leg carries a three-axle bogie, and a bone turns about its
# OWN length - so one bone cannot roll three axles without the outer two orbiting it rather
# than spinning. All six take the one WheelAngleDegrees. The count of bones a variable drives
# has never been part of the contract: plane7 proved the other end of it with a single prop.
#
# `prop_L`/`prop_R` DRIVE TURBOFANS, not propellers, and the name is deliberate - the meshes
# are honestly called fan_L/fan_R and the BONES carry "prop" so that BONE_RULES' substring
# match finds the driver that already exists. plane4 records the same trade.
#
# THE -1 ON THE ROLLING AND SPINNING BONES matches every such bone in the fleet:
# UAirsideAgentAnim accumulates an ABSOLUTE angle and the rigs turn the other way about their
# own axis. It is a multiplier and not a negated axis on purpose - the axis column is
# measured and this is a convention, and mixing the two would make the measurement unreadable.
#
# THE GEAR AND DOOR BONES TAKE None. Their sense is already carried by the rest direction the
# axis column reports: GearAngleDegrees runs 0 (down) to +90 (retracted) and
# BayDoorAngleDegrees 0 (open) to +90 (shut). On THIS rig both main legs fold INBOARD and the
# nose leg folds FORWARD off one positive number, because gear_L points +Y, gear_R points -Y
# and gear_nose points -X - the mirroring is in the bone, never in the figure. plane7's
# Meridian folds its nose leg AFT instead, and needs no different row here for it.
#
# NO TRUCK ROW. The bogies are rigid on their legs - see build_plane6_anim.py's header - so
# there is no bone for TruckTiltAngleDegrees to drive and FGearPerformance::TruckTiltSeconds
# stays zero. plane8's A380 is where that changes.
PLAN = [
    ("prop_L",          "PropAngleDegrees",    -1.0),
    ("prop_R",          "PropAngleDegrees",    -1.0),
    ("wheel_L1",        "WheelAngleDegrees",   -1.0),
    ("wheel_L2",        "WheelAngleDegrees",   -1.0),
    ("wheel_L3",        "WheelAngleDegrees",   -1.0),
    ("wheel_R1",        "WheelAngleDegrees",   -1.0),
    ("wheel_R2",        "WheelAngleDegrees",   -1.0),
    ("wheel_R3",        "WheelAngleDegrees",   -1.0),
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

MODEL = wire_anim_lib.Model("plane6", "/Game/Aircraft/Plane6/ABP_Plane6", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
