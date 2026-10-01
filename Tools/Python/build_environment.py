"""Sets M_Test's lighting for the stylised art direction and adds the post-process
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

LEVEL = "/Game/Maps/M_Test"
# M_Test_Small and M_Test_2Runways are copies of M_Test's environment. They are NOT run
# through this script (its floor deletion and PPV rebuild would do more than a copy needs);
# the 2026-09-30 white balance and fog were set on each by hand over MCP to the values
# below. A change here must be carried to them the same way, or the maps drift.

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
#
# 2026-10-01: 1.75 -> 1.0 for the brighter Portscape-style look. NOTE it barely moves the
# frame (mean ~10% brighter); the white balance below did most of the brightening, which is
# not yet explained - measure before trusting either knob to set brightness alone.
#
# 2026-10-01 (later): 1.0 -> 1.75 BACK, owner: "washed out, difficult to tell the taxiway from the
# grass" (samples/colours.png). Measured in PIE on M_Test_Small, one view, one knob at a time:
# this one DID move the frame - grass #C4D894 -> #A8C06C - contradicting "barely moves" above;
# with the white balance below it took nearly all of the wash. Kept the rest of the Portscape pass.
LOCK_EXPOSURE_EV100 = 1.75

# Spec section 4.2. Slice G turns this pair into the noon point of a curve.
SUN_PITCH = -42.0
SUN_YAW = 150.0
SUN_TEMPERATURE = 5800.0

# Engine default is 0.5357 - the real sun's angular diameter, hence razor-sharp shadow
# edges. The concept renders have a soft penumbra, and widening the source is nearly free.
SUN_SOURCE_ANGLE = 1.5

# Cloud shadows. OFF in the engine by default (bCastCloudShadows = 0), which is why the
# volumetric clouds overhead were not landing anything on the grass.
#
# THE EXTENT IS THE WHOLE TRICK, AND THE DEFAULT IS WRONG FOR THIS PLOT. It is a RADIUS IN
# KILOMETRES around the camera, defaulting to 150 - 300 km across, sampled into a shadow map
# whose base resolution is 512. That is about 586 m per texel, so on a 3 km airfield the
# entire plot spans five texels and cloud shadows arrive as a vague brightness wobble rather
# than as shapes moving over the ground.
#
# SIZING IT TO THE PLOT IS THE MISTAKE, and it was made here first: 6 km radius covers the
# 3,024 m field comfortably and produced NO cloud shadows at all. The extent has to contain
# the CLOUDS THAT CAST ONTO the field, not the field. Cloud layers sit 5-10 km up, and with
# the sun at 42 degrees a cloud's shadow lands altitude/tan(42) - between 6 and 11 km -
# away horizontally, so a 6 km map does not hold them.
#
# r.VolumetricCloud.ShadowMap.SnapLength confirms it independently: the map's position
# snaps to a 20 km grid by default, which is larger than a 6 km map, so the map could sit
# entirely off the plot.
#
# 25 km radius holds the casting clouds at any sun angle the day cycle reaches. At a
# resolution scale of 4 (512 * 4 = 2048, exactly the ShadowMap.MaxResolution clamp) that is
# 50,000 / 2048 = 24 m per texel - about 125 texels across the plot, and ample, because a
# real cloud shadow is hundreds of metres across with soft edges.
CLOUD_SHADOW_EXTENT_KM = 25.0
CLOUD_SHADOW_RESOLUTION_SCALE = 4.0

# Below 1.0 because section 1 wants a calm ground. Full-strength cloud shadow is a hard
# dark patch that competes with the aircraft and vehicles for attention.
# 2026-10-01: 0.8 -> 0.35. With brighter exposure the patches read as dirt on the grass;
# the reference has soft, lifted shade.
CLOUD_SHADOW_STRENGTH = 0.35

# How much sky has cloud in it, and how solid that cloud is.
#
# THE ENGINE INSTANCE SHIPS COVERAGE AT -0.2, WHICH IS NEGATIVE - m_SimpleVolumetricCloud_Inst
# is tuned for sparse decorative cloud on a backdrop, not for a game where the cloud's job is
# to put moving shadows on the ground. Cloud shadows DO work at that setting; they are simply
# rare enough to miss, which is exactly what happened when this was first measured and wrongly
# written off as not working.
#
# Density 0.008 -> 0.014 for the same reason: a thin layer occludes little, so the shadow it
# casts is faint even when it is overhead.
CLOUD_COVERAGE = 0.0
CLOUD_DENSITY = 0.012

# Ours, in /Game, because the one the level starts with belongs to the ENGINE and must not be
# edited - every project on this engine install shares it.
CLOUD_MATERIAL_PATH = "/Game/Environment/MI_Clouds"

# THE FOG'S JOB IS THE HORIZON, NOT THE AIRPORT. At 0.005 (the first value, chosen because
# the stock grey fog flattened the near scene) nothing hazed at all: from a low camera the
# 3 km landscape ended in a hard edge over a purple band of planet-below-the-horizon.
# Tuned live 2026-09-30 against a -18 degree shot from 60 m and the -70 degree build view:
#
#   density 0.03, falloff 0.2           - no visible haze; the edge still hard
#   density 0.10, falloff 0.1           - edge gone, but the SKY fogged to slate, clouds lost
#   density 0.10, falloff 1.0           - fog confined so low it vanished over the ground
#   density 0.15, falloff 0.35 (chosen) - edge melts into pale blue haze, clouds kept
#
# StartDistance 600 m keeps everything within the build camera's reach crisp: haze reads as
# distance, and at the airport it would only read as murk. The inscattering colour is ADDED
# to the sky-atmosphere-lit fog (the engine default is black, ExponentialHeightFogComponent
# .cpp:76) - without it the band below the horizon stayed purple rather than pale blue.
FOG_DENSITY = 0.15
FOG_HEIGHT_FALLOFF = 0.35
FOG_START_DISTANCE = 60000.0
# 2026-10-01: (0.15, 0.2, 0.3) -> (0.2, 0.28, 0.42) for a paler horizon. (0.4, 0.5, 0.65)
# and (0.28, 0.36, 0.5) were tried: both turned the far field milky.
FOG_INSCATTERING = (0.2, 0.28, 0.42)

# White balance, in kelvin. 6500 is neutral; lower cools the image.
#
# THE WARM CAST WAS THE GRADE, NOT THE MATERIALS. Under a 5800 K sun and no white balance,
# neutral concrete rendered tan and every apron read as dirt (2026-09-27, apron #6D6457,
# red minus blue +22). Swept live 2026-09-30 on the build view:
#
#   6000 - apron r-b +23, asphalt -1    (still tan)
#   5600 - apron r-b +11, asphalt -9    (matches the palette: concrete #9C9B91 is +11,
#                                        taxiway asphalt #454D50 is -11)
#   5200 - apron r-b  -3, asphalt -19   (concrete goes lilac, grass loses its olive)
#
# 5600 rather than the sun's own 5800 because the blue sky light pulls the other way less
# than the sun pushes. The sun still warms toward dusk against this fixed balance, which is
# the point: dusk should look warmer than noon.
#
# 2026-10-01: 5600 -> 6000, Portscape reference (innercorestudios.com/games/portscape):
# warm and bright rather than neutral. Judged by eye at the build camera with the apron
# re-picked to warm beige at the same time, so the concrete-not-dirt argument above was
# deliberately traded for warmth. M_Test had been saved at 5200, NOT 5600 - the maps had
# already drifted from this constant.
#
# 2026-10-01 (later): 6000 -> 5600 BACK with the exposure above, for the same report; owner
# judged the pair "a lot better than the old" in PIE on slot2's maps.
WHITE_TEMP_KELVIN = 5600.0

# Sky light, the shadow fill. Engine default 1.0 left the shade side of everything dark
# against Portscape's lifted shadows; 1.6 set live 2026-10-01.
# 1.6 -> 1.0 BACK the same evening with the exposure: it lifted the shade toward the grass's
# own green and flattened the frame - about 3% of the brightness alone, measured in PIE.
SKY_LIGHT_INTENSITY = 1.0

# Sky atmosphere. Less Mie (engine 0.003996) clears the milky haze from the sky dome, and a
# blue-leaning luminance factor deepens it toward the clear pale blue of the reference.
# Set live 2026-10-01.
SKY_MIE_SCATTERING_SCALE = 0.0015
SKY_LUMINANCE_FACTOR = (0.9, 1.0, 1.15)

# THE CLOUDS CAST SHADOWS BUT ARE NOT DRAWN (2026-10-01). Seen from the low camera the layer
# filled the sky with grey overcast, the opposite of the reference's clear sky. Cutting its
# density to 0 in MI_Clouds changed NOTHING on screen (the parameter is not what the visible
# layer reads; unexplained), so the component's bRenderInMainPass is turned off instead:
# the layer still exists for the cloud shadows below, and only its image is gone.
CLOUDS_RENDER_IN_MAIN_PASS = False


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

    comp.set_editor_property("cast_cloud_shadows", True)
    comp.set_editor_property("cloud_shadow_extent", CLOUD_SHADOW_EXTENT_KM)
    comp.set_editor_property("cloud_shadow_map_resolution_scale", CLOUD_SHADOW_RESOLUTION_SCALE)
    comp.set_editor_property("cloud_shadow_strength", CLOUD_SHADOW_STRENGTH)
    comp.set_editor_property("cloud_shadow_on_surface_strength", CLOUD_SHADOW_STRENGTH)
    say("sun set: pitch %.1f yaw %.1f source angle %.2f, cloud shadows on at %.0f km / x%.0f"
        % (SUN_PITCH, SUN_YAW, SUN_SOURCE_ANGLE, CLOUD_SHADOW_EXTENT_KM,
           CLOUD_SHADOW_RESOLUTION_SCALE))


def set_sky_light(sky):
    """Real Time Capture ASSERTED, not assumed. SkyLightComponent.cpp:331 defaults
    bRealTimeCapture to false; with a moving sun (Slice G) a static capture bakes the
    wrong ambient for 23 hours of every 24."""
    comp = sky.get_component_by_class(unreal.SkyLightComponent)
    comp.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    comp.set_editor_property("real_time_capture", True)
    comp.set_editor_property("intensity", SKY_LIGHT_INTENSITY)
    say("sky light: real time capture on, movable, intensity %.2f" % SKY_LIGHT_INTENSITY)


def set_sky_atmosphere(atmosphere):
    comp = atmosphere.get_component_by_class(unreal.SkyAtmosphereComponent)
    comp.set_editor_property("mie_scattering_scale", SKY_MIE_SCATTERING_SCALE)
    comp.set_editor_property("sky_luminance_factor", unreal.LinearColor(*SKY_LUMINANCE_FACTOR, 1.0))
    say("sky atmosphere: mie %.4f, luminance factor %r" % (SKY_MIE_SCATTERING_SCALE, SKY_LUMINANCE_FACTOR))


def set_fog(fog):
    comp = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    comp.set_editor_property("fog_density", FOG_DENSITY)
    comp.set_editor_property("fog_height_falloff", FOG_HEIGHT_FALLOFF)
    comp.set_editor_property("start_distance", FOG_START_DISTANCE)
    comp.set_editor_property("fog_inscattering_luminance", unreal.LinearColor(*FOG_INSCATTERING, 1.0))
    say("fog: density %.3f, falloff %.2f, from %.0f m" % (FOG_DENSITY, FOG_HEIGHT_FALLOFF,
                                                          FOG_START_DISTANCE / 100.0))


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

    s.set_editor_property("override_white_temp", True)
    s.set_editor_property("white_temp", WHITE_TEMP_KELVIN)

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


def set_clouds(cloud):
    """Point the cloud layer at our own material instance, with more cloud in it.

    Reached through the component rather than by path: the engine's sky content is not in
    the asset registry, so load_asset on /Engine/EngineSky/... returns None. The level's own
    actor holds a live reference, which is the only way to get at the parent material.
    """
    comp = cloud.get_component_by_class(unreal.VolumetricCloudComponent)
    current = comp.get_editor_property("material")
    if current is None:
        fail("volumetric cloud has no material")
        return


    # DUPLICATED, NOT CREATED FRESH, and that distinction cost a cloudless sky. A new
    # MaterialInstanceConstant parented to m_SimpleVolumetricCloud overrides only what this
    # script sets and inherits the PARENT's defaults for everything else - and the engine's
    # instance carries a good deal more than two scalars. Building one from scratch removed
    # every cloud from the sky (cloud-ish pixels 9.6% -> 0.0%) rather than adding any.
    #
    # Duplicating the live object rather than the path because the engine's sky content is
    # not in the asset registry: EditorAssetLibrary.duplicate_asset on
    # /Engine/EngineSky/... finds nothing, but the component hands over the loaded object.
    if unreal.EditorAssetLibrary.does_asset_exist(CLOUD_MATERIAL_PATH):
        mi = unreal.EditorAssetLibrary.load_asset(CLOUD_MATERIAL_PATH)
    else:
        mi = unreal.EditorAssetLibrary.duplicate_loaded_asset(current, CLOUD_MATERIAL_PATH)
        if mi is None:
            fail("could not duplicate the cloud material")
            return

    lib = unreal.MaterialEditingLibrary
    lib.set_material_instance_scalar_parameter_value(mi, "Cloud_GlobalCoverage", CLOUD_COVERAGE)
    lib.set_material_instance_scalar_parameter_value(mi, "Cloud_GlobalDensity", CLOUD_DENSITY)
    unreal.EditorAssetLibrary.save_asset(mi.get_path_name(), only_if_is_dirty=False)

    comp.set_editor_property("material", mi)
    comp.set_editor_property("render_in_main_pass", CLOUDS_RENDER_IN_MAIN_PASS)
    say("clouds: drawn %s, coverage %.2f, density %.3f via %s" % (CLOUDS_RENDER_IN_MAIN_PASS, CLOUD_COVERAGE, CLOUD_DENSITY, CLOUD_MATERIAL_PATH))


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
              ("bloom_intensity", 0.4),
              ("white_temp", WHITE_TEMP_KELVIN)]
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

        if not comp.get_editor_property("cast_cloud_shadows"):
            fail("cloud shadows are off")
            ok = False
        else:
            say("PASS cloud shadows on, extent %.0f km, resolution scale x%.0f"
                % (comp.get_editor_property("cloud_shadow_extent"),
                   comp.get_editor_property("cloud_shadow_map_resolution_scale")))

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
        got = comp.get_editor_property("intensity")
        if abs(got - SKY_LIGHT_INTENSITY) > 1e-4:
            fail("sky light intensity is %r, expected %r" % (got, SKY_LIGHT_INTENSITY))
            ok = False
        else:
            say("PASS sky light intensity = %r" % got)

    fog = find_one(unreal.ExponentialHeightFog)
    if fog is not None:
        comp = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
        for prop, expected in (("fog_density", FOG_DENSITY),
                               ("fog_height_falloff", FOG_HEIGHT_FALLOFF),
                               ("start_distance", FOG_START_DISTANCE)):
            got = comp.get_editor_property(prop)
            if abs(got - expected) > 1e-4:
                fail("fog %s is %r, expected %r" % (prop, got, expected))
                ok = False
            else:
                say("PASS fog %s = %r" % (prop, got))

    clouds = [a for a in actors().get_all_level_actors() if isinstance(a, unreal.VolumetricCloud)]
    if clouds:
        cmat = clouds[0].get_component_by_class(unreal.VolumetricCloudComponent).get_editor_property("material")
        got = unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(
            cmat, "Cloud_GlobalCoverage")
        if abs(got - CLOUD_COVERAGE) > 1e-4:
            fail("cloud coverage is %r, expected %r" % (got, CLOUD_COVERAGE))
            ok = False
        else:
            say("PASS cloud coverage = %.2f (engine default was -0.20)" % got)

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
    cloud = find_one(unreal.VolumetricCloud)
    if cloud is not None:
        set_clouds(cloud)
    atmosphere = find_one(unreal.SkyAtmosphere)
    if atmosphere is not None:
        set_sky_atmosphere(atmosphere)
    add_post_process()
    delete_floor()

    levels.save_current_level()
    say("saved level")

    say("ALL VERIFIED" if verify() else "VERIFY FAILED")
    say("DONE")


run()
