"""Authors /Game/Maps/M_RigTest - the level ARigTestCourse drives on. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. With it open, save_current_level() returns False and the save
fails with a sharing violation; EditorLoadingAndSavingUtils.save_map is worse, returning
without complaint and writing nothing (memory: unreal-editing-levels-headlessly). This
script RELOADS AND COUNTS what it wrote for exactly that reason.

WHAT THE LEVEL HOLDS AND WHY. ARigTestCourse lays its own roads at BeginPlay through
IRoadEditTarget (RigTestCourse.h's class comment: "THE COURSE IS CODE, NOT LEVEL DATA"), so
this script's job is only the four things the course itself cannot supply: somewhere for it
to stand on, something to lay roads INTO, the course actor, and the game mode that gives PIE
its road-build keys and camera. Modelled on build_model_yard.py's build_bench()/verify()
pattern - the same silent-success trap applies to a fresh level as to a re-authored one.

THE FLOOR SIZE AND POSITION ARE READ OFF RigTestCourse.cpp's OWN CONSTANTS (RigCourse
namespace), not eyeballed - see COURSE_EXTENTS_UU below for the derivation. If that file's
layout constants change, re-derive this block from them; a floor that stops covering the
course reads as "the road build failed" when it did not.

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
"""
import unreal

LEVEL = "/Game/Maps/M_RigTest"

# ---------------------------------------------------------------------------------------
# COURSE EXTENTS, uu (1 uu = 1 cm), derived from Source/AirportMgr/RigTestCourse.cpp's
# RigCourse namespace constants as of 2026-09-25. Recompute this block if that file's
# layout constants move.
#
#   LaneStraight=8000  CornerRise=3000  StemLength=3000  UpperRise=3000  ExitRun=6000
#   LaneLength = LaneStraight+ExitRun = 14000        (P0..P4, one lane, local X)
#   LaneHeight = CornerRise+UpperRise = 6000         (one lane, local Y)
#   TierGap=6000  TierPitch = LaneHeight+TierGap = 12000
#   EastLinkX=16000  WestLinkX=-3000  ReturnEastX=20000  ReturnWestX=-5000  ReturnSouthY=-4000
#
# Walking every node BuildCourse places (three lanes at Tier*TierPitch, the east/west links,
# the return road): X ranges ReturnWestX..ReturnEastX = -5000..20000; Y ranges
# ReturnSouthY..(2*TierPitch+LaneHeight) = -4000..30000 (the top of tier 2's lane, R0's own Y).
# So the course's own bounding box is 25000 x 34000 uu (250 x 340 m).
COURSE_MIN_X = -5000.0
COURSE_MAX_X = 20000.0
COURSE_MIN_Y = -4000.0
COURSE_MAX_Y = 30000.0

# The floor is centred on the course's own centre, not on the world origin (unlike
# build_model_yard.py's yard, which IS centred on the origin because nothing else pins its
# rows there) - ARigTestCourse lays its nodes at RigNetworkActor's absolute world XY
# (RoadNetworkActor.h: "its transform stays at the world origin"), so the course itself picks
# where on the floor it sits, and the floor must follow it, not the other way round.
COURSE_CENTRE_X = (COURSE_MIN_X + COURSE_MAX_X) * 0.5
COURSE_CENTRE_Y = (COURSE_MIN_Y + COURSE_MAX_Y) * 0.5

# MARGIN: about 125 m clear on every side of the course's own 250 x 340 m box, so the dead-end
# balloons (which reach past the lane ends - UTurnGeom's own comment) and a rig overshooting a
# refused leg both stay on pavement-adjacent ground rather than driving off the edge into the
# void. Generous rather than measured exactly, the same choice build_model_yard.py's
# FLOOR_X_M/FLOOR_Y_M comment makes for the same reason.
FLOOR_X_M = 500.0
FLOOR_Y_M = 600.0

FLOOR_MATERIAL = "/Game/Environment/M_Ground"
FLOOR_MESH = "/Engine/BasicShapes/Plane"

# Lighting, copied from build_model_yard.py's build_lighting() values rather than reinvented -
# this is a second dev-tooling map judged under the same light as the yard and the game, not a
# new backdrop to tune by eye.
SUN_PITCH = -42.0
SUN_YAW = 150.0
SUN_TEMPERATURE = 5800.0
SUN_SOURCE_ANGLE = 1.5
SUN_INTENSITY = 2.487
FOG_DENSITY = 0.005
FOG_HEIGHT_FALLOFF = 0.2

