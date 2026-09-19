"""Authors /Game/Maps/M_ModelYard - every imported model, laid out to scale, lit like the
game. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED. With it open, save_current_level() returns False and the save
fails with a sharing violation; EditorLoadingAndSavingUtils.save_map is worse, returning
without complaint and writing nothing. This script RELOADS AND COUNTS what it wrote for
exactly that reason - a headless level edit that reports success and writes nothing is the
known failure mode here.

WHY A SEPARATE MAP AND NOT M_STARTER. M_Starter is the level the game loads, with the road
network actor, the sun driver and the stand entities in it; parking seven display models in
it makes them part of the airport, which is not what they are yet. A yard is somewhere to
LOOK at them. Nothing else references this map, so it can be deleted the day each model has
a real home.

LAYOUT IS MEASURED, NOT TYPED. Each model's spacing comes from its own imported bounds, so
adding a model to a row needs no arithmetic and a re-export that changes a size cannot leave
two models overlapping. The rows are aircraft and ground equipment because mixing a 28 m
Dash 8 into a line of 3 m utility vehicles wastes most of the frame on gaps.

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
"""
import unreal

LEVEL = "/Game/Maps/M_ModelYard"

# THE ROWS. Sorted small to large along +Y so the size progression reads as a progression;
# each entry is (asset path, label).
#
# THE THREE ALREADY-IMPORTED MODELS ARE HERE TOO. They cost nothing to place and they are
# the scale reference that makes the new four legible - a Dash 8 next to a Piper says more
# about both than either says alone.
ROWS = [
    ("Aircraft", 1200.0, [
        # SMALLEST FIRST, so the row reads as the progression the header claims. plane1 goes
        # ahead of the Meridian because it is 2.1 m shorter and 2.1 m narrower - the first
        # entry that makes "small to large" true of the whole row rather than of most of it.
        ("/Game/Aircraft/Plane1/SK_Plane1", "Plane1 (Cessna 172)"),
        ("/Game/Aircraft/PiperMeridian/SK_PiperMeridian", "Piper Meridian"),
        ("/Game/Aircraft/Plane2/SK_Plane2", "Plane2 (Twin Otter)"),
        ("/Game/Aircraft/Plane3/SK_Plane3", "Plane3 (Dash 8-Q400)"),
        ("/Game/Aircraft/Plane4/SK_Plane4", "Plane4 (737-800W)"),
    ]),
    ("Ground equipment", 500.0, [
        ("/Game/Vehicles/GPU1/SK_GPU1", "GPU1 (towed)"),
        ("/Game/Vehicles/Utility1/SK_Utility1", "Utility1"),
        ("/Game/Vehicles/Tug1/SK_Tug1", "Tug1 (Goldhofer D 620)"),
        ("/Game/Vehicles/FuelTruck1/SK_FuelTruck1", "FuelTruck1"),
    ]),
]

# How far apart the two rows sit on X. Generous, because an aircraft's row position is its
# ORIGIN and the Dash 8's origin is on its main gear with 16 m of fuselage behind it.
ROW_PITCH_UU = 5000.0

# The floor. /Engine/BasicShapes/Plane is 100 x 100 uu at scale 1, so these are metres x 1.
FLOOR_X_M = 220.0
FLOOR_Y_M = 220.0

# A NEUTRAL GREY FLOOR, not M_ApronConcrete, and the choice is deliberate. The apron material
# is built for a road mesh that carries its own UV1 (see the road appearance notes - colour
# there comes from UV1, not from the material's own parameters), and a single scaled
# BasicShapes/Plane has UV 0-1 across 220 m, so a tiling surface applied to it stretches one
# texel over the whole yard. A studio-grey floor is also simply the better backdrop for
# judging a model's own colours.
FLOOR_MATERIAL = "/Engine/BasicShapes/BasicShapeMaterial"
FLOOR_MESH = "/Engine/BasicShapes/Plane"

# LIGHTING COPIED FROM build_environment.py, not invented, so the models are judged under the
# light the game actually puts on them. If those values change there, they should change here
# - the two are not wired together, because M_Starter's are applied to a level that already
# exists and these author a level from nothing.
SUN_PITCH = -42.0
SUN_YAW = 150.0
SUN_TEMPERATURE = 5800.0
SUN_SOURCE_ANGLE = 1.5
FOG_DENSITY = 0.005
FOG_HEIGHT_FALLOFF = 0.2

