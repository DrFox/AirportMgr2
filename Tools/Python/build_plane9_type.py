"""Authors DA_Aircraft_Plane9 and sets ABP_Plane9's per-model defaults. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every build_plane<N>_type.py states it:
a figure typed here is a second opinion about an object the export already answers for. The
measuring half is shared - airside_import.part_bounds_uu.

WHAT THIS TYPE IS FOR. plane4's 737-800 made "the JET" a rung; the A320 is the other half of
Code C, and the pair is the point: same letter, same stands, different aeroplanes. It needs
LESS runway than the 737 (2,100 m against 2,316) and noses 1 m further past its gear (5.08 m
against 4.09), which is exactly the pair of facts IcaoCode's Code C row already takes from
two different aeroplanes - MaxTailAft from the 737, MaxNoseFwd from the A320.

WHY THIS TYPE AND NOT DA_Aircraft_A320, which already exists - plane4's reason for not
reusing DA_Aircraft_B738. That one is the PAPER A320: UAircraftType::BuildA320 types its
figures from the datasheet, it carries no mesh, and it is the design aircraft of
DA_Stand_CodeC, with the service points the stand lays its roads to. This type is the
aeroplane that flies, and its figures are MEASURED off SK_Plane9. The two agree where both
state a figure: nose 508 against 507, tail -3249 against -3250.

THE SOURCE DRAWING IS ON DISK: plane9/concept/A320_sharklet-001.dwg (Airbus's own 3-view),
and plane9/SPEC.md tabulates the model against it. The one figure that differs from the
brochure is the SPAN, 35.49 m against 35.80, and that is the drawing's, left unscaled by
ruling; it is Code C either way.
"""
import json
import math
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see
# import_models.py for the full note. Put it on before importing the shared reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane9"
ABP = "/Game/Aircraft/Plane9/ABP_Plane9"
# The ICAO type designator - the paper type's too. ShortCode is a label, not a key.
SHORT_CODE = "A320"
MESH = "/Game/Aircraft/Plane9/SK_Plane9"

MODELS = r"C:\repos\AirportMgr2Models\plane9"
SOURCE = os.path.join(MODELS, "export", "plane9.glb")
RIG_MAP = os.path.join(MODELS, "scripts", "rig_map.json")

# The parts this type is measured from, NAMED rather than assumed equal to plane4's; a
# renamed part raises rather than defaulting. The sharklets are part of the wing object, so
# the span is over the sharklets - the figure that decides the letter.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear_L"
# The MESH is honestly a fan; its BONE is prop_L so BONE_RULES' "prop" needle finds a driver.
PROP = "fan_L"

# TWENTY-FOUR BLADES, THE MODEL'S, NOT THE ENGINE'S 36. plane9/scripts/build_fan.py arrays
# 24 and says why (at game distance the count does not read, the twist does). This figure is
# what UAirsideAgentAnim::PropStepDegrees resolves the frame rate against - ONE VISIBLE BLADE
# REPEAT, 360/N - so it must describe the mesh on screen. 36 against 24 drawn blades would
# allow a step too large by half and alias.
PROP_BLADE_COUNT = 24

def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def rig_angles():
    """The per-type angles, from the file the rig proved them in - plane9/scripts/
    build_rig.py writes them after posing the solved fold. Read, not typed, so the ABP's
    angle and the model's cannot drift."""
    with open(RIG_MAP) as handle:
        angles = json.load(handle)["angles"]
    return {
        # 86, NOT THE CLASS DEFAULT 90: the main leg's inboard end rises through the 0.32 m
        # wing root past 86. The nose folds 86 too (gear_pivots.json), so one figure serves
        # all three legs and wire_plane9_anim.py needs no per-leg ratio.
        "gear_retracted": angles["GearRetractedAngleDegrees"],
        # 81 closes the doors exactly from their OPEN bind pose.
        "bay_door_closed": angles["BayDoorClosedAngleDegrees"],
    }


