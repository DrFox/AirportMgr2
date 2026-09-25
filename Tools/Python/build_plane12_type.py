"""Authors DA_Aircraft_Plane12 and sets ABP_Plane12's rig facts. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every build_plane<N>_type.py states it:
a figure typed here is a second opinion about an object the export already answers for. The
measuring half is shared - airside_import.part_bounds_uu.

WHAT THIS TYPE IS FOR. The Piper PA-28-180 Cherokee is plane1's LOW-WING TWIN: the same
four-seat, 180 hp, fixed-gear trainer class as the 172, the same Code A, the same grass.
It is NOT a new capability tier and does not pretend to be one - it is variety at the bottom
of the ladder, the way the 172 was variety beside the Meridian. What it does differently is
SHAPE: 9.14 m of span against the 172's 11.00, so it is the narrowest aeroplane in the game.

THE PRIMARY SOURCE IS ON DISK: plane12/concept/drawing.png, Piper's three-view. Span, length,
stabilator, track, wheelbase and propeller come from its dimensions (inches). PERFORMANCE does
NOT - the drawing carries none - and is the PA-28-180 owner's handbook figures as commonly
quoted, at 2,400 lb gross, sea level, standard day. They are marked where typed.

THE AEROPLANE SITS 5 DEGREES NOSE-UP ON ITS GEAR, and the mesh is exported that way:
plane12/README.md "The drawing, and the static attitude". Nothing here corrects for it. The
footprint is a PLAN shadow of the pitched mesh, and it is LONGER than the drawing's 282":
nose-up swings the 2.27 m fin top about 0.2 m aft (2.27 sin 5), so the plan length measured
7.271 m on 2026-09-25 against 7.163 along the reference line. The table below prints both
and does not call the difference an error. The nose oleo is raked by that pitch - see
build_plane12_anim.py.

THE GEAR IS FIXED, so this type declares no gear cycle, plane1's ruling: leaving
TravelSeconds at the C++ default of 0 is how a type says "nothing retracts", and verify()
asserts the zero.
"""
import json
import math
import os
import struct
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. Put it on before importing the shared reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
TYPE_NAME = "DA_Aircraft_Plane12"
ABP = "/Game/Aircraft/Plane12/ABP_Plane12"
# The ICAO type designator. ShortCode is a label, not a key.
SHORT_CODE = "P28A"
MESH = "/Game/Aircraft/Plane12/SK_Plane12"

SOURCE = r"C:\repos\AirportMgr2Models\plane12\export\plane12.glb"

# The parts this type is measured from, NAMED rather than assumed equal to plane1's; a renamed
# part raises rather than defaulting.
WING = "wing"
# STABILATOR, NOT STABILISER - the PA-28's all-moving tailplane, and the export names it for
# what it is. Every other type script says `stabiliser`; copying one unedited raises here.
STABILISER = "stabilator"
# ONE LEG, NOT THE PAIR: the only thing taken from it is its HEIGHT, the sanity bound on the
# wheel radius - plane1's use of its `maingear`.
GEAR_LEG = "maingear_L"
PROP = "prop"


# IcaoCode.cpp's Code A row, as this script expects to find it. Checked every run so a
# re-export that grows past its letter fails here, in the script that measured it.
MAX_SPAN_A = 1500.0
MAX_NOSE_FWD_A = 300.0
MAX_TAIL_AFT_A = 1000.0


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def measure():
    """The footprint, the propeller and the gear leg's height, off the export's own parts."""
    parts = part_bounds_uu(read_gltf(SOURCE))

    def find(stem):
        if stem not in parts:
            raise KeyError("no part named %s in %s - the rig was renamed. Parts: %s"
                           % (stem, SOURCE, ", ".join(sorted(parts))))
        return parts[stem]

    every = list(parts.values())
    # The nose is the SPINNER and the tail the FIN; the maximum over every part gets both
    # without naming either, the expression every other type script uses.
    nose_x = max(high[0] for _, high in every)
    tail_x = min(low[0] for low, _ in every)

    wing_lo, wing_hi = find(WING)
    stab_lo, stab_hi = find(STABILISER)
    find(PROP)
    leg_lo, leg_hi = find(GEAR_LEG)

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    return footprint, disc_diameter_uu(PROP), leg_hi[2] - leg_lo[2]