# EXPOSURE IS LEFT ON AUTO, and this deviates from M_Starter deliberately. build_environment.py
# records that locking it at a wrong EV100 rendered the entire viewport BLACK, and that a
# black frame is indistinguishable from the sun, sky or atmosphere being broken. Its 1.75 was
# MEASURED against a landscape of grass; this yard's floor is grey plastic with a different
# albedo, there is no way to meter it headlessly (a commandlet has no rendering pipeline, so
# SceneCapture2D writes nothing), and the entire purpose of this map is that you can see the
# models. Auto-exposure cannot produce a black frame. Lock it once someone has read the
# viewport's exposure readout here.
LOCK_EXPOSURE_EV100 = None

# Labels lie FLAT ON THE GROUND rather than standing up, because this is a Cities-Skylines
# camera looking down: a billboard facing one horizontal direction is edge-on from most of
# the angles the game is ever viewed from. Flat text reads from every one of them.
#
# THE ROTATION IS MEASURED, and the measurement corrected the sign this line shipped with.
# UTextRenderComponent lays its glyphs in the component's LOCAL X = 0 PLANE:
# TextRenderComponent.cpp:1373 builds the bounds as
# FBox(FVector(0, -Size.X - LeftTop.X, -Size.Y), FVector(0, -LeftTop.X, 0)), so the string
# advances along local -Y with the text's own up on local +Z, and the READABLE FACE IS
# LOCAL +X. Pitch +90 is therefore what lays that face along world +Z, where a camera
# looking down can see its front. Yaw 180 then puts the reading direction on world +Y and
# the text's up on world +X - screen-right and screen-up for a top-down view with +X up the
# screen.
#
# PITCH -90 WAS THE BUG and it shipped under a comment predicting the opposite, calling
# itself a prediction and inviting the wrong remedy. It sent the readable face to world -Z,
# so every label in the yard was read through its own back and came out MIRRORED - which is
# what "backwards" looked like in the viewport on 2026-09-19.
#
# FLIPPING THE YAW, which that comment advised, WOULD NOT HAVE FIXED IT. A yaw is a rotation
# about Z, and no amount of it turns a normal already lying along -Z back to +Z: it slides
# the text around the ground and leaves it mirrored. The axis that is wrong is not always
# the axis whose symptom you can see.
LABEL_ROTATION = unreal.Rotator(0.0, 90.0, 180.0)
LABEL_SIZE_UU = 150.0
LABEL_GAP_UU = 400.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def levels():
    return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def start_level():
    """A clean level to author into, whether or not one is already there.

    RE-AUTHORED FROM EMPTY rather than patched, because this script owns every actor in the
    map: there is no hand-placed work here to preserve, and clearing means a model removed
    from ROWS actually leaves rather than lingering from a previous run. That is the opposite
    of the rule for DATA ASSETS, which must be re-authored IN PLACE because other assets hold
    references to them - nothing references this map.
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
    """Sun, sky, atmosphere, fog and post - in that order, because the atmosphere needs a
    light that has declared itself its sun."""
    sun = actors().spawn_actor_from_class(
        unreal.DirectionalLight, unreal.Vector(0.0, 0.0, 2000.0))
    sun.set_actor_label("Sun")
    sun.set_actor_rotation(unreal.Rotator(0.0, SUN_PITCH, SUN_YAW), False)
    comp = sun.get_component_by_class(unreal.DirectionalLightComponent)
    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    # WITHOUT THIS THE SKY IS BLACK. SkyAtmosphere takes its sun from the directional light
    # that claims the role; no claimant means no scattering, and the result reads as the
    # atmosphere actor not working.
    comp.set_editor_property("atmosphere_sun_light", True)
    comp.set_editor_property("use_temperature", True)
    comp.set_editor_property("temperature", SUN_TEMPERATURE)
    comp.set_editor_property("light_source_angle", SUN_SOURCE_ANGLE)

    sky = actors().spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0.0, 0.0, 2000.0))
    sky.set_actor_label("SkyLight")
    sky_comp = sky.get_component_by_class(unreal.SkyLightComponent)
    # MOVABLE AND REAL-TIME, asserted rather than assumed: SkyLightComponent defaults
    # bRealTimeCapture to false, and a static capture in a level that was never lit bakes
    # black ambient - which looks exactly like the models having no materials.
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

    ppv = actors().spawn_actor_from_class(
        unreal.PostProcessVolume, unreal.Vector(0.0, 0.0, 0.0))
    ppv.set_actor_label("PP_ModelYard")
    ppv.set_editor_property("unbound", True)
    # get_editor_property on a struct returns a COPY. Mutating it does nothing until it is
    # set back - the single most common way a Python post-process edit silently no-ops.
    settings = ppv.get_editor_property("settings")
    if LOCK_EXPOSURE_EV100 is not None:
        settings.set_editor_property("override_auto_exposure_min_brightness", True)
        settings.set_editor_property("auto_exposure_min_brightness", LOCK_EXPOSURE_EV100)
        settings.set_editor_property("override_auto_exposure_max_brightness", True)
        settings.set_editor_property("auto_exposure_max_brightness", LOCK_EXPOSURE_EV100)
    settings.set_editor_property("override_motion_blur_amount", True)
    settings.set_editor_property("motion_blur_amount", 0.0)
    settings.set_editor_property("override_vignette_intensity", True)
    settings.set_editor_property("vignette_intensity", 0.0)
    settings.set_editor_property("override_film_grain_intensity", True)
    settings.set_editor_property("film_grain_intensity", 0.0)
    ppv.set_editor_property("settings", settings)

    say("lighting: sun pitch %.0f yaw %.0f, sky light real-time, atmosphere, fog, post "
        "(exposure %s)" % (SUN_PITCH, SUN_YAW,
                           "locked at EV100 %.2f" % LOCK_EXPOSURE_EV100
                           if LOCK_EXPOSURE_EV100 is not None else "on auto"))


def build_floor():
    mesh = unreal.EditorAssetLibrary.load_asset(FLOOR_MESH)
    if mesh is None:
        fail("no floor mesh at %s - the yard would have no ground and every model would "
             "float over the void" % FLOOR_MESH)
        return
    floor = actors().spawn_actor_from_class(
        unreal.StaticMeshActor, unreal.Vector(0.0, 0.0, 0.0))
    floor.set_actor_label("Floor")
    comp = floor.static_mesh_component
    comp.set_editor_property("static_mesh", mesh)
    material = unreal.EditorAssetLibrary.load_asset(FLOOR_MATERIAL)
    if material is not None:
        comp.set_material(0, material)
    else:
        say("NOTE %s would not load; the floor keeps the engine default material"
            % FLOOR_MATERIAL)
    floor.set_actor_scale3d(unreal.Vector(FLOOR_X_M, FLOOR_Y_M, 1.0))
    say("floor: %.0f x %.0f m" % (FLOOR_X_M, FLOOR_Y_M))


def bounds_of(mesh):
    """(size, min) in uu. USkeletalMesh has no get_bounding_box; it gives FBoxSphereBounds."""
    bounds = mesh.get_bounds()
    origin = bounds.origin
    extent = bounds.box_extent
    size = unreal.Vector(extent.x * 2.0, extent.y * 2.0, extent.z * 2.0)
    low = unreal.Vector(origin.x - extent.x, origin.y - extent.y, origin.z - extent.z)
    return size, low


def place_row(name, row_x, gap_uu, entries):
    """One row, spaced by each model's own measured width.

    THE ORIGIN IS NOT THE CENTRE for any of these models - they are authored with the origin
    on an axle or a gear leg, which is the whole point of the convention - so the placement
    works in the model's own bounds and then offsets by where its origin sits inside them.
    Spacing on the origin instead would overlap the Dash 8 with whatever is beside it.
    """
    placed = []
    loaded = []
    total = 0.0
    for path, label in entries:
        mesh = unreal.EditorAssetLibrary.load_asset(path)
        if mesh is None:
            fail("%s is not on disk - import it before building the yard" % path)
            continue
        if not isinstance(mesh, unreal.SkeletalMesh):
            fail("%s is a %s, not a SkeletalMesh" % (path, type(mesh).__name__))
            continue
        size, low = bounds_of(mesh)
        loaded.append((mesh, label, size, low))
        total += size.y + gap_uu
    if not loaded:
        return placed
    total -= gap_uu

    # Centred on Y = 0, so the row grows both ways as models are added rather than wandering
    # off one side of the floor.
    cursor = -total * 0.5
    for mesh, label, size, low in loaded:
        # cursor is the row's running edge; the model's LEFT edge goes there, and its origin
        # sits (origin - min) into its own width.
        origin_offset_y = -low.y
        y = cursor + origin_offset_y
        actor = actors().spawn_actor_from_class(
            unreal.SkeletalMeshActor, unreal.Vector(row_x, y, 0.0))
        actor.set_actor_label(label)
        actor.skeletal_mesh_component.set_skeletal_mesh_asset(mesh)
        # STATIC would be wrong even for a display model: these have skeletons, and a static
        # skeletal mesh component does not update its bounds from the pose.
        actor.skeletal_mesh_component.set_editor_property(
            "mobility", unreal.ComponentMobility.MOVABLE)

        # The label goes at the model's own -X edge, clear of its nose, on the ground.
        text = actors().spawn_actor_from_class(
            unreal.TextRenderActor,
            unreal.Vector(row_x + low.x - LABEL_GAP_UU, y, 2.0))
        text.set_actor_label("Label_%s" % label)
        text.set_actor_rotation(LABEL_ROTATION, False)
        text_comp = text.text_render
        text_comp.set_text(unreal.Text("%s   %.1f x %.1f m"
                                       % (label, size.x / 100.0, size.y / 100.0)))
        text_comp.set_world_size(LABEL_SIZE_UU)
        text_comp.set_horizontal_alignment(unreal.HorizTextAligment.EHTA_CENTER)
        text_comp.set_text_render_color(unreal.Color(20, 20, 20, 255))

        say("  %-24s at X=%.0f Y=%.0f, %.1f x %.1f x %.1f m"
            % (label, row_x, y, size.x / 100.0, size.y / 100.0, size.z / 100.0))
        placed.append(label)
        cursor += size.y + gap_uu

    say("%s: %d model(s) over %.0f m" % (name, len(placed), total / 100.0))
    return placed


def build_rows():
    placed = []
    # Rows straddle X = 0 so the yard is centred on the origin whatever is in it.
    first_x = ROW_PITCH_UU * (len(ROWS) - 1) * 0.5
    for index, (name, gap, entries) in enumerate(ROWS):
        placed += place_row(name, first_x - index * ROW_PITCH_UU, gap, entries)
    return placed


def build_start():
    """A PlayerStart, so PIE puts the camera somewhere useful rather than at the origin
    inside the Dash 8."""
    start = actors().spawn_actor_from_class(
        unreal.PlayerStart, unreal.Vector(-9000.0, 0.0, 400.0))
    start.set_actor_label("PlayerStart")
    start.set_actor_rotation(unreal.Rotator(0.0, -10.0, 0.0), False)


def verify(expected_models):
    """RELOAD AND COUNT, because the save is the step that lies.

    save_current_level() on a locked .umap returns False and nothing downstream checks it;
    save_map returns cleanly having written nothing at all. The only honest evidence that
    this ran is reading the level back off disk in a state it was not in before.
    """
    levels().load_level(LEVEL)
    found = actors().get_all_level_actors()
    meshes = [a for a in found if isinstance(a, unreal.SkeletalMeshActor)]
    labels = sorted(str(a.get_actor_label()) for a in meshes)
    say("reloaded %s: %d actor(s), %d of them skeletal mesh actors"
        % (LEVEL, len(found), len(meshes)))
    say("models on disk: %s" % ", ".join(labels))

    missing = [want for want in sorted(expected_models) if want not in labels]
    if missing:
        fail("%d model(s) placed but not on disk after reload: %s. The level save wrote "
             "nothing - the editor is almost certainly still open."
             % (len(missing), ", ".join(missing)))
        return False

    # THE MESHES ARE ASSERTED TOO, not just the actors. An actor whose SkeletalMesh failed to
    # serialise loads as a correctly named, correctly placed, completely invisible thing.
    #
    # THROUGH THE ACCESSOR, NOT get_editor_property("skeletal_mesh_asset"). That UPROPERTY is
    # declared Transient and WITH_EDITORONLY_DATA on USkeletalMeshComponent (line 360 of its
    # header), carrying a deprecation note that reads "this property isn't deprecated, but
    # getter and setter must be used at all times to preserve correct operations" - the
    # serialised storage is USkinnedMeshComponent::SkinnedAsset. Reading the transient field
    # after a reload would have reported every correctly saved model as invisible, which is
    # the most expensive kind of wrong check: one that fails on a passing run.
    empty = [str(a.get_actor_label()) for a in meshes
             if a.skeletal_mesh_component.get_skeletal_mesh_asset() is None]
    if empty:
        fail("%d actor(s) came back with no mesh and would be invisible: %s"
             % (len(empty), ", ".join(empty)))
        return False
    say("PASS every placed model came back off disk carrying its mesh")

    if not [a for a in found if isinstance(a, unreal.DirectionalLight)]:
        fail("no directional light survived the save - the yard would be lit by ambient "
             "only, which reads as the materials being wrong")
        return False
    say("PASS the lighting survived the save")
    return True


def main():
    say("=" * 78)
    say("building the model yard at %s" % LEVEL)
    if not start_level():
        say("DONE")
        return
    build_lighting()
    build_floor()
    placed = build_rows()
    build_start()

    if not levels().save_current_level():
        fail("save_current_level() returned False - the .umap is locked, which means the "
             "editor is open. Close it and re-run; nothing was written.")
        say("DONE")
        return
    say("saved %s" % LEVEL)

    if verify(placed):
        say("the yard holds %d model(s); open %s and look" % (len(placed), LEVEL))
    say("=" * 78)
    say("DONE")


main()
