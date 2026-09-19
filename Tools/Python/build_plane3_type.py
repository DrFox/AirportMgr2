"""Authors DA_Aircraft_Plane3 and sets ABP_Plane3's wheel radius. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as build_plane2_type.py states it
and for the same reason: a figure typed here is a second opinion about an object the export
already answers for, and it does not move when the model does. The measuring half is now
shared - airside_import.part_bounds_uu - so the two scripts cannot disagree about which
glTF axis is UE's span.

WHAT THIS TYPE IS FOR, beyond being another aeroplane. plane2 is the STOL end of the range:
grass, 366 m, 19 seats. The Dash 8-Q400 is the step up that makes a runway a DECISION - it
wants tarmac and 1,402 m, nearly four times plane2's take-off field length, so an airport
that can take one is an airport that was built for one. The progression is the content, and
RunwayAdmission is what enforces it.

THE WHEEL RADIUS COMES OFF THE RIG HERE, NOT OFF THE TYRE. plane2 measures its wheel_L mesh,
whose vertical extent IS the tyre's diameter, and asserts the hub bone agrees. plane3's gear
used to be one object per leg - gearRear_L was tyre AND strut, 237 uu tall against a tyre
nearer 90 - so a bounding box would have claimed a wheel two and a half times too big and
spun it at two fifths of the right rate.

THE GEAR WAS SEPARATED ON 2026-09-19 (three wheels, three legs, because skinning a combined
mesh per vertex was rotating the legs in engine), so a tyre box would now be available. This
still reads the BONE, for the reason that was always the stronger one: build_export.py puts
each wheel's origin ON its rotation axis, and the importer has already asserted the model
sits on z = 0, so the hub's height above that contact plane IS the radius - a statement about
where the wheel turns, not a by-product of tyre shape. The leg's box stays as a sanity bound:
a radius may not exceed the leg that carries it, and gearRear_L is now purely that leg.
"""
import math
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. Put it on before importing the shared reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane3"
ABP = "/Game/Aircraft/Plane3/ABP_Plane3"
SHORT_CODE = "DH8D"
MESH = "/Game/Aircraft/Plane3/SK_Plane3"

SOURCE = r"C:\repos\AirportMgr2Models\plane3\export\plane3.glb"

# The parts this type is measured from. NAMED HERE rather than assumed equal to plane2's,
# because they are not: plane3's lifting surface is "wing2" and its gear legs are
# "gearRear_L/R" where plane2 has "wing" and "wheel_L/R". A wrong stem raises.
WING = "wing2"
STABILISER = "stabiliser"
GEAR_LEG = "gearRear_L"
PROP = "prop_L"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def measure():
    """The footprint, the propeller and the gear leg's height, off the export's own parts.

    Raises rather than guessing: a renamed part is a change to look at, not a figure to
    silently default.
    """
    parts = part_bounds_uu(read_gltf(SOURCE))

    def find(stem):
        if stem not in parts:
            raise KeyError("no part named %s in %s - the rig was renamed. Parts: %s"
                           % (stem, SOURCE, ", ".join(sorted(parts))))
        return parts[stem]

    every = list(parts.values())
    nose_x = max(high[0] for _, high in every)
    tail_x = min(low[0] for low, _ in every)

    wing_lo, wing_hi = find(WING)
    stab_lo, stab_hi = find(STABILISER)
    prop_lo, prop_hi = find(PROP)
    leg_lo, leg_hi = find(GEAR_LEG)

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        "wingspan": wing_hi[1] - wing_lo[1],   # the Q400's published span is 28.42 m
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The prop's widest sweep is its disc, which lies across the airframe rather than along
    # it - so the larger of the two cross-axis extents, never the chordwise one.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane3's reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, for the two reasons the header gives: the bone is
    a STATEMENT about where the axle is - build_export.py places each wheel's origin on its
    rotation axis so the thing can spin - and on this model the tyre has no bounding box of
    its own to take, because the leg is one object.

    The hub's HEIGHT is the radius because z = 0 is the contact plane. That is not assumed:
    airside_import.report_bounds FAILS an import whose lowest vertex is more than 10 uu off
    z = 0, so a model that floats never reaches this script.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        raise KeyError("no %s - run import_models.py first" % MESH)

    pose = mesh.get_editor_property("skeleton").get_reference_pose()
    at = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        bone = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        at[str(name)] = bone.translation

    for wanted in ("nosewheel", "wheel_L", "wheel_R"):
        if wanted not in at:
            raise KeyError("SK_Plane3 has no %s bone - the rig was renamed. Bones: %s"
                           % (wanted, ", ".join(sorted(at))))

    mains_x = (at["wheel_L"].x + at["wheel_R"].x) * 0.5
    radius = (at["wheel_L"].z + at["wheel_R"].z) * 0.5

    # THE TRACK, MEASURED, for the same reason the axles are: it is a fact about the model
    # that is drawn, not about the aeroplane in the datasheet. Anything hung off it - tyre
    # smoke at touchdown, a tug lining up, wheel spray - lands under the wheels the player
    # can actually see, and stays there if the model is ever re-rigged. A published figure
    # would be right about the Q400 and wrong about SK_Plane3 the moment the two differ.
    track = abs(at["wheel_L"].y - at["wheel_R"].y)
    if track <= radius:
        raise ValueError("wheel_L and wheel_R are %.1f uu apart, which is inside one wheel's "
                         "%.1f uu radius - the two main gear bones are on top of each other"
                         % (track, radius))

    # A RADIUS MAY NOT EXCEED THE LEG THAT CARRIES IT, and it may not be nothing. The two
    # bounds catch the same failure from opposite sides: a rig whose wheel bones were left at
    # the root rather than put on the axle reads as radius 0, and one exported in metres
    # rather than centimetres reads as a wheel taller than the aeroplane. Either would set
    # the animation's spin rate to something nobody would recognise as wrong on screen.
    if not 5.0 < radius < leg_height:
        raise ValueError("the main-gear hub sits at z=%.1f against a %.1f uu leg - the bone "
                         "is not on the axle, or the model is not on the ground plane"
                         % (radius, leg_height))

    return at["nosewheel"].x, mains_x, radius, track


