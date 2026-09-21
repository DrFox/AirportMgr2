"""Authors ABP_Plane4's AnimGraph - eleven bones - and reads it back to prove it.

  python Tools/wire_plane4_anim.py            # wires, compiles, saves, verifies
  python Tools/wire_plane4_anim.py --verify   # verifies only, changes nothing
  python Tools/wire_plane4_anim.py --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_plane4_anim.py is the half that needs it
closed, and writes the measured rotation axes this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py. Read that file for the four decisions a wiring run
makes - chain order, measured axis, node modes, and one door angle driving every door.

THIS REPLACES A HAND-AUTHORED GRAPH, WHICH NO OTHER wire_plane<N>_anim.py DOES. ABP_Plane4
was duplicated from ABP_Plane2 in the editor on 2026-09-19 and retargeted by hand, before any
of this tooling existed, so nothing recorded which axis it drove or why - and what it in fact
drove was the gear the WRONG WAY. Run `--read` before the first wiring if you want the old
chain in the log; after that the graph is generated from the measured plan like every other.

WHAT THE HAND-WIRED GRAPH ACTUALLY HELD, read off it with --read before it was replaced, and
it is the most interesting thing in this file:

    prop_L, prop_R    GetPropAngleDegrees()               <- NO -1
    wheel_L/R, nose   GetWheelAngleDegrees()   x -1
    nosewheel_steer   GetSteerAngleDegrees()
    gear_L, gear_R    GetGearAngleDegrees()    x -1
    gear_nose         GetGearAngleDegrees()               <- NO -1
    door_nose_L/R     GetBayDoorAngleDegrees() x -1

SO IT WAS THREE QUARTERS RIGHT, BY EYE, AND THAT IS WORSE THAN BEING WRONG. Somebody wiring
this by hand in the editor could SEE the main legs swing out through the wing and negated
them; they could see the nose doors and negated those; and the two things that stayed wrong
are the two nobody would catch by looking. gear_nose folds the leg AFT where a 737's retracts
FORWARD - visible only from underneath, mid-cycle - and the fans turn BACKWARDS, which on a
24-blade disc at a capped display rate reads as a fan turning. A per-bone fix by eye leaves
exactly the bones that are hard to see, and leaves no record of which were considered.

WHY THE SIGN IS NEEDED AT ALL. Blender is right-handed and UE is left-handed, so the import
MIRRORS every bone-local rotation: the model's positive is the graph's negative, for every
bone on every rig. It is one fact, and the table above is what happens when it is discovered
four times separately instead. plane6's 777 is what finally named it - see
Tools/wire_plane6_anim.py's header for the measurements and the control that validated the
probe, and AirportMgr.View.AnimYard.GearFoldsIntoTheAirframe for the test that now pins the
fold direction so the legs, at least, cannot go back.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# (bone, variable, multiplier) in CHAIN ORDER, source first. None drives the axis directly.
#
# THE ORDER IS THE HIERARCHY, BOTTOM UP: a bone goes after every bone it is the parent of, so
# the wheels precede their legs and nosewheel_steer sits strictly between nosewheel and
# gear_nose. Rotate a leg before the wheel it carries and the wheel spins inside the bay.
#
# ELEVEN BONES, NOT THIRTEEN, and the two missing ones are not an omission. This rig has NO
# MAIN BAY DOORS: a 737's main wheels sit in a well behind a fixed fairing, so there is
# nothing to hinge and the rig has only door_nose_L/_R. plane5 and plane7 are the ones with
# four doors.
#
# `prop_L`/`prop_R` DRIVE TURBOFANS. The meshes are honestly called fan_L/fan_R and the BONES
# carry "prop" so that BONE_RULES' substring match finds the driver that already exists;
# build_plane4_type.py records the same trade from the measuring side.
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
    ("door_nose_L",     "BayDoorAngleDegrees", -1.0),
    ("door_nose_R",     "BayDoorAngleDegrees", -1.0),
]

MODEL = wire_anim_lib.Model("plane4", "/Game/Aircraft/Plane4/ABP_Plane4", PLAN)


if __name__ == "__main__":
    sys.exit(wire_anim_lib.main(sys.argv, MODEL))
