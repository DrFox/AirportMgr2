"""Says how each aircraft type gets off a stand. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

WHY THIS EXISTS. EPushbackNeed is the one thing that makes a Pushback depot a decision
rather than a tax. The light types reverse off a stand under their own power, so a new
airport needs no depot at all; the airliners need a tug, so accepting the first A320 is what
forces the building. That progression is a CONTENT decision, and this is where it is made.

IN PYTHON RATHER THAN IN BuildPiperMeridian and friends, for the reason
build_aircraft_looks.py gives at length: those builders live in Airside, and a /Game asset
path may not be named from there - Check-Architecture enforces the direction.

IN PLACE, never delete-and-recreate. DA_Aircraft_Plane7 is referenced by DA_Airline_Cumbria's
fleet, and deleting an asset something points at breaks the reference rather than updating
it. It was DA_Aircraft_Piper until 2026-09-21, and it was RENAMED rather than replaced for
exactly that reason - see build_aircraft_type.py plane7 (aircraft/plane7.py's `old_name`).
save_asset takes only_if_is_dirty=False because a headless save writes nothing at all
unless it is forced.

THE MARKER LINES ARE NOT THE EVIDENCE. Both headless save APIs report success while writing
nothing, so a line in this log says the script RAN, not that the value landed on disk. The
check that survives the script is the automation test
Airside.Content.PushbackNeedsAuthored, which loads each asset in a fresh editor and asserts
the same table this file writes. If the two ever disagree, believe the test.

THE ENTRIES ARE NO LONGER TYPED HERE. Until issue #293 this file carried its own {path:
(need, why)} table, one entry per modelled aeroplane, hand-kept in step with
AirframeAxlesTest.cpp's Expected[] table (fourteen "why" strings written out twice) and
silently missing plane4 and plane5 (neither had EVER been given a need in either language -
found only once EveryAircraftType() iterated every type with a mesh instead of a hand list).
Every modelled aeroplane's need and reason now live ONCE, on its own aircraft/<key>.py spec's
`pushback_need`/`pushback_reason` (build_aircraft_type.all_keys() is the list this file
iterates); this script's own job shrinks to WRITING what the spec says, and
Airside.Content.PushbackNeedsAuthored reads the same fields back off the same specs to
assert what actually landed, not a second copy of the table in C++.

A type absent from here (pushback_need left None on its spec, or a type with no spec at all
- the two DA_Aircraft_* PAPER types below) loads with FAirframe's own default, which is
VehicleTug - the conservative answer, because "needs a tug" of something that does not is a
missing fee, while "reverses itself" of an A320 is an airport that never needs the depot.
"""
import os
import sys

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see
# import_models.py's own note. Needed here (and not before) because this script now imports
# build_aircraft_type to read every spec's pushback_need/pushback_reason rather than typing
# its own copy of the fleet.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal  # noqa: E402

from build_aircraft_type import all_keys, spec_for  # noqa: E402

# THE TWO PAPER TYPES ALONE. DA_Aircraft_A320 and DA_Aircraft_B738 carry no mesh and no
# aircraft/<key>.py spec - EntityDefinition builds them in C++ - so there is no spec for
# either to read a pushback_need off, and they stay a small hand-typed residual rather than
# growing a second directory of paper-only specs for two entries.
PAPER_NEEDS = {
    "/Game/Entities/DA_Aircraft_A320": (
        unreal.PushbackNeed.VEHICLE_TUG,
        "an A320 is what forces the Pushback depot",
    ),
    "/Game/Entities/DA_Aircraft_B738": (
        unreal.PushbackNeed.VEHICLE_TUG,
        "and so is a 737",
    ),
}


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def set_need(type_path, need, why):
    aircraft = unreal.EditorAssetLibrary.load_asset(type_path)
    if aircraft is None:
        fail("%s absent" % type_path)
        return False

    aircraft.set_editor_property("pushback_need", need)

    # READ BACK OFF THE OBJECT BEFORE SAVING, so a property name that does not exist is a
    # loud failure here rather than a silent no-op that the save then reports as success.
    # set_editor_property raises on an unknown name, but a name that exists and is ignored
    # would not - and this codebase has been bitten by exactly that shape.
    landed = aircraft.get_editor_property("pushback_need")
    if landed != need:
        fail("%s: set %s but read back %s" % (type_path, need, landed))
        return False

    if not unreal.EditorAssetLibrary.save_asset(type_path, only_if_is_dirty=False):
        fail("%s: save reported failure" % type_path)
        return False

    say("%s: %s - %s" % (type_path.split("/")[-1], need, why))
    return True


def _all_needs():
    """PAPER_NEEDS plus every aircraft/<key>.py spec that has decided a pushback_need - the
    ONE list this script writes from, built fresh each run rather than typed here so a new
    spec (or a changed one) needs no edit to this file at all."""
    needs = dict(PAPER_NEEDS)
    for key in all_keys():
        spec = spec_for(key)
        if spec.pushback_need is None:
            # NOT YET DECIDED, and left alone - the type keeps FAirframe's own VehicleTug
            # default, exactly as an absent NEEDS entry always has.
            continue
        need = getattr(unreal.PushbackNeed, spec.pushback_need)
        needs["/Game/Entities/%s" % spec.type_name] = (need, spec.pushback_reason)
    return needs


def run():
    needs = _all_needs()
    done = 0
    for type_path, (need, why) in sorted(needs.items()):
        if set_need(type_path, need, why):
            done += 1

    say("%d of %d aircraft type(s) given a pushback need" % (done, len(needs)))
    say("now run Airside.Content.PushbackNeedsAuthored - THAT is the evidence, not this line")


run()