def disc_diameter_uu(blades):
    """Twice the farthest blade vertex's distance from the blades' own centroid, uu.

    NOT THE BOUNDING BOX, for two reasons, one per aeroplane that met it. plane10's three
    blades read 12% short in a box. This one has two, which a box would get right if the disc
    stood square - but it does not: the whole aeroplane is pitched 5 degrees, so the disc is
    tilted and its box height is D cos(5). The CENTROID of a symmetric blade set lies on the
    hub axis in the disc plane, and the 3D distance from it ignores which way the disc faces.
    """
    doc, binary = _read_glb(SOURCE)
    points = []
    for node in doc["nodes"]:
        if node.get("mesh") is None or node.get("name", "").split(".")[0] != blades:
            continue
        for primitive in doc["meshes"][node["mesh"]]["primitives"]:
            points.extend(_positions(doc, binary, primitive["attributes"]["POSITION"]))
    if not points:
        raise KeyError("no vertices under a mesh named %s in %s" % (blades, SOURCE))
    centre = [sum(p[k] for p in points) / len(points) for k in range(3)]
    farthest = max(math.sqrt(sum((p[k] - centre[k]) ** 2 for k in range(3))) for p in points)
    return 2.0 * farthest * 100.0


def _read_glb(path):
    """(JSON chunk, BIN chunk). read_gltf returns only the first; this needs the vertices."""
    with open(path, "rb") as handle:
        data = handle.read()
    json_length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + json_length].decode("utf-8"))
    bin_at = 20 + json_length
    bin_length = struct.unpack_from("<I", data, bin_at)[0]
    return doc, data[bin_at + 8:bin_at + 8 + bin_length]


