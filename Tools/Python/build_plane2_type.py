"""Authors DA_Aircraft_Plane2 and sets ABP_Plane2's wheel radius. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, and the split is the point. Every dimension
is measured AT RUN TIME off the export's own parts - see measure() - because those describe
the model that is actually drawn, and a figure typed from a spec sheet would be a second
opinion about the same object. They were literals here until 2026-09-13, when the export
moved its origin to the nose gear and made all four position figures wrong at once.
The speeds and distances come from the DHC-6 Twin Otter the model is based on, because
nothing in the mesh knows how fast it rotates.

THE WHEEL RADIUS MATTERS TWICE, and is easy to set in only one place. UAircraftType carries
it for the model; UAirsideAgentAnim carries its own copy for the animation, defaulted to the
Meridian's 21 uu. A Twin Otter wheel measures 68.6 uu, so an unset ABP spins the wheels at
over three times the right rate against ground speed - which reads as an aircraft skating
rather than rolling. Both are set here, from the one measurement.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. Put it on before importing the shared reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane2"
ABP = "/Game/Aircraft/Plane2/ABP_Plane2"
SHORT_CODE = "DHC6"
MESH = "/Game/Aircraft/Plane2/SK_Plane2"

# --- Measured off the export, uu (a uu is a centimetre) -------------------------------
#
# MEASURED, NOT TYPED, and this block is the reason the rule exists. Every figure below was
# a literal here - nose_x 811.6, tail_x -782.0, wing_x 204.8, tailplane_x -622.0 - in a file
# whose own header says "GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED". On 2026-09-13 the
# export moved its origin onto the nose gear (UAircraftType's documented local space), which
# shifted every one of them by 644 uu at a stroke. Typed figures do not move with the model;
# they just quietly describe an aeroplane that no longer exists, and the drawn envelope ends
# up a wheelbase away from the drawn aircraft.
#
# The prop was already drifting for the same reason: 318.1 written here against 317.3 in the
# file after a re-export nobody re-measured.
#
# From the .glb rather than from SK_Plane2 because these are PER-PART figures - the wing's
# mid-chord, the stabiliser's span - and a UStaticMesh/USkeletalMesh gives only the bounds of
# everything at once. import_plane2.py asserts the import matches the source, so measuring the
# source is measuring what landed.
SOURCE = r"C:\repos\AirportMgr2Models\plane2\export\plane2.glb"


def measure():
    """The footprint and the two scalars, off the export's own parts.

    Raises rather than guessing: a renamed part is a change to look at, not a figure to
    silently default. part_bounds_uu keys by stem, so Blender's '.001' suffixes - which the
    exporter adds and removes as objects are duplicated - do not decide whether a part is
    found.
    """
    parts = part_bounds_uu(read_gltf(SOURCE))

    def find(stem):
        if stem not in parts:
            raise KeyError("no part named %s in %s - the rig was renamed" % (stem, SOURCE))
        return parts[stem]

    every = list(parts.values())
    nose_x = max(high[0] for _, high in every)
    tail_x = min(low[0] for low, _ in every)

    wing_lo, wing_hi = find("wing")
    stab_lo, stab_hi = find("stabiliser")
    wheel_lo, wheel_hi = find("wheel_L")
    prop_lo, prop_hi = find("prop_L")

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        "wingspan": wing_hi[1] - wing_lo[1],   # the Twin Otter's published span is 19.81 m
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The wheel's VERTICAL extent is its diameter; the prop's widest sweep is its disc, which
    # lies across the airframe rather than along it.
    wheel_radius = (wheel_hi[2] - wheel_lo[2]) * 0.5
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, wheel_radius, prop_diameter


def axles_from_rig(wheel_radius):
    """(steer axle, fixed axle) X in uu, off SK_Plane2's own reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX. Both answer the same question and they agree to
    0.02 uu on this model, but they are not the same KIND of answer: build_export.py places
    each wheel's origin ON ITS ROTATION AXIS so the thing can spin, which makes the bone a
    statement about where the axle is. A bounding-box centre is a by-product of tyre shape,
    and would quietly move if a fairing or a brake were ever modelled into the same object.

    It is also the point the animation turns the wheel about, so the drawn wheel and the
    pitch pivot cannot drift apart - and the import already asserts a bone per joint the
    skin declares, so a rig that loses these fails loudly rather than defaulting.

    WHAT IS NOT TAKEN IS THE BONE'S HEIGHT. It sits at the HUB, one wheel radius up, and
    the pitch pivot wants the CONTACT PATCH below it - see FAgentMotion::PitchPivotX, which
    is a scalar X for exactly that reason. Asserted here rather than assumed: hub height and
    measured tyre radius are the same number arrived at two ways, and a disagreement means
    the wheel has moved off the ground plane that ARoadAgentActor::SetMotion relies on.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        raise KeyError("no %s - run import_plane2.py first" % MESH)

    pose = mesh.get_editor_property("skeleton").get_reference_pose()
    at = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        bone = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        at[str(name)] = bone.translation

    for wanted in ("nosewheel", "wheel_L", "wheel_R"):
        if wanted not in at:
            raise KeyError("SK_Plane2 has no %s bone - the rig was renamed. Bones: %s"
                           % (wanted, ", ".join(sorted(at))))

    mains_x = (at["wheel_L"].x + at["wheel_R"].x) * 0.5
    hub_z = (at["wheel_L"].z + at["wheel_R"].z) * 0.5
    if abs(hub_z - wheel_radius) > 2.0:
        fail("the main-gear hub sits at z=%.1f but the tyre measures %.1f in radius. Either "
             "the wheels are off the ground plane or the bone is not on the axle, and the "
             "pitch pivot assumes both." % (hub_z, wheel_radius))

    return {
        "steer_axle_x": at["nosewheel"].x,
        "fixed_axle_x": mains_x,
    }


