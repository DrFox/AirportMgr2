"""Authors DA_Aircraft_Plane14 and sets ABP_Plane14's per-model defaults. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every build_plane<N>_type.py states it:
a figure typed here is a second opinion about an object the export already answers for. The
measuring half is shared - airside_import.part_bounds_uu.

WHAT THIS TYPE IS FOR. The first BUSINESS JET, and the first jet under Code C. Every jet
before it needed a Code C stand and 2 km of runway; the Phenom 300 is a Code B aeroplane -
15.91 m of wing, 0.91 m over Code A's 15 m - that asks about 1,000 m of TARMAC. So it is the
jet a small paved field can take, beside the King Air it shares a letter with.

THE SOURCE DRAWINGS ARE ON DISK, but they are MARKETING drawings, not a manufacturer's
dimensioned 3-view: plane14/concept/drawing-*.png, and plane14/SPEC.md tabulates the model
against them - span 15.910 (15.91), length 15.635 (15.64), height 4.896 against the published
5.10, where the front GA and Embraer's figure disagree and the model follows the GA.

THE PERFORMANCE FIGURES ARE NOT FROM A PRIMARY SOURCE ON DISK, as plane11's and plane13's are
not. No Embraer AFM or Airport Planning Manual is in plane14/concept; the field lengths,
speeds, tiller lock and fan speed below are commonly quoted figures, rounded in the
conservative direction. Replace them from the Phenom 300 APM when it is to hand.
"""
import re
import math
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see
# import_models.py for the full note. Put it on before importing the shared reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane14"
ABP = "/Game/Aircraft/Plane14/ABP_Plane14"
# The ICAO type designator. ShortCode is a label, not a key.
SHORT_CODE = "E55P"
MESH = "/Game/Aircraft/Plane14/SK_Plane14"

MODELS = r"C:\repos\AirportMgr2Models\plane14"
SOURCE = os.path.join(MODELS, "export", "plane14.glb")
# NO rig_map.json FOR THIS MODEL - plane14's pipeline is plane12's, which writes none. The
# angles are constants in build_rig.py, which poses the fold with them; rig_angles() reads that
# line rather than retyping it, so the ABP and the model still cannot drift.
RIG_SCRIPT = os.path.join(MODELS, "scripts", "build_rig.py")

# The parts this type is measured from, NAMED rather than assumed; a renamed part raises
# rather than defaulting. The winglets are part of the wing object (SPEC.md: the bend start is
# solved so the outermost vertex is the published half-span), so the span is over them - the
# figure that decides the letter.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear_L"
# The MESH is honestly a fan; its BONE is prop_L so BONE_RULES' "prop" needle finds a driver.
PROP = "fan_L"

# TWENTY BLADES, plane14/scripts/build_engine.py's N_BLADES. UAirsideAgentAnim::PropStepDegrees
# resolves the frame rate against ONE VISIBLE BLADE REPEAT, 360/N, so this must describe the
# mesh on screen; the class default of 3 would alias backwards.
PROP_BLADE_COUNT = 20

