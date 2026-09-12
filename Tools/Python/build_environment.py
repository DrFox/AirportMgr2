"""Sets M_Starter's lighting for the stylised art direction and adds the post-process
volume. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

THE EXPOSURE LOCK IS THE POINT, EVENTUALLY. Auto-exposure re-brightens the frame as the
player pans across a dark hangar, which reads as a rendering bug in a builder and makes
judging a colour impossible because nothing holds still. Scene.h:1988 - "Eye Adaptation is
disabled if Min = Max".

BUT THE VALUE IS MEASURED, NOT GUESSED, AND IT CANNOT BE MEASURED YET. See
LOCK_EXPOSURE_EV100 below. The units are EV100 (Config/DefaultEngine.ini sets
ExtendDefaultLuminanceRange, and Scene.cpp:499-506 switches Min/Max to EV100 when it is),
but knowing the units does not tell you the number - that depends on what this level's sun
and sky actually put on the ground, and there is no ground until Slice B.

APPLIES, THEN RELOADS AND VERIFIES. A headless level edit can report success and write
nothing (a locked .umap does exactly that), so returning without reading the values back
off disk would prove nothing.
"""
import unreal

LEVEL = "/Game/Maps/M_Starter"

# The locked exposure, in EV100, or None to leave auto-exposure alone.
#
# NONE UNTIL THE GROUND EXISTS, and that ordering is the point. Locking exposure means
# choosing an absolute brightness to expose FOR, and there is nothing to meter against
# until Slice B's Landscape is in: this level currently has no ground at all, because
# Slice A deletes the template floor.
#
# Locked at EV100 14 with no ground, the entire viewport rendered BLACK - and a black
# frame is indistinguishable from the sun, sky or atmosphere being broken, which is
# exactly the kind of false trail that costs a session. 14 came from the real-world
# photographic scale (sunny-16 is about EV 15); this level's sun is at the engine default
# Intensity of 10, which produces nothing like that luminance.
#
# Measuring it headlessly does not work either: a -run=pythonscript commandlet has no live
# rendering pipeline, so SceneCapture2D plus ExportRenderTarget writes nothing and logs
# "render target has been released". The value has to be read in the interactive editor,
# from the viewport's exposure readout, once there is grass under the camera.
LOCK_EXPOSURE_EV100 = None

# Spec section 4.2. Slice G turns this pair into the noon point of a curve.
SUN_PITCH = -42.0
SUN_YAW = 150.0
SUN_TEMPERATURE = 5800.0

# Engine default is 0.5357 - the real sun's angular diameter, hence razor-sharp shadow
# edges. The concept renders have a soft penumbra, and widening the source is nearly free.
SUN_SOURCE_ANGLE = 1.5

# SkyAtmosphere's aerial perspective already gives distance haze; the stock fog is grey
# and flattens everything behind it.
FOG_DENSITY = 0.005
FOG_HEIGHT_FALLOFF = 0.2


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def find_one(cls):
    """The single actor of this class, or None. Two is a level authoring error, not a
    thing to pick between silently."""
    found = [a for a in actors().get_all_level_actors() if isinstance(a, cls)]
    if len(found) != 1:
        fail("expected exactly 1 %s, found %d" % (cls.__name__, len(found)))
        return None
    return found[0]


def set_sun(light):
    comp = light.get_component_by_class(unreal.DirectionalLightComponent)
    light.set_actor_rotation(unreal.Rotator(0.0, SUN_PITCH, SUN_YAW), False)
    comp.set_editor_property("atmosphere_sun_light", True)
    comp.set_editor_property("light_source_angle", SUN_SOURCE_ANGLE)
    comp.set_editor_property("use_temperature", True)
    comp.set_editor_property("temperature", SUN_TEMPERATURE)
    say("sun set: pitch %.1f yaw %.1f source angle %.2f" % (SUN_PITCH, SUN_YAW, SUN_SOURCE_ANGLE))


def set_sky_light(sky):
    """Real Time Capture ASSERTED, not assumed. SkyLightComponent.cpp:331 defaults
    bRealTimeCapture to false; with a moving sun (Slice G) a static capture bakes the
    wrong ambient for 23 hours of every 24."""
    comp = sky.get_component_by_class(unreal.SkyLightComponent)
    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    comp.set_editor_property("real_time_capture", True)
    say("sky light: real time capture on, movable")


def set_fog(fog):
    comp = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    comp.set_editor_property("fog_density", FOG_DENSITY)
    comp.set_editor_property("fog_height_falloff", FOG_HEIGHT_FALLOFF)
    say("fog dialled down to density %.4f" % FOG_DENSITY)