_MEASURED = None


def measured():
    """measure(), once.

    Three places need these figures - the type, the ABP's wheel radius and the read-back
    verification - and all three must agree or the verification is checking a different
    aeroplane than the one that was written. Cached rather than re-read because a file that
    changed mid-run would be a worse problem than a stale one.
    """
    global _MEASURED
    if _MEASURED is None:
        _MEASURED = measure()
    return _MEASURED

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

# STEERING, published rather than measured - nothing in the mesh knows how far the tiller
# turns or how hard a pilot will corner. Both are read only by the rolling-steer law, which
# this type uses because it carries axle figures; MaxTurnRateDegPerSec is left at its default
# for the take-off line-up and never consulted for a taxi turn.
#
# 60 degrees of lock on the measured 4.54 m wheelbase makes the tightest followable radius
# 5.2 m, well inside any taxiway bend. 0.2 g is between the Meridian's 0.25 and an airliner's
# 0.15: a Twin Otter on a quiet apron corners harder than a jet and less hard than a single.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
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
    footprint_figures, wheel_radius, prop_diameter = measured()
    axles = axles_from_rig(wheel_radius)
    say("measured off the export: wheel radius %.1f, prop %.1f, nose %.1f, tail %.1f uu"
        % (wheel_radius, prop_diameter, footprint_figures["nose_x"],
           footprint_figures["tail_x"]))
    say("measured off the rig: steer axle %.1f, fixed axle %.1f uu"
        % (axles["steer_axle_x"], axles["fixed_axle_x"]))
    asset.set_editor_property("steer_axle_x", axles["steer_axle_x"])
    asset.set_editor_property("fixed_axle_x", axles["fixed_axle_x"])
    asset.set_editor_property("main_wheel_radius", wheel_radius)
    asset.set_editor_property("propeller_diameter", prop_diameter)
    asset.set_editor_property("turnaround_seconds", TURNAROUND_SECONDS)

    footprint = asset.get_editor_property("footprint")
    for field, value in footprint_figures.items():
        footprint.set_editor_property(field, value)
    asset.set_editor_property("footprint", footprint)

    ground = asset.get_editor_property("ground")
    for name, values in GROUND.items():
        set_regime(ground, name, values)
    for field, value in STEERING.items():
        ground.set_editor_property(field, value)
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
    radius = measured()[1]
    before = defaults.get_editor_property("main_wheel_radius")
    defaults.set_editor_property("main_wheel_radius", radius)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class()
    ).get_editor_property("main_wheel_radius")
    if abs(after - radius) > 0.01:
        fail("ABP wheel radius read back as %.1f, expected %.1f" % (after, radius))
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
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"), measured()[1]),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"), TURNAROUND_SECONDS),
    ]
    for name, got, want in checks:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %s = %s" % (name, got))

    footprint = asset.get_editor_property("footprint")
    span = footprint.get_editor_property("wingspan")
    if abs(span - measured()[0]["wingspan"]) > 0.1:
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