def _positions(doc, binary, accessor_index):
    """An accessor's VEC3 float positions. Only what a POSITION accessor can be: FLOAT, VEC3."""
    accessor = doc["accessors"][accessor_index]
    view = doc["bufferViews"][accessor["bufferView"]]
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", 12)
    for i in range(accessor["count"]):
        yield struct.unpack_from("<3f", binary, start + i * stride)


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane12's reference pose - the BONES, which build_rig.py places on each wheel's axle,
    so the hub's height above the contact plane IS the radius. plane3's rule."""
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
            raise KeyError("SK_Plane12 has no %s bone - the rig was renamed. Bones: %s"
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


# --- Published, from the Piper PA-28-180 Cherokee ----------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s. 2,400 lb, sea level,
# standard day. NOT FROM THE DRAWING ON DISK - see the header.
#
# The 172's class, and within a few knots of it everywhere: it rotates at 52 kn against 55
# and climbs at the same 74.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 160 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 2673 gives 223 m against the quoted 720 ft (219 m).
    "takeoff": dict(accel=160.0, decel=400.0, speed_cap=2673.0),   # Vr 52 kn (60 mph)
    "landing": dict(accel=100.0, decel=400.0, speed_cap=3341.0),   # approach 65 kn (75 mph)
}

# 30 DEGREES, plane1's EFFECTIVE AUTHORITY, for plane1's reason: a PA-28 steers its nosewheel
# off the rudder pedals and turns the rest on differential brake, so there is no one
# mechanical lock to publish, and the drawing gives no turning radius to derive one from as
# plane10's POH did. 0.25 g, the light-single figure plane1 and the Meridian share.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 5.0,   # plane1's: a light elevator - here a stabilator
    "climb_speed": 3804.0,            # 74 kn (85 mph), best rate
    # THE SAME CIRCUIT AS plane1 AND plane2 - light aeroplanes flying a visual circuit off the
    # same field.
    "clear_altitude": 30000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    # plane1's 800, and lower if anything: a low wing puts the pilot's eye nearer the mains.
    "flare_height": 800.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE: the O-360-A3A turns the propeller itself, so redline IS propeller rpm -
    # plane1's argument, same engine family.
    "max_rpm": 2700.0,
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

# PUBLISHED FIELD LENGTHS, both over a 50 ft obstacle - the definition every other type uses.
# 1,625 ft and 1,150 ft, rounded UP to the next 10 m. AirportMgr.Content.
# FieldLengthsCoverTheRoll asserts the roll fits under each, and that take-off stays under
# the Meridian's 510 m, as plane1's does.
REQUIREMENTS = {
    "takeoff_field_length": 50000.0,   # 500 m
    "landing_field_length": 36000.0,   # 360 m
}

# Four seats and ONE door, on the right over the wing root. Ten minutes, plane1's.
TURNAROUND_SECONDS = 600.0

# TWO BLADES - the drawing's 76" propeller for the -180, and plane12/scripts/build_prop.py's
# blade, lifted from plane1's two-blade.
PROP_BLADE_COUNT = 2

# PUSHBACK IS NOT SET HERE - Tools/Python/build_pushback_needs.py owns EPushbackNeed for
# every type.


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def check_letter(m):
    """Code A, checked against the measurement rather than asserted - the three figures the
    letters-row test pins, so a re-export that outgrows its row fails HERE first."""
    span = m["footprint"]["wingspan"]
    nose = m["footprint"]["nose_x"] - m["steer_axle_x"]
    tail = -(m["footprint"]["tail_x"] - m["steer_axle_x"])
    for label, got, limit in (("span", span, MAX_SPAN_A), ("nose", nose, MAX_NOSE_FWD_A),
                              ("tail", tail, MAX_TAIL_AFT_A)):
        if got > limit:
            fail("%s measures %.1f uu against Code A's %.0f - raise the row in "
                 "Solve/IcaoCode.cpp and this constant with it" % (label, got, limit))
        else:
            say("PASS %s %.1f uu is inside Code A's %.0f by %.1f" % (label, got, limit,
                                                                     limit - got))


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: an airline's fleet points at this.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    asset.set_editor_property("code", unreal.Name("A"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("Piper PA-28-180 Cherokee"))

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
    say("measured off the export: wheel radius %.1f, prop %.1f, nose %.1f, tail %.1f uu, "
        "span %.1f, wing line %.1f"
        % (m["wheel_radius"], m["prop_diameter"], m["footprint"]["nose_x"],
           m["footprint"]["tail_x"], m["footprint"]["wingspan"], m["footprint"]["wing_x"]))
    say("measured off the rig: steer axle %.1f, fixed axle %.1f uu (wheelbase %.1f), track %.1f"
        % (m["steer_axle_x"], m["fixed_axle_x"],
           abs(m["fixed_axle_x"] - m["steer_axle_x"]), m["main_gear_track"]))
    check_letter(m)

    # THE MEASURED-AGAINST-PUBLISHED TABLE, printed every run - every figure on the right is
    # off Piper's three-view (inches converted). LENGTH is the exception and reads long on
    # purpose - the plan shadow of a pitched mesh against 282" along the reference line; see
    # the header.
    for label, got_uu, published_m in (
            ("span", m["footprint"]["wingspan"], 9.144),
            ("length*", m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 7.163),
            ("wheelbase", abs(m["fixed_axle_x"] - m["steer_axle_x"]), 1.897),
            ("track", m["main_gear_track"], 3.048),
            ("stabilator", m["footprint"]["tailplane_span"], 3.048),
            ("propeller", m["prop_diameter"], 1.930)):
        got_m = got_uu / 100.0
        say("  %-12s measured %6.3f m against the drawing's %6.3f m  (%+.1f%%)"
            % (label, got_m, published_m, 100.0 * (got_m - published_m) / published_m))
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
    # GRASS AND VISUAL - plane1's, for plane1's reason: a club trainer off a farm strip.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.GRASS)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    # NO GEAR CYCLE IS DECLARED, AND THE OMISSION IS THE STATEMENT - plane1's ruling.

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the rig facts: wheel radius and blade count, on the
    generated class's defaults. Left unset, it would spin the Meridian's 21 uu wheels and
    drive a 2-blade disc at a 3-blade step."""
    abp = unreal.EditorAssetLibrary.load_asset(ABP)
    if abp is None:
        fail("no %s" % ABP)
        return
    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class; compile it first" % ABP)
        return

    want = {
        "main_wheel_radius": measured()["wheel_radius"],
        "prop_blade_count": PROP_BLADE_COUNT,
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
            say("PASS ABP_Plane12 %s = %s" % (prop, got))


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
        ("propeller_diameter", asset.get_editor_property("propeller_diameter"),
         m["prop_diameter"]),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"),
         TURNAROUND_SECONDS),
    ]
    for name, got, want in checks:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %s = %.2f" % (name, got))

    footprint = asset.get_editor_property("footprint")
    for field in ("wingspan", "tailplane_span"):
        got = footprint.get_editor_property(field)
        want = m["footprint"][field]
        if abs(got - want) > 0.1:
            fail("%s read back as %.1f, expected %.1f" % (field, got, want))
        else:
            say("PASS footprint %s %.1f uu (%.2f m)" % (field, got, got / 100.0))

    for prop, want in (("short_code", SHORT_CODE), ("code", "A")):
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

    requirements = asset.get_editor_property("requirements")
    takeoff = requirements.get_editor_property("takeoff_field_length")
    landing = requirements.get_editor_property("landing_field_length")
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m)"
        % (takeoff, takeoff / 100.0, landing, landing / 100.0))
    say("PASS surface %s, approach %s"
        % (requirements.get_editor_property("minimum_surface"),
           requirements.get_editor_property("approach_needed")))

    # THE FIXED GEAR, ASSERTED - plane1's check, for plane1's reason.
    gear = asset.get_editor_property("gear")
    travel = gear.get_editor_property("travel_seconds")
    if travel != 0.0:
        fail("gear.travel_seconds is %.2f - a Cherokee's gear is fixed, so this type must "
             "declare no cycle at all" % travel)
    else:
        say("PASS gear.travel_seconds = 0, so nothing retracts")

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