def measure():
    """The footprint, the fan and the gear leg's height, off the export's own parts."""
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
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The fan's widest sweep is its disc, across the airframe - never the chordwise extent.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane9's reference pose - the BONES, which build_export.py places on each wheel's
    rotation axis, so the hub's height above the contact plane IS the radius. plane4's rule.
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
            raise KeyError("SK_Plane9 has no %s bone - the rig was renamed. Bones: %s"
                           % (wanted, ", ".join(sorted(at))))

    mains_x = (at["wheel_L"].x + at["wheel_R"].x) * 0.5
    radius = (at["wheel_L"].z + at["wheel_R"].z) * 0.5
    track = abs(at["wheel_L"].y - at["wheel_R"].y)
    if track <= radius:
        raise ValueError("wheel_L and wheel_R are %.1f uu apart, inside one wheel's %.1f uu "
                         "radius - the two main gear bones are on top of each other"
                         % (track, radius))
    # A radius may not exceed the leg that carries it, and may not be nothing - wheel bones
    # left at the root read 0, a model in metres reads taller than the aeroplane.
    if not 5.0 < radius < leg_height:
        raise ValueError("the main-gear hub sits at z=%.1f against a %.1f uu leg - the bone "
                         "is not on the axle, or the model is not on the ground plane"
                         % (radius, leg_height))

    return at["nosewheel"].x, mains_x, radius, track


_MEASURED = None


def measured():
    """measure() and axles_and_radius(), once - the type, the ABP and the read-back must
    all check the same aeroplane."""
    global _MEASURED
    if _MEASURED is None:
        footprint, prop, leg = measure()
        steer_x, fixed_x, radius, track = axles_and_radius(leg)
        _MEASURED = dict(footprint=footprint, prop_diameter=prop, wheel_radius=radius,
                         steer_axle_x=steer_x, fixed_axle_x=fixed_x, main_gear_track=track)
    return _MEASURED


def tightest_radius_uu():
    """R >= L / sin(lock), off the measured wheelbase."""
    m = measured()
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    return wheelbase / math.sin(math.radians(STEERING["max_steer_degrees"]))


# --- Published, from the Airbus A320-200 (CFM56-5B, sharklets) ----------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# A 78 t narrowbody, the 737-800's peer. It rotates a few knots slower (about 142 against
# 145) and lands slower (about 136 against 140), which is most of why it wants less runway.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as plane4
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=7299.0),   # Vr about 142 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=6990.0),   # Vref about 136 kn
}

# 75 DEGREES of tiller authority - Airbus AC 4-3-0's figure for the A320 family. On the
# measured 12.64 m wheelbase that is about a 13 m tightest radius, tighter than the 737's
# 16 m, and author_type() logs it. 0.15 g as plane4: a 78 t aeroplane is not driven briskly.
STEERING = {
    "max_steer_degrees": 75.0,
    "max_lateral_accel_uu": 147.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 9.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 12850.0,       # 250 kn below 10,000 ft
    "clear_altitude": 50000.0,    # 500 m, plane4's - the gear cycle is checked against it
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # The CFM56-5B's real 100 % N1, about 5,000 rpm. The VIEW caps what it draws - see
    # UAirsideAgentAnim::PropDisplayCapRPM - and PROP_BLADE_COUNT says what it is drawing.
    "max_rpm": 5000.0,
    # A jet spools slowly and winds down more slowly still - plane4's figures, same class.
    "spool_up_seconds": 8.0,
    "spool_down_seconds": 25.0,
}

# The gear cycle. 8 s is the A320's usual quoted transit; a second of bay door either side,
# as plane4. The MAINS HAVE DOORS on this type (plane4's do not), so the door stage is seen
# at both ends of the aeroplane - the same stage, same figure.
GEAR = {
    "travel_seconds": 8.0,
    "door_seconds": 1.0,
    # About 295 ft - raising the gear is a pilot command, not a consequence of lift-off.
    "retract_above_height": 9000.0,
    # Above FinalAltitude on purpose: every arrival is born down and locked. plane4's note.
    "extend_below_height": 15000.0,
}

# PUBLISHED FIELD LENGTHS at MTOW, sea level, ISA - Airbus A320 AC section 3 for the
# CFM56-5B at 78 t, rounded UP to the nearest 100 m. AirportMgr.Content.FieldLengthsCoverTheRoll
# asserts the model's roll fits under each, and that take-off stays under plane4's 2,316.
REQUIREMENTS = {
    "takeoff_field_length": 210000.0,   # 2,100 m
    "landing_field_length": 150000.0,   # 1,500 m
}