_MEASURED = None


def measured():
    """measure() and axles_and_radius(), once.

    Three places need these figures - the type, the ABP's wheel radius and the read-back
    verification - and all three must agree or the verification is checking a different
    aeroplane than the one that was written.
    """
    global _MEASURED
    if _MEASURED is None:
        footprint, prop, leg = measure()
        steer_x, fixed_x, radius, track = axles_and_radius(leg)
        _MEASURED = dict(footprint=footprint, prop_diameter=prop, wheel_radius=radius,
                         steer_axle_x=steer_x, fixed_axle_x=fixed_x, main_gear_track=track)
    return _MEASURED


def tightest_radius_uu():
    """The tightest corner the rolling-steer law will follow, uu: R >= L / sin(lock).

    Derived here rather than typed into a comment, because the wheelbase is measured and a
    typed figure would stop being true the moment the model moved.
    """
    m = measured()
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    return wheelbase / math.sin(math.radians(STEERING["max_steer_degrees"]))


# --- Published, from the de Havilland Canada Dash 8-Q400 ------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# A 29.5 t turboprop airliner, and every figure below is what separates it from the two light
# types already in the project. It rotates at 120 kn where plane2 rotates at 60, and climbs
# out at 170 against 85 - so it leaves the ground later, faster, and needs a strip plane2
# would regard as an aerodrome in its own right.
GROUND = {
    # Taxi as the others do: an airliner does not taxi faster than a Twin Otter, it just
    # takes longer to stop, which is what the decel figure says.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=6168.0),   # Vr about 120 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=6425.0),   # Vref about 125 kn
}

# STEERING, published rather than measured - nothing in the mesh knows how far the tiller
# turns or how hard a pilot will corner.
#
# 70 DEGREES IS THE TILLER'S AUTHORITY, not a comfortable taxi angle, and on the measured
# 14.05 m wheelbase it makes the tightest followable radius about 15 m (R >= L / sin(lock),
# the rule FSpeedProfile applies). That is a REQUIREMENT ON THE AIRPORT, and the first thing
# to suspect if a Q400 refuses a route plane2 takes happily: a hand-drawn taxiway bend
# tighter than that is unfollowable for this type and followable for everything else in the
# game. tightest_radius_uu() computes it and author_type() logs it for exactly that reason.
#
# 0.15 g is the airliner end of the range - plane2 corners at 0.2, the Meridian at 0.25. A
# 29 t aeroplane is driven round an apron more gently than a 5 t one.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 147.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 8738.0,        # 170 kn, the Q400's climb speed
    "clear_altitude": 40000.0,    # 400 m; it leaves the circuit higher than the light types
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    "max_rpm": 1020.0,            # PW150A driving a Dowty R408: 1,020 prop rpm at 100% Np
    "spool_up_seconds": 5.0,
    "spool_down_seconds": 10.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes -
# AirportMgr.Content.FieldLengthsCoverTheRoll states the rule: a published figure may be
# generous, but one shorter than the roll admits an aircraft to a strip it then runs off the
# end of. The values are the real aeroplane's at MTOW, sea level, ISA.
REQUIREMENTS = {
    "takeoff_field_length": 140200.0,   # 1,402 m
    "landing_field_length": 128700.0,   # 1,287 m
}

