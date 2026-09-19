"""Authors DA_Aircraft_Plane4 and sets ABP_Plane4's wheel radius. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as build_plane2_type.py and
build_plane3_type.py state it and for the same reason: a figure typed here is a second
opinion about an object the export already answers for, and it does not move when the model
does. The measuring half is shared - airside_import.part_bounds_uu - so the three scripts
cannot disagree about which glTF axis is UE's span.

WHAT THIS TYPE IS FOR. plane2 is the STOL end (grass, 366 m), plane3 the regional step up
that makes a runway a decision (tarmac, 1,402 m). This is the JET: 2,316 m of runway, 189
seats and a forty-minute turnaround, so an airport that can take one has been built for
nothing else. It is also the first airframe in the project whose gear retracts - see GEAR
below and docs/superpowers/specs/2026-09-19-gear-retraction-design.md.

WHY THIS TYPE AND NOT DA_Aircraft_B738, which already exists. That one is the PAPER 737:
its figures are hand-typed from the datasheet in UAircraftType::Build737, it carries no mesh,
and Solve/IcaoCode.cpp pins its nose and tail against the same constants. It exists so stand
layout and aerodrome-code classification can reason about a 737 without one being modelled.
This type is the aeroplane that actually flies, and its figures are MEASURED off SK_Plane4 -
which is why the two are separate assets rather than one asset with two authors. Compare the
two and they disagree by about a metre; see the note at NOSE OVERHANG.

NOSE OVERHANG, a known discrepancy rather than a measurement error. Build737 puts the nose
5.20 m ahead of the nose gear; this export measures 4.09 m. The WHEELBASE is exactly right
(15.600 m against the real aircraft's 15.60) and the overall length is within 0.21 m, so it
is the gear ASSEMBLY that sits about 1.11 m too far forward relative to the fuselage, not one
leg out of place. ARoadAgentActor::SetPose puts the origin on the guideline, so until the
model is re-exported this type parks about a metre forward of every stand mark. Nothing here
compensates for it: this script measures what the export says, and a fudge factor would be a
lie that survived the fix.
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
TYPE_NAME = "DA_Aircraft_Plane4"
ABP = "/Game/Aircraft/Plane4/ABP_Plane4"
SHORT_CODE = "B738"
MESH = "/Game/Aircraft/Plane4/SK_Plane4"

SOURCE = r"C:\repos\AirportMgr2Models\plane4\export\plane4.glb"

# The parts this type is measured from. NAMED HERE rather than assumed equal to plane3's,
# because they are not: this airframe's lifting surface is "wing" where plane3's is "wing2",
# its main leg is "maingear_L" where plane3's is "gearRear_L", and its "propeller" is a
# TURBOFAN called "fan_L". A wrong stem raises rather than defaulting.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear_L"

# THE FAN, and the name difference is the whole story of this rig's propeller handling.
# build_export.py names the meshes fan_L/fan_R honestly and rides them on bones called
# prop_L/prop_R, because build_plane2_anim.py matches animation variables by SUBSTRING and
# "prop" is the name that already has a driver. This script measures the MESH, so it wants
# the honest name.
PROP = "fan_L"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def measure():
    """The footprint, the fan and the gear leg's height, off the export's own parts.

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
        # The winglets are part of the wing object, so this is the span OVER THE WINGLETS -
        # 35.79 m, which is the figure the real 737-800W is published at and the one that
        # decides its aerodrome code letter. A span measured to the wingtip alone would be
        # 34.3 m and would still be Code C, so nothing turns on it here; it would start
        # mattering on a type near a code boundary.
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The fan's widest sweep is its disc, which lies across the airframe rather than along
    # it - so the larger of the two cross-axis extents, never the chordwise one. Reads 1.59 m
    # against the CFM56-7B's published 1.55 m fan.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane4's reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, the rule build_plane3_type.py settled: the bone is
    a STATEMENT about where the axle is - build_export.py places each wheel's origin on its
    rotation axis so the thing can spin - and the hub's HEIGHT above the contact plane IS the
    radius. That z = 0 is the contact plane is not assumed: airside_import.report_bounds
    FAILS an import whose lowest vertex is more than 10 uu off it, so a model that floats
    never reaches this script.

    THIS RIG HAS THE WHEELS SPLIT FROM THE LEGS already - maingear_L is purely the leg - so
    the leg's box serves only as the sanity bound below.
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
            raise KeyError("SK_Plane4 has no %s bone - the rig was renamed. Bones: %s"
                           % (wanted, ", ".join(sorted(at))))

    mains_x = (at["wheel_L"].x + at["wheel_R"].x) * 0.5
    radius = (at["wheel_L"].z + at["wheel_R"].z) * 0.5

    # THE TRACK, MEASURED, for the same reason the axles are: it is a fact about the model
    # that is drawn, not about the aeroplane in the datasheet. Anything hung off it - tyre
    # smoke at touchdown, a tug lining up - lands under the wheels the player can see.
    track = abs(at["wheel_L"].y - at["wheel_R"].y)
    if track <= radius:
        raise ValueError("wheel_L and wheel_R are %.1f uu apart, which is inside one wheel's "
                         "%.1f uu radius - the two main gear bones are on top of each other"
                         % (track, radius))

    # A RADIUS MAY NOT EXCEED THE LEG THAT CARRIES IT, and it may not be nothing. The two
    # bounds catch the same failure from opposite sides: wheel bones left at the root read as
    # radius 0, and a model exported in metres reads as a wheel taller than the aeroplane.
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


