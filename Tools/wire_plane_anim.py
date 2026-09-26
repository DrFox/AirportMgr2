"""Authors ABP_Plane<N>'s AnimGraph and reads it back to prove it, for whichever key names one
of the aeroplanes wired this way - Issue #294's replacement for the twelve
wire_plane<N>_anim.py wrappers.

  python Tools/wire_plane_anim.py <key>            # wires, compiles, saves, verifies
  python Tools/wire_plane_anim.py <key> --verify   # verifies only, changes nothing
  python Tools/wire_plane_anim.py <key> --read     # prints the chain as it stands, no assertions

THE EDITOR MUST BE RUNNING; Tools/Python/build_aircraft_anim.py <key> is the half that needs it
closed, and writes Saved/<key>_axis_plan.json this one refuses to run without.

THE MECHANISM IS Tools/wire_anim_lib.py, unchanged by this file: chain order, measured axis,
node modes, and one door angle driving however many doors a rig has. WHAT USED TO BE HERE,
twelve times over - each aircraft's own (bone, variable, multiplier) PLAN, hand-typed in CHAIN
ORDER - is GONE: Tools/Python/build_aircraft_anim.py now derives all three columns from the
same `aircraft/<key>.py` spec build_aircraft_type.py reads (see airside_anim.driven_bone_plan()
and anim_multiplier_for()) and writes them, whole, into the axis-plan JSON;
wire_anim_lib.load_axis_plan() fills wire_anim_lib.Model.plan from that file rather than from
a table in this one. What is left here is the one fact this script cannot derive from that
file: the Blueprint's own package path.

EVERY KEY BUT TWO. plane1 and plane2 were wired by hand before this tooling
existed and have no Saved/<key>_axis_plan.json waiting for them - see
aircraft/plane1.py's/plane2.py's `wire_script=False` and build_aircraft_anim.py's own
docstring. Naming either here fails before touching the editor, the same refusal
wire_anim_lib.load_axis_plan() gives any key whose build step has not run.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_anim_lib

# THE KEYS THIS SCRIPT WIRES - thirteen since plane15 joined on 2026-09-26. plane1 and plane2
# are hand-wired (no axis plan is ever written for them); plane4 is create_abp=False on the BUILD side only - its Blueprint already
# exists but is wired exactly like the rest once build_aircraft_anim.py has measured
# its axes.
KEYS = ("plane3", "plane4", "plane5", "plane6", "plane7", "plane8", "plane9", "plane10",
        "plane11", "plane12", "plane13", "plane14", "plane15")

_KEY_PATTERN = re.compile(r"^plane(\d+)$")


def bp_path(key):
    """/Game/Aircraft/Plane<N>/ABP_Plane<N> - every one of the twelve follows this naming
    exactly, checked against all twelve wire_plane<N>_anim.py's own MODEL lines before this
    replaced them. Not read off aircraft/<key>.py's own AircraftSpec.abp, deliberately: that
    module imports build_aircraft_type, which imports `unreal` at module scope, and this
    script runs under plain Python against a RUNNING editor's MCP server rather than inside
    the editor's own interpreter - importing it here would fail before a single line ran."""
    match = _KEY_PATTERN.match(key)
    n = match.group(1)
    return "/Game/Aircraft/Plane%s/ABP_Plane%s" % (n, n)


def main(argv):
    positional = [a for a in argv[1:] if not a.startswith("-")]
    if not positional:
        sys.exit("usage: wire_plane_anim.py <key> [--verify|--read]\n  keys: %s"
                 % ", ".join(KEYS))
    key = positional[0]
    if key not in KEYS:
        sys.exit("%r is not one of the wired aircraft.\n  keys: %s"
                 % (key, ", ".join(KEYS)))

    model = wire_anim_lib.Model(key, bp_path(key))
    return wire_anim_lib.main(argv, model)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