# IcaoCode.cpp's Code B row, as this script expects to find it. Checked every run so a
# re-export that grows past its letter fails here, in the script that measured it.
MAX_TAIL_AFT_B = 2000.0
MAX_NOSE_FWD_B = 400.0
MAX_SPAN_B = 2400.0
# AND THE FLOOR - plane13's check, for plane10's reason: the span is 1591, 91 uu over Code A's
# 1500, so a re-export that trimmed the winglets would quietly make this a Code A type.
MIN_SPAN_B = 1500.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def rig_angles():
    """The per-type angles, from the file the rig proved them in - plane14/scripts/
    build_rig.py poses the fold with its GEAR_DEG, DOOR_DEG line and asserts every part is
    inside the hull when folded. Read, not typed, so the ABP's angle and the model's cannot
    drift; a reworded line raises rather than defaulting."""
    with open(RIG_SCRIPT) as handle:
        found = re.search(r"^GEAR_DEG, DOOR_DEG = ([0-9.]+), ([0-9.]+)", handle.read(), re.M)
    if found is None:
        raise ValueError("no 'GEAR_DEG, DOOR_DEG = ...' line in %s - the rig script was "
                         "reworded; read its angles by hand and update this parser"
                         % RIG_SCRIPT)
    return {
        # 90, THE CLASS DEFAULT, AND WRITTEN ANYWAY - plane6's argument: a default that
        # HAPPENS to be right stops being right the day somebody retunes it. One figure for
        # all three legs (SPEC.md), so wire_plane14_anim.py needs no per-leg ratio.
        "gear_retracted": float(found.group(1)),
        # 90 closes the clamshell nose doors from their OPEN bind pose. The mains have no
        # door bones: their leg doors are meshes that ride gear_L/R.
        "bay_door_closed": float(found.group(2)),
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
    SK_Plane14's reference pose - the BONES, which build_export.py places on each wheel's
    rotation axis, so the hub's height above the contact plane IS the radius. plane4's rule;
    one wheel a side, plane9's shape.
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
            raise KeyError("SK_Plane14 has no %s bone - the rig was renamed. Bones: %s"
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


# --- Published (approximately - see the header) for the Phenom 300, PW535E -----------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
GROUND = {
    # THE FLEET'S TAXI FIGURES, UNCHANGED - a taxi speed is a procedure, not a capability.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 250, THE NARROWBODIES': a light jet's thrust-to-weight is an airliner's, not a
    # turboprop's 350 - it is the lower Vr that makes the roll short.
    "takeoff": dict(accel=250.0, decel=400.0, speed_cap=5397.0),   # Vr about 105 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=5911.0),   # Vref about 115 kn
}

# 60 DEGREES of tiller authority, a round figure for a light jet's nosewheel. On the measured
# ~5.9 m wheelbase that is about a 7 m tightest radius; author_type() logs it. 0.2 g, a light
# aeroplane cornered like one.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 10.0,
    "climb_pitch_degrees": 12.0,
    "rotate_rate_deg_per_sec": 3.0,
    "climb_speed": 10280.0,       # 200 kn
    # 400 m, plane5's: an 8 s gear cycle on this climb finishes near 260 m.
    # author_type() asserts the relation rather than trusting this comment.
    "clear_altitude": 40000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # ABOUT 14,000 rpm AT 100 % N1 - a 0.6 m fan turns fast; the figure is a tip speed of
    # about 450 m/s over the modelled disc, not a quoted one. The VIEW caps what it draws
    # (UAirsideAgentAnim::PropDisplayCapRPM), so the aliasing it would cause never reaches
    # the screen.
    "max_rpm": 14000.0,
    # A small fan spools faster than the airliners' 8/25.
    "spool_up_seconds": 6.0,
    "spool_down_seconds": 20.0,
}

# THE GEAR. Trailing-link mains fold inboard into the belly fairing and the nose folds forward
# (SPEC.md; the trunnions are INVENTED). 6 s of travel, a light jet's; only the nose has door
# bones, so the door stage is seen at the nose alone.
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    # About 295 ft - raising the gear is a pilot command, not a consequence of lift-off.
    "retract_above_height": 9000.0,
    # Above FinalAltitude on purpose: every arrival is born down and locked. plane4's note.
    "extend_below_height": 15000.0,
}

# FIELD LENGTHS at MTOW, sea level, ISA, rounded UP to the nearest 50 m. The commonly quoted
# figures are about 980 m to take off and 675 m to land. AirportMgr.Content.
# FieldLengthsCoverTheRoll asserts the model's roll fits under each.
REQUIREMENTS = {
    "takeoff_field_length": 100000.0,   # 1,000 m
    "landing_field_length": 70000.0,    # 700 m
}

# FIFTEEN MINUTES: eight seats through one airstair door, and a bizjet's turn is a fuel and a
# lavatory service, not catering for a cabin. plane5's twelve plus a jet's fuel uplift.
TURNAROUND_SECONDS = 900.0

# PUSHBACK IS NOT SET HERE - Tools/Python/build_pushback_needs.py owns EPushbackNeed for
# every type.


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def check_letter(m):
    """Code B, checked against the measurement rather than asserted - the three figures the
    letters-row test pins, plus the floor, so a re-export that leaves its row fails HERE first."""
    span = m["footprint"]["wingspan"]
    nose = m["footprint"]["nose_x"] - m["steer_axle_x"]
    tail = -(m["footprint"]["tail_x"] - m["steer_axle_x"])
    for label, got, limit in (("span", span, MAX_SPAN_B), ("nose", nose, MAX_NOSE_FWD_B),
                              ("tail", tail, MAX_TAIL_AFT_B)):
        if got > limit:
            fail("%s measures %.1f uu against Code B's %.0f - raise the row in "
                 "Solve/IcaoCode.cpp and this constant with it" % (label, got, limit))
        else:
            say("PASS %s %.1f uu is inside Code B's %.0f by %.1f" % (label, got, limit,
                                                                     limit - got))
    if span <= MIN_SPAN_B:
        fail("span measures %.1f uu, inside Code A's %.0f - this is no longer a Code B type"
             % (span, MIN_SPAN_B))
    else:
        say("PASS span %.1f uu is over Code A's %.0f by %.1f, so the letter is B"
            % (span, MIN_SPAN_B, span - MIN_SPAN_B))


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

    asset.set_editor_property("code", unreal.Name("B"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("Phenom 300"))

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
    check_letter(m)
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
    # TARMAC AND VISUAL. Tarmac is the point: the King Air beside it on Code B demands it
    # too, and no jet here is flown off grass.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults:
    wheel radius, blade count, and the two angles from build_rig.py. The blade count is the
    one that bites if left: the class default is 3 against this fan's 20."""
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
            say("PASS ABP_Plane14 %s = %s" % (prop, got))


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

    for prop, want in (("short_code", SHORT_CODE), ("code", "B")):
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
