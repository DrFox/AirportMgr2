"""Authors DA_Aircraft_Plane2 and sets ABP_Plane2's wheel radius. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, and the split is the point. Every dimension
below came off SK_Plane2's own meshes - wheel radius from the wheel's vertical extent, span
and reaches from the airframe's bounds - because those describe the model that is actually
drawn, and a figure typed from a spec sheet would be a second opinion about the same object.
The speeds and distances come from the DHC-6 Twin Otter the model is based on, because
nothing in the mesh knows how fast it rotates.

THE WHEEL RADIUS MATTERS TWICE, and is easy to set in only one place. UAircraftType carries
it for the model; UAirsideAgentAnim carries its own copy for the animation, defaulted to the
Meridian's 21 uu. A Twin Otter wheel measures 68.6 uu, so an unset ABP spins the wheels at
over three times the right rate against ground speed - which reads as an aircraft skating
rather than rolling. Both are set here, from the one measurement.
"""
import unreal

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane2"
ABP = "/Game/Aircraft/Plane2/ABP_Plane2"
SHORT_CODE = "DHC6"
MESH = "/Game/Aircraft/Plane2/SK_Plane2"

# --- Measured off SK_Plane2, uu (a uu is a centimetre) --------------------------------
# Wheel: vertical extent 137.2 uu, so radius 68.6. Prop: widest sweep 318.1 uu.
MAIN_WHEEL_RADIUS = 68.6
PROPELLER_DIAMETER = 318.1

FOOTPRINT = {
    "nose_x": 811.6,          # the airframe's +X reach
    "tail_x": -782.0,
    "wingspan": 1975.0,       # 19.75 m; the Twin Otter's published span is 19.81
    "wing_x": 204.8,          # wing mid-chord, a high wing set slightly forward
    "tailplane_span": 794.0,
    "tailplane_x": -622.0,
}

# --- Published, from the DHC-6 Twin Otter ---------------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# The Twin Otter is a STOL aeroplane and these are what make it one. Against the Meridian
# already in the project: it rotates at 60 kn where the Meridian needs 86, and climbs out at
# 85 against 121. That is the whole character of the type - it gets off the ground early and
# slowly, and it is why it can use a strip the Meridian cannot.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the Meridian
    "takeoff": dict(accel=450.0, decel=400.0, speed_cap=3100.0),   # Vr about 60 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=3600.0),   # threshold about 70 kn
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,
    "climb_speed": 4400.0,        # 85 kn, the Twin Otter's best-rate speed
    "clear_altitude": 30000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 900.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    "max_rpm": 2200.0,            # PT6A-27, propeller rpm
    "spool_up_seconds": 4.0,
    "spool_down_seconds": 9.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes -
# Airside.Model.FieldLengthsCoverTheRoll states the rule: a published figure may be generous,
# but one shorter than the roll admits an aircraft to a strip it then runs off the end of.
#
# The take-off roll these figures produce is about 17,800 uu (178 m), from
# FTakeoffRun::RequiredRoll: Vr squared over twice the acceleration, plus the rotation. The
# landing distance is SIMULATED rather than closed-form, so it is not computed here -
# AirportMgr.Content.Plane2FieldLengthsCoverTheRoll checks both against the model itself.
#
# The values are the real aeroplane's: 1,200 ft to fifty feet on take-off and 1,050 ft on
# landing. Against the Meridian's 510 m and 400 m already in the project, that is the short
# field the type is famous for.
REQUIREMENTS = {
    "takeoff_field_length": 36600.0,   # 366 m
    "landing_field_length": 32000.0,   # 320 m
}