# 180 seats through two doors - plane4's forty minutes, for plane4's reason.
TURNAROUND_SECONDS = 2400.0

# PUSHBACK IS NOT SET HERE - Tools/Python/build_pushback_needs.py owns EPushbackNeed for
# every type.


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: an airline's fleet may point at this.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    asset.set_editor_property("code", unreal.Name("C"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("A320-200"))

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
    say("measured off the export: wheel radius %.1f, fan %.1f, nose %.1f, tail %.1f uu, "
        "span %.1f, wing line %.1f"
        % (m["wheel_radius"], m["prop_diameter"], m["footprint"]["nose_x"],
           m["footprint"]["tail_x"], m["footprint"]["wingspan"], m["footprint"]["wing_x"]))
    say("measured off the rig: steer axle %.1f, fixed axle %.1f uu (wheelbase %.1f), track %.1f"
        % (m["steer_axle_x"], m["fixed_axle_x"],
           abs(m["fixed_axle_x"] - m["steer_axle_x"]), m["main_gear_track"]))
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

    # THE GEAR CYCLE MUST FINISH BEFORE THE AGENT IS REMOVED - plane8's check.
    cycle = GEAR["door_seconds"] * 2.0 + GEAR["travel_seconds"]
    rate = CLIMB["climb_speed"] * math.radians(CLIMB["climb_pitch_degrees"])
    top = GEAR["retract_above_height"] + rate * cycle
    if top >= CLIMB["clear_altitude"]:
        fail("the %.0f s gear cycle finishes at about %.0f uu but the agent is cleared at "
             "%.0f - it would vanish mid-retraction." % (cycle, top, CLIMB["clear_altitude"]))
    else:
        say("PASS the %.0f s gear cycle finishes at about %.0f uu, below the %.0f uu the "
            "agent is cleared at" % (cycle, top, CLIMB["clear_altitude"]))

    requirements = asset.get_editor_property("requirements")
    for field, value in REQUIREMENTS.items():
        requirements.set_editor_property(field, value)
    # TARMAC AND VISUAL, plane4's: the step from the 737 is not a different surface.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults:
    wheel radius, blade count, and the two angles from rig_map.json. GearRetracted is 86 -
    NOT the class default - so leaving it unset would fold the legs 4 degrees through the
    wing root."""
    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s" % ABP)
        return
    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class; compile it first" % ABP)
        return

    angles = rig_angles()
    want = {
        "main_wheel_radius": measured()["wheel_radius"],
        "prop_blade_count": PROP_BLADE_COUNT,
        "bay_door_closed_angle_degrees": angles["bay_door_closed"],
        "gear_retracted_angle_degrees": angles["gear_retracted"],
    }
    defaults = unreal.get_default_object(generated)
    for prop, value in want.items():
        defaults.set_editor_property(prop, value)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class())
    for prop, value in want.items():
        got = after.get_editor_property(prop)
        if abs(float(got) - float(value)) > 0.01:
            fail("ABP %s read back as %s, expected %s" % (prop, got, value))
        else:
            say("PASS ABP_Plane9 %s = %s" % (prop, got))


def verify(path):
    """Read back from disk: an asset edit that reports success and writes nothing is this
    project's most familiar failure."""
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("%s did not survive the save" % path)
        return

    m = measured()
    checks = [
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"), m["wheel_radius"]),
        ("fixed_axle_x", asset.get_editor_property("fixed_axle_x"), m["fixed_axle_x"]),
        ("main_gear_track", asset.get_editor_property("main_gear_track"), m["main_gear_track"]),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"), TURNAROUND_SECONDS),
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
            fail("%s is unset - the agent would fall back to the game-wide default mesh" % prop)
        else:
            say("PASS %s = %s" % (prop, got.get_name()))

    gear = asset.get_editor_property("gear")
    for field, want in GEAR.items():
        got = gear.get_editor_property(field)
        if abs(got - want) > 0.01:
            fail("gear.%s read back as %s, expected %s" % (field, got, want))
        else:
            say("PASS gear.%-21s = %.1f" % (field, got))

    requirements = asset.get_editor_property("requirements")
    takeoff = requirements.get_editor_property("takeoff_field_length")
    landing = requirements.get_editor_property("landing_field_length")
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m)"
        % (takeoff, takeoff / 100.0, landing, landing / 100.0))


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