COURSE_CLASS = "/Script/AirportMgr.RigTestCourse"
NETWORK_CLASS = "/Script/Airside.RoadNetworkActor"
GAME_MODE = "/Game/BP_RoadBuildGameMode.BP_RoadBuildGameMode_C"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def levels():
    return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def world_settings():
    """The level's AWorldSettings, or None.

    NOT THROUGH get_all_level_actors, which does not return it (memory:
    unreal-editing-levels-headlessly, measured 2026-09-21 against M_ModelYard: 27 actors and
    WorldSettings not among them). UWorld::GetWorldSettings is reflected and answers directly.
    """
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    return world.get_world_settings() if world is not None else None


def start_level():
    """A clean level to author into, whether or not one is already there.

    RE-AUTHORED FROM EMPTY, like build_model_yard.py's start_level(): this script owns every
    actor in the map, and a re-run must not leave a previous run's actors behind it.
    """
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        levels().load_level(LEVEL)
        existing = actors().get_all_level_actors()
        for actor in existing:
            actors().destroy_actor(actor)
        say("cleared %d actor(s) from the existing %s" % (len(existing), LEVEL))
        return True
    if not levels().new_level(LEVEL):
        fail("could not create %s" % LEVEL)
        return False
    say("created %s" % LEVEL)
    return True


def build_lighting():
    sun = actors().spawn_actor_from_class(
        unreal.DirectionalLight, unreal.Vector(0.0, 0.0, 2000.0))
    sun.set_actor_label("Sun")
    sun.set_actor_rotation(unreal.Rotator(0.0, SUN_PITCH, SUN_YAW), False)
    comp = sun.get_component_by_class(unreal.DirectionalLightComponent)
    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    comp.set_editor_property("atmosphere_sun_light", True)
    comp.set_editor_property("use_temperature", True)
    comp.set_editor_property("temperature", SUN_TEMPERATURE)
    comp.set_editor_property("light_source_angle", SUN_SOURCE_ANGLE)
    comp.set_editor_property("intensity", SUN_INTENSITY)

    sky = actors().spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0.0, 0.0, 2000.0))
    sky.set_actor_label("SkyLight")
    sky_comp = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    sky_comp.set_editor_property("real_time_capture", True)

    atmosphere = actors().spawn_actor_from_class(
        unreal.SkyAtmosphere, unreal.Vector(0.0, 0.0, 0.0))
    atmosphere.set_actor_label("SkyAtmosphere")

    fog = actors().spawn_actor_from_class(
        unreal.ExponentialHeightFog, unreal.Vector(0.0, 0.0, 0.0))
    fog.set_actor_label("HeightFog")
    fog_comp = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    fog_comp.set_editor_property("fog_density", FOG_DENSITY)
    fog_comp.set_editor_property("fog_height_falloff", FOG_HEIGHT_FALLOFF)

    say("lighting: sun pitch %.0f yaw %.0f intensity %.3f, sky light real-time, atmosphere, fog"
        % (SUN_PITCH, SUN_YAW, SUN_INTENSITY))


def build_floor():
    """A floor plane sized and centred to cover the course's own bounding box - see
    COURSE_EXTENTS_UU above."""
    mesh = unreal.EditorAssetLibrary.load_asset(FLOOR_MESH)
    if mesh is None:
        fail("no floor mesh at %s - the course would have no ground under it" % FLOOR_MESH)
        return
    floor = actors().spawn_actor_from_class(
        unreal.StaticMeshActor, unreal.Vector(COURSE_CENTRE_X, COURSE_CENTRE_Y, 0.0))
    floor.set_actor_label("Floor")
    comp = floor.static_mesh_component
    comp.set_editor_property("static_mesh", mesh)
    material = unreal.EditorAssetLibrary.load_asset(FLOOR_MATERIAL)
    if material is not None:
        comp.set_material(0, material)
    else:
        say("NOTE %s would not load; the floor keeps the engine default material" % FLOOR_MATERIAL)
    floor.set_actor_scale3d(unreal.Vector(FLOOR_X_M, FLOOR_Y_M, 1.0))
    say("floor: %.0f x %.0f m, centred at (%.0f, %.0f) m to cover the course's %.0f x %.0f m box"
        % (FLOOR_X_M, FLOOR_Y_M, COURSE_CENTRE_X / 100.0, COURSE_CENTRE_Y / 100.0,
           (COURSE_MAX_X - COURSE_MIN_X) / 100.0, (COURSE_MAX_Y - COURSE_MIN_Y) / 100.0))


def build_start():
    """A PlayerStart, which PIE wants and BP_RoadBuildGameMode's own pawn/controller then
    place a camera from - see build_model_yard.py's build_start() for the same reasoning
    applied to a different game mode. Off to one side so it is not sitting on a road node."""
    start = actors().spawn_actor_from_class(
        unreal.PlayerStart, unreal.Vector(COURSE_CENTRE_X, COURSE_MIN_Y - 3000.0, 400.0))
    start.set_actor_label("PlayerStart")


