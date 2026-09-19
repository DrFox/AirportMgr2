"""Authors DA_Aircraft_Plane1 and sets ABP_Plane1's rig facts. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as build_plane2_type.py,
build_plane3_type.py and build_plane4_type.py state it and for the same reason: a figure
typed here is a second opinion about an object the export already answers for, and it does
not move when the model does. The measuring half is shared - airside_import.part_bounds_uu -
so the four scripts cannot disagree about which glTF axis is UE's span.

WHAT THIS TYPE IS FOR. It is the SMALLEST AEROPLANE IN THE GAME, and that is the whole
entry: 11.00 m of span against the Meridian's 13.11 and the Twin Otter's 19.75, on a 1.63 m
wheelbase that will follow a 3.3 m corner. Nothing else in the fleet fits where this fits.

IT IS NOT THE SHORTEST-FIELD TYPE, and that surprise is worth writing down because it
reverses the obvious guess. A 172S needs 497 m over a 50 ft obstacle; plane2's Twin Otter
needs 366 m. A DHC-6 genuinely out-STOLs a Cessna 172 - it is a STOL aeroplane and this is a
trainer - so the ladder this type joins is the SPAN ladder, not the length one. Its published
length lands within 3% of the Meridian's 510 m, so on runway length alone the two are
interchangeable and the 172 offers wherever the Meridian does. That overlap is deliberate:
the fleet gains variety here rather than a new capability tier.

THE GEAR IS FIXED, so this type declares no gear cycle at all. A 172 has nothing to retract
and its rig has no gear or door bones to drive - see
docs/superpowers/specs/2026-09-19-gear-retraction-design.md for what a type that DOES
retract carries. plane4 remains the only retracting type, which is why DefaultGame.ini's
Land key still points at it.

THE FIGURES AGREE WITH THE REAL AEROPLANE TO WITHIN 5 CM, which is unusual enough to record
rather than assume will stay true. Cessna 172S Skyhawk SP against this export:

    length     8.28 m      measured 8.233
    span      11.00 m      measured 11.000
    height     2.72 m      measured 2.659   (see below)
    wheelbase  1.63 m      measured 1.630
    track      2.51 m      measured 2.540
    propeller  1.905 m     measured 1.919   (76 in McCauley, 2 blades)

HEIGHT READ 2.720 UNTIL 2026-09-19 AND THE CHANGE IS NOT A REGRESSION. The highest vertex in
the export was the tail beacon, at 272.0 uu; the FIN tip is at 265.9. The beacon and the two
other light meshes were removed from the export, so the figure now measures the aeroplane
rather than a lamp on top of it - which makes the 2.72 m agreement it used to show a
coincidence worth not having trusted. Nothing reads height: the footprint carries nose_x,
tail_x and the two spans, and the aerodrome code letter is decided by span and gear track.

The two wheel RADII are the loose ones - 0.198 m measured against a 6.00-6's 0.222, and
0.169 against a 5.00-5's 0.180 - and they are left alone. They are the figures the animation
spins and the tyre smoke sits on, so they must describe the model that is drawn; a published
radius would be right about the aeroplane and wrong about SK_Plane1.

THE RIG WAS FIXED AT THE EXPORT RATHER THAN WORKED AROUND HERE, three times on 2026-09-19 -
one `wheel` bone driving both mains from the right wheel's outer face, a `nosewheel` bone
12 uu off the centreline, and the wing and tailplane merged into `fuselage` with nothing for
the footprint to measure. import_models.py's Spec carries the detail. The alternative each
time was a special case in this file, and a special case outlives the model it was written
for.
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
TYPE_NAME = "DA_Aircraft_Plane1"
ABP = "/Game/Aircraft/Plane1/ABP_Plane1"
SHORT_CODE = "C172"
MESH = "/Game/Aircraft/Plane1/SK_Plane1"

SOURCE = r"C:\repos\AirportMgr2Models\plane1\export\plane1.glb"

# The parts this type is measured from. NAMED HERE rather than assumed equal to plane3's or
# plane4's, because two of the four are not: this airframe's propeller is the singular `prop`
# where every other aeroplane in the fleet is a twin with `prop_L`, and its main gear is one
# `maingear` where plane4 has `maingear_L` and `maingear_R`. A wrong stem raises rather than
# defaulting.
#
# `maingear` IS BOTH LEGS IN ONE OBJECT, and that is fine here: the only thing taken from it
# is its HEIGHT, as the sanity bound on the wheel radius below. A one-object leg would be
# wrong for an AXLE - a leg's centre is not an axle - and nothing here asks it for one. It
# was called `wheelstrut` until 2026-09-19, a name in the tyre namespace for a part that is
# not a tyre.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear"
PROP = "prop"


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
    # THE NOSE IS THE SPINNER, not the fuselage, and on this aeroplane the two differ by
    # 19 uu - the propeller sits ahead of everything. Taking the maximum over every part is
    # what gets that right without this file having to know which part is foremost, and it
    # is the same expression the other three scripts use.
    nose_x = max(high[0] for _, high in every)
    # And the tail is the FIN, which reaches 79 uu behind the fuselage's own box.
    tail_x = min(low[0] for low, _ in every)

    wing_lo, wing_hi = find(WING)
    stab_lo, stab_hi = find(STABILISER)
    prop_lo, prop_hi = find(PROP)
    leg_lo, leg_hi = find(GEAR_LEG)

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        "wingspan": wing_hi[1] - wing_lo[1],   # the 172's published span is 11.00 m exactly
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The prop's widest sweep is its disc, which lies across the airframe rather than along
    # it - so the larger of the two cross-axis extents, never the chordwise one. On a TWO
    # BLADE propeller parked vertically the two are far apart: 1.92 m across the disc against
    # 0.27 m of blade chord, and picking the smaller would publish a propeller the size of a
    # dinner plate.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane1's reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, the rule build_plane3_type.py settled: the bone is
    a STATEMENT about where the axle is - build_export.py places each wheel's origin on its
    rotation axis so the thing can spin - and the hub's HEIGHT above the contact plane IS the
    radius. That z = 0 is the contact plane is not assumed: airside_import.report_bounds
    FAILS an import whose lowest vertex is more than 10 uu off it, so a model that floats
    never reaches this script.

    THIS RIG EARNED THAT RULE THE HARD WAY. Until 2026-09-19 it had a single `wheel` bone for
    both mains, sitting at y +137.0 - the right wheel's outer FACE, 10 uu outboard of its own
    axle and 264 uu from the left one's. A bounding box would have read the pair as one box
    and reported a plausible-looking track of 274 uu; the BONE said, correctly, that there
    was no second axle to measure from. The export was split into wheel_L/wheel_R rather than
    this function being taught to guess.
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
            raise KeyError("SK_Plane1 has no %s bone - the rig was renamed. Bones: %s"
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

    Three places need these figures - the type, the ABP's defaults and the read-back
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


# --- Published, from the Cessna 172S Skyhawk SP POH ------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s. All figures are at
# 2,550 lb, sea level, ISA, no wind - POH section 5, the same conditions the other three
# types are published at.
#
# A 1,157 kg piston single with 180 hp, and every figure below is what separates it from the
# lightest thing already in the game. It rotates at 55 kn where plane2 rotates at 60 and
# plane3 at 120, and climbs out at 74 against 85 and 170 - so it leaves the ground sooner and
# slower than anything else, and then goes nowhere in a hurry.
GROUND = {
    # Taxi as the others do: a 172 does not taxi faster than a Twin Otter. 19 kn is the whole
    # fleet's figure and a difference here would be a difference nobody asked for.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 140 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, not guessed: Vr^2 / 2a
    # with Vr = 2827 gives 285 m against the POH's 293 m, and FTakeoffRun adds the rotation
    # on top to reach about 332 m. Compare plane2's 450 - a Twin Otter accelerates more than
    # three times as hard as a trainer, which is most of why it out-STOLs one.
    "takeoff": dict(accel=140.0, decel=400.0, speed_cap=2827.0),   # Vr 55 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=3135.0),   # Vref 61 kn, flaps 30
}

# STEERING, published rather than measured - nothing in the mesh knows how far the nosewheel
# turns or how hard a pilot will corner.
#
# 30 DEGREES, THE LOWEST LOCK IN THE GAME, PRODUCING THE TIGHTEST CORNER IN THE GAME, which
# is only a contradiction if the wheelbase is forgotten. A 172 has no tiller: the nosewheel
# steers about 10 degrees off the rudder pedals and the rest comes from differential braking,
# so 30 is an effective authority rather than a mechanical limit. On the measured 1.63 m
# wheelbase that still makes R >= L / sin(lock) about 3.3 m - against plane2's 5.2 and
# plane3's 15.0. There is no taxiway this type can be refused for being unable to turn, and
# tightest_radius_uu() logs the figure so that stays checkable rather than assumed.
#
# 0.25 g is the light-single end of the range, shared with the Meridian - plane2 corners at
# 0.2 and an airliner at 0.15. The lightest aeroplane on the apron is driven round it
# hardest.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    # 5 degrees a second, the quickest in the fleet: a trainer's elevator is light and its
    # tail is a metre and a half behind the mains. plane2 rotates at 4, plane3 at 3.
    "rotate_rate_deg_per_sec": 5.0,
    "climb_speed": 3804.0,        # 74 kn, Vy at sea level
    # THE SAME CIRCUIT AS plane2, deliberately. Both are light aeroplanes flying a visual
    # circuit off the same field, and a 172 leaving it at a different height from a Twin
    # Otter would be a distinction with no rule behind it.
    "clear_altitude": 30000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    # A 172 flares lower than anything else here - plane2 at 900, plane3 at 1000 - because
    # the flare height scales with how far the pilot's eye is above the mains, and on this
    # aeroplane that is under two metres.
    "flare_height": 800.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE, WHICH IS WHY THIS IS 2700 AND NOT A REDUCTION OF IT. The Lycoming
    # IO-360-L2A turns the propeller itself - there is no gearbox - so engine redline and
    # propeller rpm are the same number. plane2's 2200 and plane3's 1020 are both PROP rpm
    # behind a reduction gear, which is the figure this field wants in every case.
    "max_rpm": 2700.0,
    # A piston single spools in about two seconds. The turboprops take 4 and 5.
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes -
# Airside.Model.FieldLengthsCoverTheRoll states the rule: a published figure may be generous,
# but one shorter than the roll admits an aircraft to a strip it then runs off the end of.
#
# Both are the POH's TOTAL over a 50 ft obstacle rather than the ground roll, which is the
# definition plane2, plane3 and plane4 all use. Taking the 172's 293 m ground roll instead
# would have made it the shortest-field type in the game, and it would have done so by
# measuring a different thing from every other type in the same field. Two definitions in one
# property, chosen per aeroplane, is how a capability filter starts lying.
REQUIREMENTS = {
    "takeoff_field_length": 49700.0,   # 497 m, POH 1,630 ft over 50 ft
    "landing_field_length": 40700.0,   # 407 m, POH 1,335 ft over 50 ft
}

# Four seats and a door on each side. Ten minutes, the shortest in the game - plane2 turns in
# fifteen, plane3 in twenty-five, plane4 in forty.
TURNAROUND_SECONDS = 600.0

# TWO BLADES, COUNTED OFF THE EXPORT rather than taken from the datasheet: the prop_blade
# primitive's vertices fall in two lobes 180 degrees apart about the hub, with nothing in the
# four sectors between. UAirsideAgentAnim defaults to 3, and build_plane4_type.py records why
# that number is not cosmetic - the blade count sets the largest per-frame step the animation
# may take before the disc aliases backwards. plane4's error was in the other direction, a
# 24-blade fan told it had 3; this one would be allowed a step half again too large.
PROP_BLADE_COUNT = 2


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: an airline's fleet points at this -
        # DA_Airline_Cumbria names it - and deleting an asset something references breaks the
        # reference rather than updating it.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    # CODE A, THE FIRST MEASURED ONE IN THE GAME, and it is the letter this type exists to
    # introduce. The aerodrome reference letter is decided by span AND by outer main gear
    # wheel span: Code A is under 15 m and under 4.5 m, and this aeroplane measures 11.00 and
    # about 2.74. Every other modelled type is B or C, so until now nothing exercised the
    # bottom of that table. The Meridian is A as well, and is hand-built in C++ rather than
    # measured off a model.
    asset.set_editor_property("code", unreal.Name("A"))

    # SHORT CODE, which is not the aerodrome letter above. Code cannot tell two types apart -
    # an A320 and a 737 are both C - so FAirframe::TypeCode takes this instead. Anything that
    # has to SAY what an aircraft is reads that.
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("Cessna 172 Skyhawk"))

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
    say("measured off the rig: steer axle %.1f, fixed axle %.1f uu (wheelbase %.1f), "
        "track %.1f uu" % (m["steer_axle_x"], m["fixed_axle_x"],
                           abs(m["fixed_axle_x"] - m["steer_axle_x"]), m["main_gear_track"]))

    # THE FIGURE THAT DECIDES WHETHER THIS TYPE CAN USE THE PLAYER'S TAXIWAYS, said out loud
    # because it is not otherwise visible anywhere: the rolling-steer law refuses a corner
    # tighter than this, and a refusal reads as "no route" rather than as "too big".
    say("tightest followable radius %.0f uu (%.1f m) at %.0f degrees of lock - the smallest "
        "in the fleet" % (tightest_radius_uu(), tightest_radius_uu() / 100.0,
                          STEERING["max_steer_degrees"]))

    # THE MEASURED-AGAINST-PUBLISHED TABLE, PRINTED EVERY RUN, which is the habit
    # build_plane4_type.py's nose overhang argued for: a disagreement between the model and
    # the real aeroplane is worth SEEING each time rather than discovering. Nothing here
    # compensates for one - the model is the authority for everything the game draws, and a
    # fudge factor would outlive the fix.
    for label, got_uu, published_m in (
            ("span", m["footprint"]["wingspan"], 11.00),
            ("length", m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 8.28),
            ("wheelbase", abs(m["fixed_axle_x"] - m["steer_axle_x"]), 1.63),
            ("track", m["main_gear_track"], 2.51),
            ("propeller", m["prop_diameter"], 1.905),
            ("main wheel radius", m["wheel_radius"], 0.222)):
        got_m = got_uu / 100.0
        say("  %-18s measured %6.3f m against the 172S's %6.3f m  (%+.1f%%)"
            % (label, got_m, published_m, 100.0 * (got_m - published_m) / published_m))

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
    # GRASS AND VISUAL. A 172 flies off a farm strip, so this type must never be the reason a
    # player has to pave anything - plane3 is what asks for tarmac and plane4 for a lot of it.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.GRASS)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    # PUSHBACK IS NOT SET HERE, AND THE DEFAULT IS THE WRONG ONE. UAircraftType::PushbackNeed
    # defaults to EPushbackNeed::VehicleTug, so a type nobody authors asks for a tug - which
    # on a 172 would gate the lightest aeroplane in the game behind the Pushback depot, the
    # exact opposite of what it is for. build_pushback_needs.py owns that field for every
    # type and must be run after this script; Airside.Content.PushbackNeedsAuthored is what
    # fails if it is not. Setting it here as well would make two scripts the author of one
    # value, which is how the Piper's figures ended up typed at seven sites.

    # NO GEAR CYCLE IS DECLARED, AND THE OMISSION IS THE STATEMENT. A 172's gear is welded to
    # the airframe; leaving TravelSeconds at the C++ default of 0 is how a type says "nothing
    # retracts". Setting a cycle here would put a retraction on an aeroplane whose rig has no
    # bone to move, and nothing on screen would say so. verify() asserts the zero.

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the rig facts: wheel radius and blade count.

    On the generated class's default object, which is what the editor's Class Defaults panel
    edits and what every instance of the Anim Blueprint starts from. Left unset, this type
    would spin 21 uu wheels - the Meridian's default, 6% too large here, which reads as a
    slight skate - and drive a 2-blade disc at a 3-blade step.
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
    before_radius = defaults.get_editor_property("main_wheel_radius")
    before_blades = defaults.get_editor_property("prop_blade_count")
    defaults.set_editor_property("main_wheel_radius", radius)
    defaults.set_editor_property("prop_blade_count", PROP_BLADE_COUNT)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    # Read back from a fresh load, because a set_editor_property that reports success and
    # saves nothing is this project's most familiar failure.
    reloaded = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class())
    got_radius = reloaded.get_editor_property("main_wheel_radius")
    got_blades = reloaded.get_editor_property("prop_blade_count")
    if abs(got_radius - radius) > 0.01:
        fail("ABP wheel radius read back as %.1f, expected %.1f" % (got_radius, radius))
    else:
        say("PASS ABP_Plane1 wheel radius %.1f -> %.1f uu" % (before_radius, got_radius))
    if got_blades != PROP_BLADE_COUNT:
        fail("ABP blade count read back as %d, expected %d" % (got_blades, PROP_BLADE_COUNT))
    else:
        say("PASS ABP_Plane1 blade count %d -> %d, the McCauley's two"
            % (before_blades, got_blades))


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
        ("main_gear_track", asset.get_editor_property("main_gear_track"),
         m["main_gear_track"]),
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

    # THE FIXED GEAR, ASSERTED RATHER THAN ASSUMED, and the check earns its four lines
    # because the failure it catches is silent: a cycle copied in from plane4 would retract a
    # 172's welded gear, the only bones it could drive do not exist in this rig, so nothing
    # on screen would move and nothing would complain. The absence IS the declaration, and an
    # absence is exactly the kind of thing no other test looks at.
    gear = asset.get_editor_property("gear")
    travel = gear.get_editor_property("travel_seconds")
    if travel != 0.0:
        fail("gear.travel_seconds is %.2f - a 172's gear is welded on, so this type must "
             "declare no cycle at all" % travel)
    else:
        say("PASS gear.travel_seconds = 0, so nothing retracts, which is a 172")

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
