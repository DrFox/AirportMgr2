"""Places ASunDriver in M_Starter and points it at the level's sun. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

THE SUN POINTER IS THE WHOLE POINT OF THIS SCRIPT. ASunDriver deliberately does not hunt
for a directional light with a TActorIterator - a search silently picks one of two lights,
and the wrong choice looks exactly like the feature not working. So the pointer is
level-authored, and something has to author it.

Idempotent: re-running replaces any existing driver rather than stacking a second one that
would fight the first for the same light.
"""
import unreal

LEVEL = "/Game/Maps/M_Starter"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    for old in [a for a in actors.get_all_level_actors() if isinstance(a, unreal.SunDriver)]:
        actors.destroy_actor(old)
        say("removed an existing ASunDriver")

    suns = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.DirectionalLight)]
    if len(suns) != 1:
        fail("expected exactly 1 DirectionalLight, found %d - the driver needs to be told "
             "which one, and this script will not guess" % len(suns))
        say("DONE")
        return

    driver = actors.spawn_actor_from_class(unreal.SunDriver, unreal.Vector(0.0, 0.0, 0.0))
    driver.set_actor_label("SunDriver")
    driver.set_editor_property("sun", suns[0])
    levels.save_current_level()
    say("spawned ASunDriver pointing at %s" % suns[0].get_actor_label())

    # Reload and read it back: a level edit that reports success and writes nothing is the
    # known failure mode here, and a null Sun would only show up as the day cycle quietly
    # doing nothing at runtime.
    levels.load_level(LEVEL)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.SunDriver)]
    if len(found) != 1:
        fail("expected exactly 1 ASunDriver after reload, found %d" % len(found))
        say("DONE")
        return
    wired = found[0].get_editor_property("sun")
    if wired is None:
        fail("ASunDriver.Sun is empty after reload")
    else:
        say("PASS ASunDriver.Sun = %s" % wired.get_actor_label())
        say("ALL VERIFIED")
    say("DONE")


run()