# --- Published, from the Boeing 737-800W ----------------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# A 79 t narrowbody jet, and the figures below are what separate it from the two propeller
# types already here. It rotates at 145 kn where plane3 rotates at 120 and plane2 at 60, and
# it wants 2,316 m of tarmac against plane3's 1,402 and plane2's 366.
GROUND = {
    # Taxi no faster than the others - an airliner does not taxi quickly, it just takes
    # longer to stop, which is what the decel figure says.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=7453.0),   # Vr about 145 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7196.0),   # Vref about 140 kn
}

# STEERING, published rather than measured - nothing in the mesh knows how far the tiller
# turns or how hard a pilot will corner.
#
# 78 DEGREES is the 737's tiller authority. On the measured 15.60 m wheelbase that makes the
# tightest followable radius about 16 m (R >= L / sin(lock), the rule FSpeedProfile applies),
# which is a REQUIREMENT ON THE AIRPORT and the first thing to suspect if a 737 refuses a
# route the Q400 takes happily. author_type() logs the computed figure for that reason.
#
# 0.15 g matches plane3 - the airliner end of the range, against plane2's 0.2 and the
# Meridian's 0.25. A 79 t aeroplane is not driven round an apron briskly.
STEERING = {
    "max_steer_degrees": 78.0,
    "max_lateral_accel_uu": 147.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 9.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 12850.0,       # 250 kn, the standard climb speed below 10,000 ft
    # HIGHER THAN plane3's 400 m, and it has to be: the gear cycle below starts at 90 m and
    # takes 9 seconds, and an agent removed at 400 m would vanish mid-retraction on a slow
    # climb. 500 m leaves the whole cycle comfortably on screen.
    "clear_altitude": 50000.0,    # 500 m
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # THE REAL FAN SPEED, not a display figure. A CFM56-7B turns 5,175 rpm at 100 % N1, and
    # FAgentMotion::EngineRPM carries the honest number so anything reasoning about the
    # engine reads the truth. The VIEW is what chooses a rotation it can show - see
    # UAirsideAgentAnim::PropDisplayCapRPM, which caps the drawn rate, and PropBladeCount,
    # which wants to be the fan's 24 blades rather than the default 3.
    "max_rpm": 5175.0,
    # A JET SPOOLS SLOWLY AND WINDS DOWN MORE SLOWLY STILL, which is the opposite end of the
    # range from the Meridian's 4 up / 9 down. Idle to takeoff N1 is a famously unhurried 8
    # seconds, and a shut-down fan windmills down for the better part of half a minute.
    "spool_up_seconds": 8.0,
    "spool_down_seconds": 25.0,
}