def build_network_and_course():
    """The road network actor and the course actor that lays roads onto it at BeginPlay.

    ORDER DOES NOT MATTER for the actors themselves - ARigTestCourse::ResolveNetworkActor
    finds the network by ARoadNetworkActor::Find(GetWorld()) in its own BeginPlay/Tick, not
    by construction order - but the network actor is spawned first anyway so a reader sees
    what-lays-into-what in the order the log will report it.
    """
    network_class = unreal.load_class(None, NETWORK_CLASS)
    if network_class is None:
        fail("%s not found - build AirportMgrEditor before running this" % NETWORK_CLASS)
        return False
    network = actors().spawn_actor_from_class(network_class, unreal.Vector(0.0, 0.0, 0.0))
    network.set_actor_label("RoadNetwork")
    say("placed the road network actor")

    course_class = unreal.load_class(None, COURSE_CLASS)
    if course_class is None:
        fail("%s not found - build AirportMgrEditor before running this, or the level would "
             "have no course to drive" % COURSE_CLASS)
        return False
    course = actors().spawn_actor_from_class(course_class, unreal.Vector(0.0, 0.0, 0.0))
    course.set_actor_label("RigTestCourse")
    say("placed the rig test course - it lays its own roads at BeginPlay, not here")
    return True


def build_game_mode():
    """The world-settings game mode override, which is what gives PIE the road-build camera
    and keys - see build_model_yard.py's build_bench() for the same override on a different
    game mode, and its comment on why an absent override reads as broken input rather than a
    missing line in World Settings."""
    game_mode = unreal.load_class(None, GAME_MODE)
    if game_mode is None:
        fail("%s not found - PIE would start with the project default game mode and no "
             "road-build keys bound" % GAME_MODE)
        return False
    settings = world_settings()
    if settings is None:
        fail("no AWorldSettings in the level, so the game mode override cannot be set")
        return False
    settings.set_editor_property("default_game_mode", game_mode)
    say("set the level's game mode override to BP_RoadBuildGameMode")
    return True


def verify():
    """RELOAD AND COUNT, because the save is the step that lies (memory:
    unreal-editing-levels-headlessly). Also the name-table control (memory:
    unreal-compile-all-blueprints-headless): grep a name we KNOW the .umap carries before
    trusting a grep for RigTestCourse to mean anything."""
    levels().load_level(LEVEL)
    found = actors().get_all_level_actors()
    say("reloaded %s: %d actor(s)" % (LEVEL, len(found)))

    network_class = unreal.load_class(None, NETWORK_CLASS)
    course_class = unreal.load_class(None, COURSE_CLASS)
    networks = [a for a in found if network_class is not None and a.get_class() == network_class]
    courses = [a for a in found if course_class is not None and a.get_class() == course_class]
    floors = [a for a in found if isinstance(a, unreal.StaticMeshActor)]

    if len(networks) != 1:
        fail("expected exactly 1 ARoadNetworkActor, found %d - will not guess" % len(networks))
        return False
    say("PASS exactly 1 ARoadNetworkActor survived the save")

    if len(courses) != 1:
        fail("expected exactly 1 ARigTestCourse, found %d - will not guess" % len(courses))
        return False
    say("PASS exactly 1 ARigTestCourse survived the save")

    if not floors:
        fail("no floor actor survived the save - the course would have no visible ground")
        return False
    say("PASS the floor survived the save")

    if not [a for a in found if isinstance(a, unreal.DirectionalLight)]:
        fail("no directional light survived the save - the level would be lit by ambient only")
        return False
    say("PASS the lighting survived the save")

    settings = world_settings()
    on_disk = settings.get_editor_property("default_game_mode") if settings else None
    wanted = unreal.load_class(None, GAME_MODE)
    if on_disk != wanted:
        fail("the level's game mode override came back as %s, not BP_RoadBuildGameMode - PIE "
             "would run with the wrong controller" % on_disk)
        return False
    say("PASS the game mode override survived the save")
    return True


def main():
    say("=" * 78)
    say("building the rig test level at %s" % LEVEL)
    if not start_level():
        say("DONE")
        return
    build_lighting()
    build_floor()
    build_start()
    if not build_network_and_course():
        say("DONE")
        return
    if not build_game_mode():
        say("DONE")
        return

    if not levels().save_current_level():
        fail("save_current_level() returned False - the .umap is locked, which means the "
             "editor is open. Close it and re-run; nothing was written.")
        say("DONE")
        return
    say("saved %s" % LEVEL)

    if verify():
        say("M_RigTest is ready; open it and PLAY to watch the rig loop - watch the log for "
            "'LogRoadBuild: RigCourse: loop' summaries")
    say("=" * 78)
    say("DONE")


main()