TURNAROUND_SECONDS = 1500.0   # 78 seats through one airstair door: twenty-five minutes


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: an airline's fleet may come to point at
        # this, and deleting an asset something references breaks the reference rather than
        # updating it.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    # CODE C, the aerodrome reference letter, because the measured span is 28.2 m and Code C
    # runs to 36. It shares that letter with the A320 and the 737 - which is exactly why
    # ShortCode exists beside it, and why the stand it parks on is a DA_Stand_CodeC.
    asset.set_editor_property("code", unreal.Name("C"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("Dash 8-Q400"))

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

    m = measured()
    say("measured off the export: wheel radius %.1f, prop %.1f, nose %.1f, tail %.1f uu"
        % (m["wheel_radius"], m["prop_diameter"], m["footprint"]["nose_x"],
           m["footprint"]["tail_x"]))
    say("measured off the rig: steer axle %.1f, fixed axle %.1f uu (wheelbase %.1f)"
        % (m["steer_axle_x"], m["fixed_axle_x"],
           abs(m["fixed_axle_x"] - m["steer_axle_x"])))

    # THE FIGURE THAT DECIDES WHETHER THIS TYPE CAN USE THE PLAYER'S TAXIWAYS, said out loud
    # because it is not otherwise visible anywhere: the rolling-steer law refuses a corner
    # tighter than this, and a refusal reads as "no route" rather than as "too big".
    say("tightest followable radius %.0f uu (%.1f m) at %.0f degrees of lock"
        % (tightest_radius_uu(), tightest_radius_uu() / 100.0,
           STEERING["max_steer_degrees"]))

    asset.set_editor_property("steer_axle_x", m["steer_axle_x"])
    asset.set_editor_property("fixed_axle_x", m["fixed_axle_x"])
    asset.set_editor_property("main_gear_track", m["main_gear_track"])
    asset.set_editor_property("main_wheel_radius", m["wheel_radius"])
    asset.set_editor_property("propeller_diameter", m["prop_diameter"])
    asset.set_editor_property("turnaround_seconds", TURNAROUND_SECONDS)

    footprint = asset.get_editor_property("footprint")
    for field, value in m["footprint"].items():
        footprint.set_editor_property(field, value)
    asset.set_editor_property("footprint", footprint)

    ground = asset.get_editor_property("ground")
    for name, values in GROUND.items():
        set_regime(ground, name, values)
    for field, value in STEERING.items():
        ground.set_editor_property(field, value)
    asset.set_editor_property("ground", ground)

    for group, values in (("climb", CLIMB), ("approach", APPROACH), ("engine", ENGINE)):
        block = asset.get_editor_property(group)
        for field, value in values.items():
            block.set_editor_property(field, value)
        asset.set_editor_property(group, block)

    requirements = asset.get_editor_property("requirements")
    for field, value in REQUIREMENTS.items():
        requirements.set_editor_property(field, value)
    # TARMAC AND VISUAL. The surface is the whole difference from plane2, which takes grass:
    # a Q400 in scheduled service wants a paved runway, and requiring one is what makes the
    # player build a real one. The approach stays visual - an instrument approach is a
    # building the game does not have yet, and demanding one would refuse every runway.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_wheel_radius():
    """The ANIMATION's copy of the radius, which defaults to the Meridian's 21 uu.

    On the generated class's default object, which is what the editor's Class Defaults panel
    edits and what every instance of the Anim Blueprint starts from. Left unset, a Q400's
    wheels turn at over twice the right rate against ground speed - which reads as skating.
    """
    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s" % ABP)
        return

    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class; compile it first" % ABP)
        return

    defaults = unreal.get_default_object(generated)
    radius = measured()["wheel_radius"]
    before = defaults.get_editor_property("main_wheel_radius")
    defaults.set_editor_property("main_wheel_radius", radius)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class()
    ).get_editor_property("main_wheel_radius")
    if abs(after - radius) > 0.01:
        fail("ABP wheel radius read back as %.1f, expected %.1f" % (after, radius))
    else:
        say("PASS ABP_Plane3 wheel radius %.1f -> %.1f uu" % (before, after))


def verify(path):
    """Read back from disk. An asset edit that reports success and writes nothing is this
    project's most familiar failure."""
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("%s did not survive the save" % path)
        return

    m = measured()
    checks = [
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"),
         m["wheel_radius"]),
        ("fixed_axle_x", asset.get_editor_property("fixed_axle_x"), m["fixed_axle_x"]),
        ("main_gear_track", asset.get_editor_property("main_gear_track"), m["main_gear_track"]),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"),
         TURNAROUND_SECONDS),
    ]
    for name, got, want in checks:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %s = %.2f" % (name, got))

    footprint = asset.get_editor_property("footprint")
    span = footprint.get_editor_property("wingspan")
    if abs(span - m["footprint"]["wingspan"]) > 0.1:
        fail("wingspan read back as %.1f" % span)
    else:
        say("PASS footprint wingspan %.1f uu (%.1f m)" % (span, span / 100.0))

    for prop, want in (("short_code", SHORT_CODE), ("code", "C")):
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
    say("PASS surface %s, approach %s"
        % (requirements.get_editor_property("minimum_surface"),
           requirements.get_editor_property("approach_needed")))

    ground = asset.get_editor_property("ground")
    for name in ("taxi", "takeoff", "landing"):
        regime = ground.get_editor_property(name)
        cap = regime.get_editor_property("speed_cap")
        say("  ground.%-8s accel=%.0f decel=%.0f cap=%.0f uu/s (%.0f kn)"
            % (name, regime.get_editor_property("accel"),
               regime.get_editor_property("decel"), cap, cap / 51.4))


def run():
    if unreal.EditorAssetLibrary.load_asset(MESH) is None:
        fail("no %s - run import_models.py first" % MESH)
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