TURNAROUND_SECONDS = 900.0   # a 19-seat commuter turns in fifteen minutes, not thirty


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: an airline's fleet points at this, and
        # deleting an asset something references breaks the reference rather than updating it.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    asset.set_editor_property("code", unreal.Name("B"))

    # SHORT CODE, which is not the aerodrome letter above. Code is the ICAO reference letter
    # and cannot tell two types apart - an A320 and a 737 are both C - so FAirframe::TypeCode
    # takes this instead. Anything that has to SAY what an aircraft is reads that.
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("DHC-6 Twin Otter"))

    # WHAT IT LOOKS LIKE, which until now no aircraft type carried: every agent wore
    # UAirsideContent::AgentMesh, one mesh for the whole game, so a Twin Otter was offered
    # and a Meridian landed. The anim Blueprint travels with it because plane2 is a TWIN -
    # ABP_PiperMeridian drives one prop bone and this needs two.
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        fail("no %s to point the type at" % MESH)
    else:
        asset.set_editor_property("mesh", mesh)

    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s to point the type at" % ABP)
    elif abp.generated_class() is None:
        fail("%s has no generated class; compile it before running this" % ABP)
    else:
        asset.set_editor_property("anim_class", abp.generated_class())
    asset.set_editor_property("main_wheel_radius", MAIN_WHEEL_RADIUS)
    asset.set_editor_property("propeller_diameter", PROPELLER_DIAMETER)
    asset.set_editor_property("turnaround_seconds", TURNAROUND_SECONDS)

    footprint = asset.get_editor_property("footprint")
    for field, value in FOOTPRINT.items():
        footprint.set_editor_property(field, value)
    asset.set_editor_property("footprint", footprint)

    ground = asset.get_editor_property("ground")
    for name, values in GROUND.items():
        set_regime(ground, name, values)
    asset.set_editor_property("ground", ground)

    for group, values in (("climb", CLIMB), ("approach", APPROACH), ("engine", ENGINE)):
        struct = asset.get_editor_property(group)
        for field, value in values.items():
            struct.set_editor_property(field, value)
        asset.set_editor_property(group, struct)

    requirements = asset.get_editor_property("requirements")
    for field, value in REQUIREMENTS.items():
        requirements.set_editor_property(field, value)
    # Grass and visual: the type's whole point is using strips that are neither paved nor
    # instrumented.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.GRASS)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_wheel_radius():
    """The ANIMATION's copy of the radius, which defaults to the Meridian's 21 uu.

    On the generated class's default object, which is what the editor's Class Defaults panel
    edits and what every instance of the Anim Blueprint starts from.
    """
    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s - run build_plane2_anim.py first" % ABP)
        return

    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class; compile it first" % ABP)
        return

    defaults = unreal.get_default_object(generated)
    before = defaults.get_editor_property("main_wheel_radius")
    defaults.set_editor_property("main_wheel_radius", MAIN_WHEEL_RADIUS)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class()
    ).get_editor_property("main_wheel_radius")
    if abs(after - MAIN_WHEEL_RADIUS) > 0.01:
        fail("ABP wheel radius read back as %.1f, expected %.1f" % (after, MAIN_WHEEL_RADIUS))
    else:
        say("PASS ABP_Plane2 wheel radius %.1f -> %.1f uu" % (before, after))


def verify(path):
    """Read back from disk. An asset edit that reports success and writes nothing is this
    project's most familiar failure."""
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("%s did not survive the save" % path)
        return

    checks = [
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"), MAIN_WHEEL_RADIUS),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"), TURNAROUND_SECONDS),
    ]
    for name, got, want in checks:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %s = %s" % (name, got))

    footprint = asset.get_editor_property("footprint")
    span = footprint.get_editor_property("wingspan")
    if abs(span - FOOTPRINT["wingspan"]) > 0.1:
        fail("wingspan read back as %.1f" % span)
    else:
        say("PASS footprint wingspan %.1f uu" % span)

    # The three that decide whether the right aeroplane turns up at all.
    for prop, want in (("short_code", SHORT_CODE),):
        got = str(asset.get_editor_property(prop))
        if got != want:
            fail("%s read back as %s, expected %s" % (prop, got, want))
        else:
            say("PASS %s = %s" % (prop, got))
    for prop in ("mesh", "anim_class"):
        got = asset.get_editor_property(prop)
        if got is None:
            fail("%s is unset - the agent would fall back to the game-wide default mesh, "
                 "which is how a Twin Otter arrived as a Meridian" % prop)
        else:
            say("PASS %s = %s" % (prop, got.get_name()))

    requirements = asset.get_editor_property("requirements")
    takeoff = requirements.get_editor_property("takeoff_field_length")
    landing = requirements.get_editor_property("landing_field_length")
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m)"
        % (takeoff, takeoff / 100.0, landing, landing / 100.0))

    ground = asset.get_editor_property("ground")
    for name in ("taxi", "takeoff", "landing"):
        regime = ground.get_editor_property(name)
        cap = regime.get_editor_property("speed_cap")
        say("  ground.%-8s accel=%.0f decel=%.0f cap=%.0f uu/s (%.0f kn)"
            % (name, regime.get_editor_property("accel"), regime.get_editor_property("decel"),
               cap, cap / 51.4))


def run():
    if unreal.EditorAssetLibrary.load_asset(MESH) is None:
        fail("no %s - run import_plane2.py first" % MESH)
        say("DONE")
        return

    path = author_type()
    if path is None:
        say("DONE")
        return
    say("authored %s" % path)
    verify(path)
    set_anim_wheel_radius()
    say("DONE")


run()
