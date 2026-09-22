"""Places AAirsideBuildingsActor in M_Starter, bound to its road network. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. Every result line is prefixed MARKER: so it can be grepped out of
Saved/Logs/AirportMgr.log.

WHY IT IS PLACED AND NOT ONLY SPAWNED: the drivers FindOrCreate one, but outside the build
mode the editor viewport shows the level as saved - and a depot with no buildings actor is
invisible there. THE RESAVE IS THE SECOND POINT: M_Starter was saved with the road network's
old PlotBoxes component, which ARoadNetworkActor now sweeps on registration; saving after the
sweep drops it from disk.

Idempotent, like place_sun_driver.py: an existing buildings actor is replaced, never stacked.
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

    for old in [a for a in actors.get_all_level_actors() if isinstance(a, unreal.AirsideBuildingsActor)]:
        actors.destroy_actor(old)
        say("removed an existing AAirsideBuildingsActor")

    roads = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.RoadNetworkActor)]
    if len(roads) != 1:
        fail("expected exactly 1 RoadNetworkActor, found %d - will not guess" % len(roads))
        say("DONE")
        return

    buildings = actors.spawn_actor_from_class(unreal.AirsideBuildingsActor, unreal.Vector(0.0, 0.0, 0.0))
    buildings.set_actor_label("AirsideBuildings")
    buildings.set_editor_property("road_network", roads[0])
    levels.save_current_level()
    say("spawned AAirsideBuildingsActor bound to %s" % roads[0].get_actor_label())

    # Reload and read back: a level edit that reports success and writes nothing is the known
    # failure mode here (a locked .umap does exactly that).
    levels.load_level(LEVEL)
    found = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.AirsideBuildingsActor)]
    if len(found) != 1:
        fail("expected exactly 1 AAirsideBuildingsActor after reload, found %d" % len(found))
        say("DONE")
        return
    bound = found[0].get_editor_property("road_network")
    if bound is None:
        fail("AAirsideBuildingsActor.RoadNetwork is empty after reload")
    else:
        say("PASS AAirsideBuildingsActor.RoadNetwork = %s" % bound.get_actor_label())
        say("ALL VERIFIED")
    say("DONE")


run()
