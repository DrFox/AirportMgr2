"""Authors DA_Aircraft_Plane10 and sets ABP_Plane10's rig facts. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every build_plane<N>_type.py states it:
a figure typed here is a second opinion about an object the export already answers for. The
measuring half is shared - airside_import.part_bounds_uu.

WHAT THIS TYPE IS FOR. It is the fleet's UTILITY SINGLE: a Code B span (15.875 m, over the
15 m line the 172 and the Meridian stay under) on a GRASS strip. Every other Code B type
either wants tarmac (plane5's King Air) or is a twin (plane2's Twin Otter); this one is what
a grass field earns once it is widened for Code B, before it is paved. It asks more runway
than the Meridian (740 m against 510) and less than the King Air (1,006 m), and that ceiling
is what AirportMgr.Content.FieldLengthsCoverTheRoll pins.

THE PRIMARY SOURCE IS ON DISK: plane10/concept/Cessna 208 Diagram and Dimensions-Master.PDF,
POH section 1, figure 1-1 and its notes. Geometry and the turning radius come from there. The
PERFORMANCE figures do NOT - those pages are not in the PDF - and are the 208B's commonly
quoted brochure/POH section 5 figures at MTOW, sea level, ISA. They are marked where typed.

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
TYPE_NAME = "DA_Aircraft_Plane10"
ABP = "/Game/Aircraft/Plane10/ABP_Plane10"
# The ICAO type designator. ShortCode is a label, not a key.
SHORT_CODE = "C208"
MESH = "/Game/Aircraft/Plane10/SK_Plane10"

SOURCE = r"C:\repos\AirportMgr2Models\plane10\export\plane10.glb"

# The parts this type is measured from, NAMED rather than assumed equal to plane1's; a renamed
# part raises rather than defaulting. The span is the WING object's, which on this export
# includes the strobe tips - the POH's 52'-1" does too (figure 1-1 note 2), so the two measure
# the same thing.
WING = "wing"
STABILISER = "stabiliser"
# ONE LEG, NOT THE PAIR: the only thing taken from it is its HEIGHT, the sanity bound on the
# wheel radius - plane1's use of its `maingear`.
GEAR_LEG = "maingear_L"
# The spinner is a separate mesh skinned to the same `prop` bone; the DISC is the blades',
# and the spinner gives the hub axis - see disc_diameter_uu.
PROP = "prop"
SPINNER = "spinner"

# POH FIGURE 1-1, NOTE 6: minimum turning radius, pivot point to outboard wing tip strobe,
# 33'-8" for 208B0404 and on. The earlier airframes' 32'-8 5/8" is not the one taken - a
# current Grand Caravan is the later serial range.
POH_TURN_RADIUS_UU = (33 * 12 + 8) * 2.54


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
    find(SPINNER)
    leg_lo, leg_hi = find(GEAR_LEG)

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    return footprint, disc_diameter_uu(PROP, SPINNER), leg_hi[2] - leg_lo[2]


def disc_diameter_uu(blades, hub):
    """Twice the farthest blade vertex's distance from the hub's axis, uu.

    NOT THE BOUNDING BOX, which every other type script takes, and a 3-BLADE PROP IS WHY. With
    one blade straight up the box is 1.5 r tall and 1.73 r wide, so max(width, height) reads
    2.244 m for this 2.54 m Hartzell - 12% short. An even blade count puts two tips on one
    diameter and the box is right, which is why plane1's two blades and plane5's four never
    showed it. Measured 254.0 uu on 2026-09-25, the POH's 100" to the millimetre. The HUB AXIS
    is the spinner's box centre across the airframe - a surface of revolution, so its box
    centre IS its axis.
    """
    doc, binary = _read_glb(SOURCE)
    hub_lo, hub_hi = part_bounds_uu(doc)[hub]
    axis_y = (hub_lo[1] + hub_hi[1]) * 0.5
    axis_z = (hub_lo[2] + hub_hi[2]) * 0.5
    farthest = 0.0
    for node in doc["nodes"]:
        if node.get("mesh") is None or node.get("name", "").split(".")[0] != blades:
            continue
        for primitive in doc["meshes"][node["mesh"]]["primitives"]:
            for x, y, z in _positions(doc, binary, primitive["attributes"]["POSITION"]):
                # glTF -> UE as part_bounds_uu has it: y is UE Z, z is UE Y, metres to uu.
                farthest = max(farthest, math.hypot(z * 100.0 - axis_y, y * 100.0 - axis_z))
    if farthest <= 0.0:
        raise KeyError("no vertices under a mesh named %s in %s" % (blades, SOURCE))
    return 2.0 * farthest


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
    SK_Plane10's reference pose - the BONES, which build_rig.py places on each wheel's axle,
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
            raise KeyError("SK_Plane10 has no %s bone - the rig was renamed. Bones: %s"
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


def lock_from_poh_radius():
    """The nosewheel angle the POH's turning radius implies on THIS model, degrees.

    The POH measures from the turn's pivot to the OUTBOARD wing tip, so the pivot sits
    (radius - half span) off the centreline, on the main axle line. The nosewheel must then
    point square to the line from that pivot: atan(wheelbase / offset). The pivot marks in
    figure 1-1 sit on the wing, outboard of the main wheel, which is what a braked inner
    main and a castering nosewheel produce.
    """
    m = measured()
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    offset = POH_TURN_RADIUS_UU - m["footprint"]["wingspan"] * 0.5
    return math.degrees(math.atan2(wheelbase, offset))


# --- Published, from the Cessna 208B Grand Caravan ------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s. MTOW (8,750 lb),
# sea level, ISA, flaps 20 for take-off. NOT FROM THE PDF ON DISK - see the header.
#
# A 3,969 kg turboprop single on fixed gear. It rotates at 70 kn, between the 172's 55 and
# the King Air's 100, which is where its field length sits too.
GROUND = {
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 150 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, plane1's method: Vr^2 / 2a
    # with Vr = 3598 gives 432 m against the quoted 1,405 ft (428 m).
    "takeoff": dict(accel=150.0, decel=400.0, speed_cap=3598.0),   # Vr 70 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=4009.0),   # Vref 78 kn, flaps full
}

# 60 DEGREES, DERIVED FROM THE POH RATHER THAN QUOTED. The 208 steers about 15 degrees off
# the pedals and castors further on differential braking, so there is no one mechanical
# figure; what the POH does publish is the resulting turn - 33'-8" pivot to wing tip.
# lock_from_poh_radius() turns that into an angle on the measured model and author_type()
# FAILS if it drifts more than 2 degrees from this figure. 0.2 g as plane2: a utility
# single, not a trainer.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}
LOCK_TOLERANCE_DEGREES = 2.0

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    "rotate_rate_deg_per_sec": 4.0,   # plane2's: a big tail, a heavy single
    "climb_speed": 5346.0,            # 104 kn, Vy at sea level
    # THE SAME CIRCUIT AS plane1 AND plane2 - light aeroplanes flying a visual circuit off the
    # same field. A different height with no rule behind it would be a distinction nobody
    # asked for.
    "clear_altitude": 30000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 900.0,            # plane2's: the eye sits as high over the mains
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # PT6A-114A, PROPELLER rpm behind its reduction gear - the figure this field wants in
    # every case (see build_plane1_type.py).
    "max_rpm": 1900.0,
    # THE PT6 FAMILY'S FIGURES, copied from plane2 and plane5 deliberately: the same free-
    # turbine engine does not spool differently at this resolution.
    "spool_up_seconds": 4.0,
    "spool_down_seconds": 9.0,
}

# PUBLISHED FIELD LENGTHS, both over a 50 ft obstacle - the definition every other type uses.
# 2,420 ft and 1,740 ft, rounded UP to the next 10 m. AirportMgr.Content.
# FieldLengthsCoverTheRoll asserts the roll fits under each, and that take-off stays under
# plane5's 1,006 m.
REQUIREMENTS = {
    "takeoff_field_length": 74000.0,   # 740 m
    "landing_field_length": 54000.0,   # 540 m
}

# Up to 14 seats or a pod of freight through a crew door, a cargo door and the pod doors.
# Twelve minutes, plane5's: between the 172's ten and the Twin Otter's fifteen.
TURNAROUND_SECONDS = 720.0

# THREE BLADES - the 100" Hartzell POH figure 1-1 names, and what plane10/scripts/build_prop.py
# arrays. UAirsideAgentAnim::PropStepDegrees resolves its frame step against this, so it must
# describe the mesh on screen.
PROP_BLADE_COUNT = 3

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
        # Load-or-create, never delete-and-recreate: an airline's fleet points at this.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    asset.set_editor_property("code", unreal.Name("B"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("Cessna 208B Grand Caravan"))

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

    # THE MEASURED-AGAINST-PUBLISHED TABLE, printed every run - every figure on the right is
    # off POH figure 1-1 (inches converted), so this is the model against its drawing.
    for label, got_uu, published_m in (
            ("span", m["footprint"]["wingspan"], 15.875),
            ("length", m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 12.675),
            ("wheelbase", abs(m["fixed_axle_x"] - m["steer_axle_x"]), 4.051),
            ("track", m["main_gear_track"], 3.556),
            ("tailplane", m["footprint"]["tailplane_span"], 6.248),
            ("propeller", m["prop_diameter"], 2.540)):
        got_m = got_uu / 100.0
        say("  %-12s measured %6.3f m against the POH's %6.3f m  (%+.1f%%)"
            % (label, got_m, published_m, 100.0 * (got_m - published_m) / published_m))

    lock = lock_from_poh_radius()
    if abs(lock - STEERING["max_steer_degrees"]) > LOCK_TOLERANCE_DEGREES:
        fail("the POH's 33'-8\" turn implies %.1f deg of lock on this model, against the %.0f "
             "typed - re-derive STEERING" % (lock, STEERING["max_steer_degrees"]))
    else:
        say("PASS the POH's 33'-8\" turn implies %.1f deg of lock; %.0f typed"
            % (lock, STEERING["max_steer_degrees"]))
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
    # GRASS AND VISUAL. Unpaved strips are what a Caravan is bought for; this type must never
    # be the reason a player paves anything - plane5 is what asks for that.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.GRASS)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    # NO GEAR CYCLE IS DECLARED, AND THE OMISSION IS THE STATEMENT - plane1's ruling.

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the rig facts: wheel radius and blade count, on the
    generated class's defaults. Left unset, it would spin the Meridian's 21 uu wheels and
    drive a 3-blade disc at the class default's step - right by coincidence, so written
    anyway rather than relied on."""
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
            say("PASS ABP_Plane10 %s = %s" % (prop, got))


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

    requirements = asset.get_editor_property("requirements")
    takeoff = requirements.get_editor_property("takeoff_field_length")
    landing = requirements.get_editor_property("landing_field_length")
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m)"
        % (takeoff, takeoff / 100.0, landing, landing / 100.0))
    say("PASS surface %s, approach %s"
        % (requirements.get_editor_property("minimum_surface"),
           requirements.get_editor_property("approach_needed")))

    # THE FIXED GEAR, ASSERTED - plane1's check, for plane1's reason: a cycle copied in from a
    # retracting type would drive bones this rig does not have, and nothing would say so.
    gear = asset.get_editor_property("gear")
    travel = gear.get_editor_property("travel_seconds")
    if travel != 0.0:
        fail("gear.travel_seconds is %.2f - a Caravan's gear is fixed, so this type must "
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
