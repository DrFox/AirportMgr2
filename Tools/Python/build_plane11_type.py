"""Authors DA_Aircraft_Plane11 and sets ABP_Plane11's per-model defaults. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every build_plane<N>_type.py states it:
a figure typed here is a second opinion about an object the export already answers for. The
measuring half is shared - airside_import.part_bounds_uu.

WHAT THIS TYPE IS FOR. plane6's 777-300ER made Code E a rung; the A350-1000 is the other half
of it - plane9's argument one letter up: same letter, same stands, different aeroplanes. It is
lighter (319 t against 351) and asks less runway (2,750 m against 3,120), so it is the Code E
type a field can take BEFORE it can take the 777. AirportMgr.Content.FieldLengthsCoverTheRoll
pins that as this row's ceiling.

THE SOURCE DRAWING IS ON DISK: plane11/concept/A350-1000.dwg (Airbus's own 3-view), and
plane11/SPEC.md tabulates the model against it: 73.65 m long, 64.69 m span, 17.12 m high
against the published 73.79 / 64.75 / 17.08.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, unlike plane4's. No Airbus
A350 Aircraft Characteristics PDF is in plane11/concept; the field lengths, speeds, tiller
lock and fan speed below are commonly quoted figures, rounded in the conservative direction.
Replace them from the AC document when it is to hand.
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
TYPE_NAME = "DA_Aircraft_Plane11"
ABP = "/Game/Aircraft/Plane11/ABP_Plane11"
# The ICAO type designator. ShortCode is a label, not a key.
SHORT_CODE = "A35K"
MESH = "/Game/Aircraft/Plane11/SK_Plane11"

MODELS = r"C:\repos\AirportMgr2Models\plane11"
SOURCE = os.path.join(MODELS, "export", "plane11.glb")
RIG_MAP = os.path.join(MODELS, "scripts", "rig_map.json")

# The parts this type is measured from, NAMED rather than assumed; a renamed part raises
# rather than defaulting. The curved winglets are part of the wing object, so the span is
# over them - the figure that decides the letter.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear_L"
# The MESH is honestly a fan; its BONE is prop_L so BONE_RULES' "prop" needle finds a driver.
PROP = "fan_L"

# THE SIX MAIN WHEEL BONES, AVERAGED - plane6's reason: the bogie's CENTRE is the point a
# three-axle truck pivots about, which is what FChassis::FixedAxleX wants. Naming the middle
# axle would be right today and silently wrong the day the bogie is re-spaced.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2", "wheel_L3")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2", "wheel_R3")

# TWENTY-TWO BLADES - the Trent XWB's, and plane11/scripts/build_fan.py arrays that many.
# UAirsideAgentAnim::PropStepDegrees resolves the frame rate against ONE BLADE REPEAT, 360/N,
# so this must describe the fan on screen; the class default of 3 would alias backwards.
PROP_BLADE_COUNT = 22

def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def rig_angles():
    """The per-type angles, from the file the rig proved them in - plane11/scripts/
    build_rig.py writes them after posing the solved fold. Read, not typed, so the ABP's
    angle and the model's cannot drift."""
    with open(RIG_MAP) as handle:
        angles = json.load(handle)["angles"]
    return {
        # 90, THE CLASS DEFAULT, AND WRITTEN ANYWAY - plane6's argument for its door angle: a
        # default that HAPPENS to be right stops being right the day somebody retunes it.
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
    SK_Plane11's reference pose - the BONES, which build_rig.py places on each wheel's
    rotation axis, so the hub's height above the contact plane IS the radius. plane3's rule,
    with plane6's six-wheel averaging and the three checks that make averaging safe.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if mesh is None:
        raise KeyError("no %s - run import_models.py first" % MESH)

    pose = mesh.get_editor_property("skeleton").get_reference_pose()
    at = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        bone = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        at[str(name)] = bone.translation

    wanted = ("nosewheel",) + MAIN_WHEELS_L + MAIN_WHEELS_R
    missing = [name for name in wanted if name not in at]
    if missing:
        raise KeyError("SK_Plane11 has no %s bone(s) - the rig was renamed. Bones: %s"
                       % (", ".join(missing), ", ".join(sorted(at))))

    left = [at[name] for name in MAIN_WHEELS_L]
    right = [at[name] for name in MAIN_WHEELS_R]
    both = left + right

    def mean(values):
        return sum(values) / float(len(values))

    mains_x = mean([v.x for v in both])
    radius = mean([v.z for v in both])
    track = abs(mean([v.y for v in left]) - mean([v.y for v in right]))

    # A BOGIE IS THREE AXLES, SO ITS BONES MUST BE AT THREE STATIONS - three bones stacked on
    # one x average to a right-looking wheelbase and roll three tyres through each other.
    stations = sorted(v.x for v in left)
    if stations[-1] - stations[0] < radius:
        raise ValueError("the three left main wheel bones span %.1f uu, less than one "
                         "wheel's %.1f uu radius - stacked on one station, not three axles"
                         % (stations[-1] - stations[0], radius))

    # AND AT ONE HEIGHT, because they share a beam; a wheel off it is off the ground, and the
    # mean radius would average the error away.
    for name in MAIN_WHEELS_L + MAIN_WHEELS_R:
        if abs(at[name].z - radius) > 1.0:
            raise ValueError("%s's hub is at z=%.1f against the bogies' mean %.1f - the six "
                             "main wheels are not on one plane" % (name, at[name].z, radius))

    if track <= radius:
        raise ValueError("the two bogies are %.1f uu apart, inside one wheel's %.1f uu "
                         "radius - the main gear bones are on top of each other"
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


# --- Published (approximately - see the header) for the A350-1000, Trent XWB-97 -----------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
GROUND = {
    # THE FLEET'S TAXI FIGURES, UNCHANGED - a taxi speed is a procedure, not a capability.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 230, A SHADE ABOVE THE 777's 220: 32 t lighter on similar thrust and a Vr about 15 kn
    # lower. Still below the narrowbodies' 250.
    "takeoff": dict(accel=230.0, decel=400.0, speed_cap=7710.0),   # Vr about 150 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7453.0),   # Vref about 145 kn
}

# 70 DEGREES of tiller authority, the 777's figure. On the measured ~32.5 m wheelbase that is
# about a 35 m tightest radius, the largest in the game beside the 777's; author_type() logs
# it. 0.12 g, the 777's: a 300 t aeroplane on bogies is not cornered briskly.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 118.0,
}

CLIMB = {
    # 9 DEGREES, NOT THE 777-300ER's 8. As long an aeroplane, but not the type famous for
    # tail strikes; plane4's and plane9's rotation.
    "lift_angle_at_rotate_degrees": 9.0,
    "climb_pitch_degrees": 11.0,
    # 2.5 a second: between the -300ER's deliberate 2 and the narrowbodies' 3.
    "rotate_rate_deg_per_sec": 2.5,
    "climb_speed": 12850.0,       # 250 kn below 10,000 ft
    # 650 m, plane6's, for plane6's reason: a 16 s gear cycle on this climb finishes above
    # 500 m. author_type() asserts the relation rather than trusting this comment.
    "clear_altitude": 65000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # THE REAL FAN SPEED, about 2,700 rpm at 100 % N1 - a big fan turns slowly, as the GE90's
    # 2,355 does. The VIEW caps what it draws (UAirsideAgentAnim::PropDisplayCapRPM).
    "max_rpm": 2700.0,
    # The 777's figures: the same class of fan.
    "spool_up_seconds": 10.0,
    "spool_down_seconds": 30.0,
}

# THE GEAR. plane6's cycle for plane6's reason: three-axle bogies into wells between the wing
# box and the belly fairing, behind big doors.
GEAR = {
    "travel_seconds": 12.0,
    "door_seconds": 2.0,
    # ZERO: plane11/SPEC.md - "No bogie bone (the bogie stows untilted)". A non-zero figure
    # would animate nothing and claim a part the mesh does not have. plane6's ruling.
    "truck_tilt_seconds": 0.0,
    # About 295 ft - raising the gear is a pilot command, not a consequence of lift-off.
    "retract_above_height": 9000.0,
    # Above FinalAltitude on purpose: every arrival is born down and locked. plane4's note.
    "extend_below_height": 15000.0,
}

# FIELD LENGTHS at MTOW, sea level, ISA, rounded UP to the nearest 50 m. AirportMgr.Content.
# FieldLengthsCoverTheRoll asserts the model's roll fits under each, and that take-off stays
# under plane6's 3,120 - the A350 is the Code E type a shorter runway admits.
# THE TAKE-OFF FIGURE IS ALSO plane13's CEILING, typed into that test's row: the 757-300 must
# ask less than this, so raising it loosens the Code D rung and lowering it may break it.
REQUIREMENTS = {
    "takeoff_field_length": 275000.0,   # 2,750 m
    "landing_field_length": 210000.0,   # 2,100 m at max landing weight
}

# NINETY MINUTES, plane6's: ~370 seats through four doors, plus a long-haul turn's catering
# and fuel uplift. A widebody occupies its stand long enough to be felt.
TURNAROUND_SECONDS = 5400.0

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

    asset.set_editor_property("code", unreal.Name("E"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("A350-1000"))

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
    # TARMAC AND VISUAL, plane6's: the step from the 777 is not a different surface.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults:
    wheel radius, blade count, and the two angles from rig_map.json. The blade count is the
    one that bites if left: the class default is 3 against this fan's 22."""
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
            say("PASS ABP_Plane11 %s = %s" % (prop, got))


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

    for prop, want in (("short_code", SHORT_CODE), ("code", "E")):
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