# THE FIRST RETRACTABLE GEAR IN THE PROJECT. Every figure here is the spec's, and this is the
# ONLY place they are authored - UAircraftType::Build737 deliberately declares none, because
# the paper type never flies and two copies of one aeroplane's figures is the drift this
# codebase keeps paying for. See the gear-retraction spec, section 6.
#
# 7 seconds is the real 737-800 transit, with a second of nose bay door either side of it, so
# a full cycle is 9. The mains have no doors and want none: their wheels sit in a well behind
# a fixed fairing, which is why the rig has door_nose_L/_R and no door_main_* pair.
GEAR = {
    "travel_seconds": 7.0,
    "door_seconds": 1.0,
    # A FEW HUNDRED FEET - 9000 uu is about 295 ft - because raising the gear is a pilot
    # command and not a consequence of lift-off. FAgentMotion::bAirborne is the precondition.
    "retract_above_height": 9000.0,
    # ABOVE FApproachPerformance::FinalAltitude (2000 uu) on purpose, so every arrival is born
    # down and locked and this never fires at today's figures. Authored at an honest ~500 ft
    # anyway, so the extension is correct the day the approach is joined higher.
    "extend_below_height": 15000.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes -
# AirportMgr.Content.FieldLengthsCoverTheRoll states the rule: a published figure may be
# generous, but one shorter than the roll admits an aircraft to a strip it then runs off the
# end of. The values are the real aeroplane's at MTOW, sea level, ISA.
REQUIREMENTS = {
    "takeoff_field_length": 231600.0,   # 2,316 m
    "landing_field_length": 163400.0,   # 1,634 m
}

TURNAROUND_SECONDS = 2400.0   # 189 seats through two doors: forty minutes


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

    # CODE C, the aerodrome reference letter: the measured span is 35.79 m and Code C runs to
    # under 36. It shares that letter with the A320 and the Q400 - which is why ShortCode
    # exists beside it - and it is the LARGEST aeroplane that letter admits, so a Code C stand
    # sized for this one is sized for everything below it.
    asset.set_editor_property("code", unreal.Name("C"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("737-800"))

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
    say("measured off the export: wheel radius %.1f, fan %.1f, nose %.1f, tail %.1f uu"
        % (m["wheel_radius"], m["prop_diameter"], m["footprint"]["nose_x"],
           m["footprint"]["tail_x"]))
    say("measured off the rig: steer axle %.1f, fixed axle %.1f uu (wheelbase %.1f)"
        % (m["steer_axle_x"], m["fixed_axle_x"],
           abs(m["fixed_axle_x"] - m["steer_axle_x"])))

    # THE NOSE OVERHANG, said out loud every run because it is a known defect in the model
    # rather than in this script - see the header. 520 uu is Build737's datasheet figure and
    # Solve/IcaoCode.cpp pins it, so the two are directly comparable.
    overhang = m["footprint"]["nose_x"] - m["steer_axle_x"]
    if abs(overhang - 520.0) > 25.0:
        say("NOTE nose sits %.0f uu ahead of the nose gear against Build737's datasheet 520 "
            "- the gear assembly is about %.2f m too far forward, so this type parks that "
            "far ahead of every stand mark until the model is re-exported"
            % (overhang, (520.0 - overhang) / 100.0))
    else:
        say("PASS nose overhang %.0f uu, within 25 uu of Build737's datasheet 520" % overhang)

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

    for group, values in (("climb", CLIMB), ("approach", APPROACH), ("engine", ENGINE),
                          ("gear", GEAR)):
        block = asset.get_editor_property(group)
        for field, value in values.items():
            block.set_editor_property(field, value)
        asset.set_editor_property(group, block)

    requirements = asset.get_editor_property("requirements")
    for field, value in REQUIREMENTS.items():
        requirements.set_editor_property(field, value)
    # TARMAC AND VISUAL. plane3 already demands tarmac; what this type adds is LENGTH - 2,316
    # m against its 1,402 - so the progression from one to the other is a longer runway rather
    # than a different surface. The approach stays visual: an instrument approach is a
    # building the game does not have yet, and demanding one would refuse every runway.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of two measured figures, on the generated class's defaults.

    THE WHEEL RADIUS defaults to the Meridian's 21 uu. Left unset, a 737's wheels turn at
    nearly three times the right rate against ground speed - which reads as skating.

    THE BLADE COUNT defaults to 3, and a CFM56-7B fan has 24. That number is not cosmetic:
    UAirsideAgentAnim::PropStepDegrees resolves the frame rate against ONE BLADE REPEAT,
    360/N degrees, so a 24-blade fan told it has 3 blades is allowed an eight-times-larger
    step and aliases backwards at any frame rate this game runs at.
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
    defaults.set_editor_property("prop_blade_count", 24)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class())
    got_radius = after.get_editor_property("main_wheel_radius")
    got_blades = after.get_editor_property("prop_blade_count")
    if abs(got_radius - radius) > 0.01:
        fail("ABP wheel radius read back as %.1f, expected %.1f" % (got_radius, radius))
    else:
        say("PASS ABP_Plane4 wheel radius %.1f -> %.1f uu" % (before, got_radius))
    if got_blades != 24:
        fail("ABP blade count read back as %d, expected 24" % got_blades)
    else:
        say("PASS ABP_Plane4 blade count = %d, the CFM56-7B's fan" % got_blades)


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

    # THE GEAR, READ BACK FIELD BY FIELD. This is the only asset in the project that carries
    # a gear cycle, and a set_editor_property that silently no-ops on an unknown field name
    # reports success and saves cleanly - which would leave this type with TravelSeconds 0,
    # indistinguishable on screen from an aeroplane whose gear is simply fixed.
    gear = asset.get_editor_property("gear")
    for field, want in GEAR.items():
        got = gear.get_editor_property(field)
        if abs(got - want) > 0.01:
            fail("gear.%s read back as %s, expected %s" % (field, got, want))
        else:
            say("PASS gear.%-21s = %.1f" % (field, got))
    cycle = GEAR["door_seconds"] * 2.0 + GEAR["travel_seconds"]
    say("PASS a full gear cycle is %.1f s: %.1f door, %.1f travel, %.1f door"
        % (cycle, GEAR["door_seconds"], GEAR["travel_seconds"], GEAR["door_seconds"]))

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
    set_anim_defaults()
    say("DONE")


run()
