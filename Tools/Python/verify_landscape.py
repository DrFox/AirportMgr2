"""Asserts the hand-made Landscape matches the spec. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

The Landscape cannot be created from Python - ALandscapeProxy::Import is editor-only C++
with no UFUNCTION (LandscapeProxy.h:1418, inside the #if WITH_EDITOR at 1326), and
everything ALandscape does expose to script is read-only. So it is made by hand, which
means nothing but this script stands between a mistyped component count and a plot that is
silently the wrong size.

The two easy mistakes it catches by name: leaving Sections Per Component at 1x1 gives 144
components over half the ground, and typing 24x24 components gives 576 over twice it.
"""
import unreal

LEVEL = "/Game/Maps/M_Starter"

EXPECTED_COMPONENTS = 144
EXPECTED_SCALE_XY = 200.0
EXPECTED_METRES = 3024.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def run():
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.Landscape)]
    if len(found) != 1:
        fail("expected exactly 1 Landscape, found %d" % len(found))
        say("DONE")
        return

    land = found[0]
    ok = True

    comps = land.get_components_by_class(unreal.LandscapeComponent)
    if len(comps) != EXPECTED_COMPONENTS:
        fail("component count is %d, expected %d - 36 means Sections Per Component was "
             "left at 1x1, 576 means Number of Components was 24x24"
             % (len(comps), EXPECTED_COMPONENTS))
        ok = False
    else:
        say("PASS component count = %d" % len(comps))

    scale = land.get_actor_scale3d()
    if abs(scale.x - EXPECTED_SCALE_XY) > 1e-3 or abs(scale.y - EXPECTED_SCALE_XY) > 1e-3:
        fail("scale is %r, expected X and Y of %r" % (scale, EXPECTED_SCALE_XY))
        ok = False
    else:
        say("PASS scale = (%.1f, %.1f, %.1f)" % (scale.x, scale.y, scale.z))

    origin, extent = land.get_actor_bounds(False)
    metres_x = (extent.x * 2.0) / 100.0
    metres_y = (extent.y * 2.0) / 100.0
    if abs(metres_x - EXPECTED_METRES) > 20.0:
        fail("extent is %.0f m across, expected about %.0f m" % (metres_x, EXPECTED_METRES))
        ok = False
    else:
        say("PASS extent = %.0f x %.0f m" % (metres_x, metres_y))

    loc = land.get_actor_location()
    if abs(loc.z) > 1e-3:
        fail("landscape Z is %r, expected 0 - roads sit at SurfaceZ 10 uu above it" % loc.z)
        ok = False
    else:
        say("PASS landscape Z = 0")

    say("ALL VERIFIED" if ok else "VERIFY FAILED")
    say("DONE")


run()
