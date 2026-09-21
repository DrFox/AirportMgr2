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
# THE GEAR AND DOOR BONES TAKE -1 TOO, AND THE FLEET'S OTHER RIGS DO NOT. They should: this
# is the same flip the rolling bones carry, and it was missed on the travelling ones because
# until PR #250's animation bench nothing in the editor drove them where a human could look.
#
# WHY THERE IS A FLIP AT ALL. Blender is right-handed and UE is left-handed, so the import is
# a MIRROR, not just a rotation - and a mirror reverses the sense of every rotation about a
# bone's own axis. Every build_rig.py in the models repo keys its cycle with one line,
#
#     pb.rotation_euler = (0.0, math.radians(deg), 0.0)   # about the bone's own Y
#
# and POSITIVE deg is the retracted leg and the shut door. In UE that same positive turns the
# other way, which is exactly what "the rigs turn the other way about their own axis" has
# always said of the wheels.
#
# MEASURED, NOT ARGUED, and the control came first. Turning each bone by the authored angle
# off SK_Plane6's reference pose and watching where the child lands:
#
#     gear_L    +90 lifts wheel_L2 274 uu and 494 uu OUTBOARD;  -90 lifts it 494 and inboard
#     gear_nose +90 lifts nosewheel 234 uu and 366 uu AFT;      -90 lifts it 366 and forward
#     door_main_L  +81 swings the panel INTO the fuselage;      -81 lays it flush across the bay
#     door_nose_L  +81 swings it outboard;                      -81 closes it on the centreline
#
# The -90 column is plane6/scripts/gear_pivots.json's own retracted pose to the decimal - it
# says the left leg rotates +90 about world +Y and puts the wheel 4.94 m up - so the model and
# the graph disagreed by a sign and nothing else. THE PROBE WAS VALIDATED FIRST against the
# one bone whose direction is known correct on screen: with the fleet's -1 the wheels roll
# FORWARD on all four rigs, and without it they roll backwards.
#
# SO THE MIRRORING IS STILL IN THE BONE and this multiplier is not undoing it. gear_L points
# +Y and gear_R points -Y, so ONE GearAngleDegrees still folds both inboard; gear_nose points
# -X, so the same figure folds the nose leg FORWARD, which is what a 777 does. What -1 fixes
# is the handedness of the whole rig, not the symmetry of a pair.
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
    ("gear_L",          "GearAngleDegrees",    -1.0),
    ("gear_R",          "GearAngleDegrees",    -1.0),
    ("gear_nose",       "GearAngleDegrees",    -1.0),
    ("door_main_L",     "BayDoorAngleDegrees", -1.0),
    ("door_main_R",     "BayDoorAngleDegrees", -1.0),
    ("door_nose_L",     "BayDoorAngleDegrees", -1.0),
    ("door_nose_R",     "BayDoorAngleDegrees", -1.0),
]

MODEL = wire_anim_lib.Model("plane6", "/Game/Aircraft/Plane6/ABP_Plane6", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
