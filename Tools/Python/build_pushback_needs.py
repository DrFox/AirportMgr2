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
exactly that reason - see Tools/Python/build_plane7_type.py. save_asset takes only_if_is_dirty=False because a headless save writes nothing at all
unless it is forced.

THE MARKER LINES ARE NOT THE EVIDENCE. Both headless save APIs report success while writing
nothing, so a line in this log says the script RAN, not that the value landed on disk. The
check that survives the script is the automation test
Airside.Content.PushbackNeedsAuthored, which loads each asset in a fresh editor and asserts
the same table this file writes. If the two ever disagree, believe the test.

THE ENTRIES BELOW ARE THE WHOLE LIST. A type absent from here loads with FAirframe's own
default, which is VehicleTug - the conservative answer, because "needs a tug" of something
that does not is a missing fee, while "reverses itself" of an A320 is an airport that never
needs the depot.

IT SAID "THE FIVE ENTRIES" UNTIL 2026-09-21 AND THERE WERE SIX, which is why it no longer
says a number. A count in prose beside a list is a second statement of the list's length with
nothing keeping the two in step, and it goes stale on the first addition - CLAUDE.md's rule
about naming rather than counting, in the smallest possible form.
"""
import unreal

# type asset -> (need, why)
#
# The why is not decoration: it is the gameplay reason the value is what it is, and the
# automation test asserts the same table with the same reasons as its failure messages.
NEEDS = {
    "/Game/Entities/DA_Aircraft_Plane1": (
        unreal.PushbackNeed.SELF_MANOEUVRE,
        "a 172 is pushed off a stand by one person leaning on the strut, and the class "
        "default of VehicleTug would gate the smallest aeroplane in the game behind the "
        "depot",
    ),
    "/Game/Entities/DA_Aircraft_Plane7": (
        unreal.PushbackNeed.SELF_MANOEUVRE,
        "the starter aeroplane reverses itself, so a new airport needs no depot",
    ),
    "/Game/Entities/DA_Aircraft_Plane2": (
        unreal.PushbackNeed.SELF_MANOEUVRE,
        "a Twin Otter beta-ranges off a stand",
    ),
    "/Game/Entities/DA_Aircraft_Plane3": (
        unreal.PushbackNeed.SELF_MANOEUVRE,
        "a Q400 turns out of a regional stand on its own props; the depot is the jets' tax",
    ),
    "/Game/Entities/DA_Aircraft_Plane6": (
        unreal.PushbackNeed.VEHICLE_TUG,
        "a 350 t 777-300ER is the far end of the same argument - and it is here rather than "
        "left to the class default on purpose: an unset field that happens to agree with the "
        "default is indistinguishable from an asset nobody authored, which is the point the "
        "Plane7 row already makes from the other side",
    ),
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


def run():
    done = 0
    for type_path, (need, why) in sorted(NEEDS.items()):
        if set_need(type_path, need, why):
            done += 1

    say("%d of %d aircraft type(s) given a pushback need" % (done, len(NEEDS)))
    say("now run Airside.Content.PushbackNeedsAuthored - THAT is the evidence, not this line")


run()