def add_post_process():
    existing = [a for a in actors().get_all_level_actors()
                if isinstance(a, unreal.PostProcessVolume)]
    for old in existing:
        actors().destroy_actor(old)
        say("removed an existing post process volume")

    ppv = actors().spawn_actor_from_class(
        unreal.PostProcessVolume, unreal.Vector(0.0, 0.0, 0.0))
    ppv.set_actor_label("PP_Environment")
    ppv.set_editor_property("unbound", True)

    # get_editor_property on a struct returns a COPY. Mutating it does nothing until it is
    # set back - the single most common way a Python post-process edit silently no-ops.
    s = ppv.get_editor_property("settings")

    # EXPOSURE IS DELIBERATELY LEFT ON AUTO HERE. See LOCK_EXPOSURE_EV100 above for why:
    # the value cannot be chosen before the ground exists, and a wrong lock renders the
    # whole frame black, which is indistinguishable from the lighting itself being broken.
    if LOCK_EXPOSURE_EV100 is not None:
        s.set_editor_property("override_auto_exposure_min_brightness", True)
        s.set_editor_property("auto_exposure_min_brightness", LOCK_EXPOSURE_EV100)
        s.set_editor_property("override_auto_exposure_max_brightness", True)
        s.set_editor_property("auto_exposure_max_brightness", LOCK_EXPOSURE_EV100)

    s.set_editor_property("override_bloom_intensity", True)
    s.set_editor_property("bloom_intensity", 0.4)

    # Stylised means clean. A panning top-down camera plus motion blur is nausea.
    s.set_editor_property("override_motion_blur_amount", True)
    s.set_editor_property("motion_blur_amount", 0.0)
    s.set_editor_property("override_vignette_intensity", True)
    s.set_editor_property("vignette_intensity", 0.0)
    s.set_editor_property("override_film_grain_intensity", True)
    s.set_editor_property("film_grain_intensity", 0.0)

    # The blue shadow tint fakes sky bounce, and is what stops the olive greens reading
    # muddy. Saturation is a LIFT, not a push - section 1 wants the ground restrained.
    s.set_editor_property("override_color_saturation", True)
    s.set_editor_property("color_saturation", unreal.Vector4(1.05, 1.05, 1.05, 1.0))
    s.set_editor_property("override_color_saturation_shadows", True)
    s.set_editor_property("color_saturation_shadows", unreal.Vector4(1.0, 1.0, 1.08, 1.0))

    ppv.set_editor_property("settings", s)
    if LOCK_EXPOSURE_EV100 is None:
        say("post process volume added, exposure left on AUTO (see LOCK_EXPOSURE_EV100)")
    else:
        say("post process volume added, exposure locked at EV100 %.1f" % LOCK_EXPOSURE_EV100)
    return ppv


def delete_floor():
    """The template floor is replaced by the Landscape in Slice B.

    SM_SkySphere is deliberately NOT deleted here. It may or may not be occluding the
    SkyAtmosphere, and that is a question for a screenshot, not for this script.
    """
    for a in actors().get_all_level_actors():
        if a.get_actor_label() == "Floor":
            actors().destroy_actor(a)
            say("deleted template floor")
            return
    say("no actor labelled 'Floor' - already gone")


def verify():
    """Reload from disk and read every value back. This is the test."""
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL)
    ok = True

    ppv = find_one(unreal.PostProcessVolume)
    if ppv is None:
        return False
    s = ppv.get_editor_property("settings")
    checks = [("motion_blur_amount", 0.0),
              ("vignette_intensity", 0.0),
              ("bloom_intensity", 0.4)]
    if LOCK_EXPOSURE_EV100 is not None:
        checks += [("auto_exposure_min_brightness", LOCK_EXPOSURE_EV100),
                   ("auto_exposure_max_brightness", LOCK_EXPOSURE_EV100)]
    for prop, expected in checks:
        got = s.get_editor_property(prop)
        if abs(got - expected) > 1e-4:
            fail("%s is %r, expected %r" % (prop, got, expected))
            ok = False
        else:
            say("PASS %s = %r" % (prop, got))

    if not ppv.get_editor_property("unbound"):
        fail("post process volume is not unbound")
        ok = False
    else:
        say("PASS post process volume is unbound")

    sun = find_one(unreal.DirectionalLight)
    if sun is not None:
        comp = sun.get_component_by_class(unreal.DirectionalLightComponent)
        angle = comp.get_editor_property("light_source_angle")
        if abs(angle - SUN_SOURCE_ANGLE) > 1e-4:
            fail("light_source_angle is %r, expected %r" % (angle, SUN_SOURCE_ANGLE))
            ok = False
        else:
            say("PASS light_source_angle = %r" % angle)

        pitch = sun.get_actor_rotation().pitch
        if abs(pitch - SUN_PITCH) > 1e-3:
            fail("sun pitch is %r, expected %r" % (pitch, SUN_PITCH))
            ok = False
        else:
            say("PASS sun pitch = %r" % pitch)

    sky = find_one(unreal.SkyLight)
    if sky is not None:
        comp = sky.get_component_by_class(unreal.SkyLightComponent)
        if not comp.get_editor_property("real_time_capture"):
            fail("sky light real_time_capture is off")
            ok = False
        else:
            say("PASS sky light real_time_capture on")

    floors = [a for a in actors().get_all_level_actors() if a.get_actor_label() == "Floor"]
    if floors:
        fail("template floor is still in the level")
        ok = False
    else:
        say("PASS template floor is gone")

    return ok


def run():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level(LEVEL)
    say("loaded %s" % LEVEL)

    sun = find_one(unreal.DirectionalLight)
    sky = find_one(unreal.SkyLight)
    fog = find_one(unreal.ExponentialHeightFog)
    if sun is None or sky is None or fog is None:
        fail("level is not the expected template layout; nothing written")
        return

    set_sun(sun)
    set_sky_light(sky)
    set_fog(fog)
    add_post_process()
    delete_floor()

    levels.save_current_level()
    say("saved level")

    say("ALL VERIFIED" if verify() else "VERIFY FAILED")
    say("DONE")


run()
