"""Authors DA_Aircraft_Plane6 and sets ABP_Plane6's per-model animation figures. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as build_plane2_type.py,
build_plane3_type.py and build_plane4_type.py state it and for the same reason: a figure
typed here is a second opinion about an object the export already answers for, and it does
not move when the model does. The measuring half is shared - airside_import.part_bounds_uu -
so the scripts cannot disagree about which glTF axis is UE's span.

WHAT THIS TYPE IS FOR. plane2 is the STOL end (grass, 366 m), plane3 the regional step up
that makes a runway a decision (tarmac, 1,402 m), plane4 the narrowbody JET (2,316 m, 189
seats, forty minutes). This is the WIDEBODY, and what it adds is not more of the same: it is
the first aeroplane in the game at any aerodrome code letter but B or C. A Code E stand is
half again as wide as a Code C one and its taxiways are 23 m rather than 15, so an airport
that can take this has been REBUILT rather than extended. Its forty-minute turnaround becomes
ninety.

THE PRIMARY SOURCE IS ON DISK FOR THE SHAPE AND NOT FOR THE PERFORMANCE, and that asymmetry
is stated here rather than discovered later. plane6/concept/777-300ER.dxf is the three-view
the model is traced from, so every measured figure below is checkable against a drawing in
the repo. There is NO Boeing D6-58329 (777 Airplane Characteristics for Airport Planning) in
that folder - unlike plane4's D6-58325-7 and plane5's Specification and Description - so the
field lengths, speeds, spool times and gear timings are published figures WITHOUT a document
here to check them against. They are marked as such at each site. Anyone who adds D6-58329 to
plane6/concept/ should re-read REQUIREMENTS and GROUND against it first; that is the exact
shape of the mistake build_plane4_type.py's header records under "the accusation that was
withdrawn", with the primary source sitting on disk the whole time.

WHY NO UAircraftType::Build777 TO GO WITH IT. plane4's header explains why DA_Aircraft_B738
exists beside DA_Aircraft_Plane4 - the paper 737 lets stand layout and code classification
reason about a 737 with no model. There is no paper 777 and there is no reason to add one:
the paper types exist because they predate the models, and a new type that arrives WITH its
model has nothing to be a placeholder for. IcaoCode's Code E row is therefore the first row
in that table measured against an ASSET rather than a C++ builder - see MAX_TAIL_AFT_E below.
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
TYPE_NAME = "DA_Aircraft_Plane6"
ABP = "/Game/Aircraft/Plane6/ABP_Plane6"
# The ICAO type designator for a 777-300ER, which is what a flight plan and a stand board
# call it. B773 is the non-ER -300; the two have different weights and different field
# lengths, so the extra letter is not decoration.
SHORT_CODE = "B77W"
MESH = "/Game/Aircraft/Plane6/SK_Plane6"

SOURCE = r"C:\repos\AirportMgr2Models\plane6\export\plane6.glb"

# The parts this type is measured from. NAMED HERE rather than assumed equal to plane4's,
# because a rig that renamed one should raise rather than default. This airframe's lifting
# surface is "wing" and its leg is "maingear_L", both as plane4's; its "propeller" is a
# TURBOFAN called "fan_L", also as plane4's.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear_L"
PROP = "fan_L"

# THE MAIN WHEELS, AND THIS IS THE FIRST ROW IN THE FLEET THAT IS NOT A PAIR.
#
# Every aeroplane before this one has ONE main wheel a side, on a bone called wheel_L /
# wheel_R. A 777 main leg carries a THREE-AXLE BOGIE, so plane6/scripts/build_rig.py names
# six - a bone turns about its own length, and one bone cannot roll three axles without the
# outer two orbiting it rather than spinning.
#
# THE BOGIE'S CENTRE IS THE AXLE, which is why these are averaged rather than one being
# picked. A multi-axle truck pivots about its centre, and that is the point
# FChassis::FixedAxleX means.
#
# IT READS -3123.0 uu AND THE MIDDLE AXLE IS AT -3122.0, a centimetre apart, because the
# bogie is not quite evenly spaced: 1.45 m from the forward axle to the middle and 1.48 m
# from the middle to the aft. Boeing publish the wheelbase as 31.22 m, measured to the
# MIDDLE axle, which is why the two figures are worth stating side by side rather than one
# being rounded into the other. Naming the middle axle here would be correct today and would
# silently stop being correct the day the bogie is re-spaced or gains a fourth axle; the
# average follows the truck.
MAIN_WHEELS_L = ("wheel_L1", "wheel_L2", "wheel_L3")
MAIN_WHEELS_R = ("wheel_R1", "wheel_R2", "wheel_R3")

# TWENTY-TWO BLADES - a GE90-115B, and plane6/scripts/build_fan.py arrays exactly that many
# because the engine has that many. This is NOT cosmetic: UAirsideAgentAnim::PropStepDegrees
# resolves the frame rate against ONE BLADE REPEAT, 360/N degrees, so a 22-blade fan told it
# has the default three is allowed a step seven times too large and aliases backwards at any
# frame rate this game runs at.
PROP_BLADE_COUNT = 22

# EIGHTY-ONE DEGREES, which is ALSO UAirsideAgentAnim's default - and it is set anyway.
# plane6/scripts/door_pivots.json keys open_deg at 81.0, so this rig happens to agree with the
# figure that default was measured from (plane4's). A default that HAPPENS to be right is not
# a declaration: it goes on being right only until somebody retunes the default for a
# different aeroplane, and then this rig's doors stop 9 degrees from shut with nothing saying
# why. Written down, and read back below.
BAY_DOOR_CLOSED_ANGLE_DEGREES = 81.0

# IcaoCode.cpp's Code E row, as this script expects to find it. See author_type(), which
# checks the measured tail against it and says so either way.
#
# 6800 SINCE 2026-09-21, RAISED FROM 6700 FOR THIS AEROPLANE. plane6's tail reaches 6799.3 uu
# aft of the nose-gear stop mark, so on the old figure a Code E stand would have laid its
# ground geometry a metre inside the 777's own tail. Code E was an AUTHORED design value -
# the row's own comment invites exactly this revision when a type arrives that exceeds it -
# and it is now MEASURED, as Code C has been since the 737-800 was corrected on 2026-09-19.
#
# 6902 SINCE 2026-09-25: plane11's A350-1000 reaches 6901.3 uu and the row followed it. This
# aeroplane is now 103 uu inside rather than tight against it.
MAX_TAIL_AFT_E = 6902.0


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
        # 64.78 m OVER THE RAKED WINGTIPS, which on this type is the figure everything turns
        # on rather than a detail: Code E runs to 65 m, so the whole classification of this
        # aeroplane sits 22 cm inside its ceiling. A 777-300ER really is that close - Boeing
        # publish 64.80 m - and it is why the folding-wingtip 777X needed the fold to stay
        # out of Code F.
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The fan's widest sweep is its disc, which lies across the airframe rather than along
    # it - so the larger of the two cross-axis extents, never the chordwise one. Reads 3.10 m,
    # which is the DRAWING's fan ring (plane6/concept/777-300ER.dxf, via build_fan.py's
    # FAN_R = 1.550) rather than the GE90-115B's published 3.25 m bare fan. The drawing is
    # the primary source that is actually on disk, and it is what the model traces; the
    # published figure is recorded here so the 4.6% gap is a known one rather than a
    # discovery.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane6's reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, the rule build_plane3_type.py settled: the bone is
    a STATEMENT about where the axle is - build_rig.py places each wheel's origin on its
    rotation axis so the thing can spin - and the hub's HEIGHT above the contact plane IS the
    radius. That z = 0 is the contact plane is not assumed: airside_import.report_bounds FAILS
    an import whose lowest vertex is more than 10 uu off it, so a model that floats never
    reaches this script.

    SIX WHEELS, AVERAGED, WHERE EVERY OTHER SCRIPT HERE READS TWO. See MAIN_WHEELS_L for why
    the bogie's CENTRE is the axle this wants. The averaging is also what makes the three
    checks below possible, and they are the reason this is not simply
    `(at["wheel_L2"].x + at["wheel_R2"].x) * 0.5`: a bogie mis-rigged as three coincident
    bones, or as three bones at three different heights, would average to a perfectly
    plausible number.
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
        raise KeyError("SK_Plane6 has no %s bone(s) - the rig was renamed. Bones: %s"
                       % (", ".join(missing), ", ".join(sorted(at))))

    left = [at[name] for name in MAIN_WHEELS_L]
    right = [at[name] for name in MAIN_WHEELS_R]
    both = left + right

    def mean(values):
        return sum(values) / float(len(values))

    mains_x = mean([v.x for v in both])
    radius = mean([v.z for v in both])
    track = abs(mean([v.y for v in left]) - mean([v.y for v in right]))

    # A BOGIE IS THREE AXLES, SO ITS BONES MUST BE AT THREE STATIONS. Three bones stacked on
    # one x average to exactly the right-looking wheelbase and roll three tyres through each
    # other; nothing on screen distinguishes that from a correct bogie until the gear is seen
    # from the side.
    stations = sorted(v.x for v in left)
    spread = stations[-1] - stations[0]
    if spread < radius:
        raise ValueError("the three left main wheel bones span %.1f uu, less than one "
                         "wheel's %.1f uu radius - they are stacked on one station rather "
                         "than on three axles" % (spread, radius))

    # AND AT ONE HEIGHT AND ONE TRACK, because they share an axle beam. A wheel off the beam
    # is a wheel that is not on the ground, and the radius above would average the error away.
    for name in MAIN_WHEELS_L + MAIN_WHEELS_R:
        if abs(at[name].z - radius) > 1.0:
            raise ValueError("%s's hub is at z=%.1f against the bogies' mean %.1f - the six "
                             "main wheels are not on one plane, so one of them is off the "
                             "ground" % (name, at[name].z, radius))

    # THE TRACK, MEASURED, for the same reason the axles are: it is a fact about the model
    # that is drawn, not about the aeroplane in the datasheet. Anything hung off it - tyre
    # smoke at touchdown, a tug lining up - lands under the wheels the player can see.
    if track <= radius:
        raise ValueError("the two bogies are %.1f uu apart, which is inside one wheel's "
                         "%.1f uu radius - the main gear bones are on top of each other"
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


# --- Published, from the Boeing 777-300ER ---------------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# NO PRIMARY DOCUMENT ON DISK FOR ANY FIGURE IN THIS SECTION - see the header. These are the
# aeroplane's published figures at MTOW (351.5 t), sea level, ISA, and they are the first
# performance block in this project that cannot be checked against something in the models
# repo. Treated as provisional in the one way that costs nothing: every one of them is
# commented with what it is meant to be, so a reader with D6-58329 open can check a line at a
# time rather than re-deriving the set.
GROUND = {
    # THE FLEET'S TAXI FIGURES, UNCHANGED, and that is deliberate rather than lazy.
    # build_plane5_type.py puts it best: "a taxi speed is a procedure, not a capability, so a
    # type that differed here would be saying something it does not mean". A 777 taxis at the
    # same 19 kn a Twin Otter does. What differs is the DECEL, and it does not differ here
    # either - the fleet shares 200, and singling this one out would be tuning by intuition
    # against no figure at all.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 220 IS BELOW plane4's 250 AND plane3's 250, WHICH IS THE POINT. A 777-300ER's
    # thrust-to-weight at MTOW is a shade under a loaded 737's, and it has to reach a Vr 20 kn
    # higher - so the roll is longer at both ends. RequiredRoll works out to about 1,990 m
    # against the 3,120 m published below, which is the margin a published figure is supposed
    # to have.
    "takeoff": dict(accel=220.0, decel=400.0, speed_cap=8481.0),   # Vr about 165 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=7659.0),   # Vref about 149 kn
}

# STEERING, published rather than measured - nothing in the mesh knows how far the tiller
# turns or how hard a pilot will corner.
#
# 70 DEGREES is the 777's tiller authority, the same figure the Q400 carries and 8 less than
# the 737's. On the measured 31.22 m wheelbase that makes the tightest followable radius about
# 33 m (R >= L / sin(lock), the rule FSpeedProfile applies) - more than DOUBLE plane4's 16 m,
# and the single most important consequence of accepting this type. It is a REQUIREMENT ON THE
# AIRPORT and the first thing to suspect if a 777 refuses a route a 737 takes happily;
# author_type() logs the computed figure for that reason.
#
# 0.12 g, the gentlest in the fleet, against the airliners' 0.15 and the Meridian's 0.25. A
# 350 t aeroplane on a bogie that scrubs sideways through every turn is not cornered briskly.
STEERING = {
    "max_steer_degrees": 70.0,
    "max_lateral_accel_uu": 118.0,
}

CLIMB = {
    # EIGHT DEGREES, LOWER THAN plane4's NINE, AND IT IS THE -300ER's DEFINING NUMBER. This is
    # the longest airliner Boeing have built and it is famously tail-strike-prone: the type
    # carries a dedicated tail-strike protection system because the geometry runs out at
    # around 9 degrees with the gear compressed. Rotating to 8 is what the aeroplane does.
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 11.0,
    # TWO DEGREES A SECOND, against plane4's three and the fleet's default four - for the
    # reason above. A -300ER is rotated deliberately slowly.
    "rotate_rate_deg_per_sec": 2.0,
    "climb_speed": 12850.0,       # 250 kn, the standard climb speed below 10,000 ft
    # 650 m, HIGHER THAN plane4's 500, AND THE FIGURE IS DERIVED RATHER THAN CHOSEN. The gear
    # cycle below is 16 seconds and starts at 90 m; at 250 kn on an 11-degree climb that is
    # about 395 m of height gained before the doors are shut, so an agent cleared at 500 m
    # would be close to vanishing mid-retraction on a slow climb. author_type() asserts the
    # relation rather than trusting this comment.
    "clear_altitude": 65000.0,    # 650 m
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    "flare_height": 1000.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # THE REAL FAN SPEED, not a display figure. A GE90-115B turns about 2,355 rpm at 100 % N1 -
    # a big fan turns SLOWLY, less than half the CFM56-7B's 5,175 - and FAgentMotion::EngineRPM
    # carries the honest number so anything reasoning about the engine reads the truth. The
    # VIEW is what chooses a rotation it can show: see UAirsideAgentAnim::PropDisplayCapRPM,
    # which caps the drawn rate, and PropBladeCount, which wants this fan's 22 blades.
    "max_rpm": 2355.0,
    # THE SLOWEST SPOOL IN THE FLEET, which is what a 115,000 lbf fan is. Idle to takeoff N1 is
    # about ten seconds against the 737's eight, and a shut-down GE90 windmills down for the
    # better part of a minute.
    "spool_up_seconds": 10.0,
    "spool_down_seconds": 30.0,
}

# THE GEAR. plane4 was the first retractable type, plane5 and plane7 the first with doors at
# both ends; this one is the slowest and the largest, and it is the first whose cycle forced
# ClearAltitude up rather than fitting inside the existing figure.
GEAR = {
    # 12 SECONDS, against the 737's 7. A 777 main leg is a six-wheel bogie swinging into a
    # well between the wing box and the belly fairing, and it takes its time.
    "travel_seconds": 12.0,
    # TWO SECONDS A DOOR, against the 737's one - these are big doors. A full cycle is
    # therefore 16 seconds: doors open, gear travels, doors close.
    "door_seconds": 2.0,
    # ZERO, AND IT IS A FACT ABOUT THIS AEROPLANE RATHER THAN AN OMISSION. A real 777 main
    # truck DOES tilt, and FGearPerformance::TruckTiltSeconds was added on 2026-09-21 for this
    # very model - as were BONE_RULES' `truck` and `bogie` needles - but plane6 then shipped
    # with its bogies RIGID on their legs and no bone for a tilt to drive. Setting a non-zero
    # figure here would animate nothing and would make the asset claim a part the mesh does
    # not have. plane8's A380 is where the field starts earning its place.
    "truck_tilt_seconds": 0.0,
    # A FEW HUNDRED FEET - 9000 uu is about 295 ft - because raising the gear is a pilot
    # command and not a consequence of lift-off. FAgentMotion::bAirborne is the precondition.
    # The same figure the rest of the fleet carries: it is a procedure, not a capability.
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
#
# 3,120 m IS THE WHOLE GAMEPLAY CLAIM OF THIS TYPE: 804 m more than the 737-800 and more than
# twice the Q400's. There is no runway in the starting field remotely near it.
REQUIREMENTS = {
    "takeoff_field_length": 312000.0,   # 3,120 m
    "landing_field_length": 186000.0,   # 1,860 m at max landing weight
}

# NINETY MINUTES. plane4's 189 seats through two doors take forty; a 777-300ER's 396 through
# four take more than twice as long, and that is before the catering and the fuel uplift that
# a long-haul turn needs and a narrowbody turn does not. It is the longest in the game and it
# is meant to be felt: a stand occupied for ninety minutes is a stand the player has to build
# a second of.
TURNAROUND_SECONDS = 5400.0

# PUSHBACK IS NOT SET HERE, AND THAT IS THE ONE FIELD THIS SCRIPT DELIBERATELY LEAVES ALONE.
# Tools/Python/build_pushback_needs.py owns EPushbackNeed for every type and says so - "the
# entries are the whole list" - because the depot progression is one content decision rather
# than a field each type's own script happens to write. plane6's row lives there, and
# Airside.Content.PushbackNeedsAuthored asserts the same table. Writing it from here as well
# would be a second author of one field with no compiler between them, which is the failure
# CLAUDE.md names under "lists that must agree are ONE list".


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

    # CODE E, AND IT IS THE FIRST TYPE IN THE GAME AT ANY LETTER BUT B OR C. The measured span
    # is 64.78 m and Code E runs to 65 - so this aeroplane is 22 cm inside its own ceiling,
    # and there is no headroom at all for a re-export that grows the wing. The letter is
    # checked against the measurement below rather than merely asserted here.
    asset.set_editor_property("code", unreal.Name("E"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("777-300ER"))

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
    say("measured off the rig: steer axle %.1f, bogie centre %.1f uu (wheelbase %.1f), "
        "track %.1f" % (m["steer_axle_x"], m["fixed_axle_x"],
                        abs(m["fixed_axle_x"] - m["steer_axle_x"]), m["main_gear_track"]))

    # THE SPAN AGAINST THE LETTER'S CEILING, every run, because this type has 22 cm of it.
    # Code E ends at 6500 uu; a re-export that grows the wing past that makes the authored
    # letter a lie, and the symptom would be a 777 parking on a stand 1.5 m too narrow for it
    # rather than anything visibly wrong with the model.
    span = m["footprint"]["wingspan"]
    if span > 6500.0:
        fail("the measured span is %.1f uu, past Code E's 6500 - this type is authored Code "
             "E and is no longer one. Either the export grew or the letter is now F." % span)
    else:
        say("PASS span %.1f uu is inside Code E's 6500 by %.1f uu (%.2f m)"
            % (span, 6500.0 - span, (6500.0 - span) / 100.0))

    # AND THE TAIL AGAINST THE ROW THAT WAS RAISED FOR IT. See MAX_TAIL_AFT_E. This is the
    # check that would catch the row being reverted, or this model's tail growing past the
    # figure the row was raised to - either of which puts a Code E stand's ground geometry
    # inside the aeroplane's own tail clearance.
    tail_aft = -(m["footprint"]["tail_x"] - m["steer_axle_x"])
    if tail_aft > MAX_TAIL_AFT_E:
        fail("the tail reaches %.1f uu aft of the stop mark against IcaoCode's Code E row of "
             "%.0f - a Code E stand would lay its GSE road inside this aeroplane. Raise the "
             "row in Solve/IcaoCode.cpp and this constant with it."
             % (tail_aft, MAX_TAIL_AFT_E))
    else:
        say("PASS the tail reaches %.1f uu aft of the stop mark, inside Code E's %.0f"
            % (tail_aft, MAX_TAIL_AFT_E))

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

    # THE GEAR CYCLE MUST FINISH BEFORE THE AGENT IS REMOVED, asserted rather than argued -
    # and against the climb figures just written, not against a copy of them. An agent removed
    # from the world part way through its cycle vanishes with its gear half up, in front of the
    # player, and the only symptom is that it looked wrong for a moment. plane7's script
    # introduced this check; this is the first type whose cycle actually forced the figure up.
    cycle = GEAR["door_seconds"] * 2.0 + GEAR["travel_seconds"]
    rate = CLIMB["climb_speed"] * math.radians(CLIMB["climb_pitch_degrees"])
    top = GEAR["retract_above_height"] + rate * cycle
    if top >= CLIMB["clear_altitude"]:
        fail("the %.0f s gear cycle finishes at about %.0f uu but the agent is cleared at "
             "%.0f - it would vanish mid-retraction. Raise ClearAltitude or shorten the cycle."
             % (cycle, top, CLIMB["clear_altitude"]))
    else:
        say("PASS the %.0f s gear cycle finishes at about %.0f uu, below the %.0f uu the "
            "agent is cleared at" % (cycle, top, CLIMB["clear_altitude"]))

    requirements = asset.get_editor_property("requirements")
    for field, value in REQUIREMENTS.items():
        requirements.set_editor_property(field, value)
    # TARMAC AND VISUAL. Every type from plane5 up demands tarmac; what this one adds is
    # LENGTH - 3,120 m against plane4's 2,316 and plane3's 1,402 - so the progression stays a
    # longer runway rather than a different surface. The approach stays VISUAL: an instrument
    # approach is a building the game does not have yet, and demanding one would refuse every
    # runway in it.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of three measured or per-model figures, on the generated class's
    defaults.

    THE WHEEL RADIUS defaults to the Meridian's 21 uu. Left unset, a 777's 66 uu wheels turn
    at more than three times the right rate against ground speed - which reads as skating.

    THE BLADE COUNT defaults to 3 and a GE90-115B fan has 22 - see PROP_BLADE_COUNT.

    THE DOOR CLOSED ANGLE already defaults to this rig's 81 and is written anyway - see
    BAY_DOOR_CLOSED_ANGLE_DEGREES.
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
    defaults.set_editor_property("prop_blade_count", PROP_BLADE_COUNT)
    defaults.set_editor_property("bay_door_closed_angle_degrees",
                                 BAY_DOOR_CLOSED_ANGLE_DEGREES)
    unreal.EditorAssetLibrary.save_asset(ABP, only_if_is_dirty=False)

    after = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(ABP).generated_class())
    got_radius = after.get_editor_property("main_wheel_radius")
    got_blades = after.get_editor_property("prop_blade_count")
    got_door = after.get_editor_property("bay_door_closed_angle_degrees")
    if abs(got_radius - radius) > 0.01:
        fail("ABP wheel radius read back as %.1f, expected %.1f" % (got_radius, radius))
    else:
        say("PASS ABP_Plane6 wheel radius %.1f -> %.1f uu" % (before, got_radius))
    if got_blades != PROP_BLADE_COUNT:
        fail("ABP blade count read back as %d, expected %d" % (got_blades, PROP_BLADE_COUNT))
    else:
        say("PASS ABP_Plane6 blade count = %d, the GE90-115B's fan" % got_blades)
    if abs(got_door - BAY_DOOR_CLOSED_ANGLE_DEGREES) > 0.01:
        fail("ABP door closed angle read back as %.1f, expected %.1f"
             % (got_door, BAY_DOOR_CLOSED_ANGLE_DEGREES))
    else:
        say("PASS ABP_Plane6 bay door closed at %.1f deg, this rig's shut angle" % got_door)


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

    for prop, want in (("short_code", SHORT_CODE), ("code", "E")):
        got = str(asset.get_editor_property(prop))
        if got != want:
            fail("%s read back as %s, expected %s" % (prop, got, want))
        else:
            say("PASS %s = %s" % (prop, got))

    # REPORTED, NOT ASSERTED, because build_pushback_needs.py owns this field - see the note
    # beside TURNAROUND_SECONDS. Said out loud anyway: whichever script ran last, the value a
    # reader of this log cares about is the one on disk.
    say("NOTE pushback_need = %s (authored by build_pushback_needs.py, asserted by "
        "Airside.Content.PushbackNeedsAuthored)" % asset.get_editor_property("pushback_need"))

    for prop in ("mesh", "anim_class"):
        got = asset.get_editor_property(prop)
        if got is None:
            fail("%s is unset - the agent would fall back to the game-wide default mesh, "
                 "which is how a Twin Otter arrived as a Meridian" % prop)
        else:
            say("PASS %s = %s" % (prop, got.get_name()))

    # THE GEAR, READ BACK FIELD BY FIELD, including the zero. A set_editor_property that
    # silently no-ops on an unknown field name reports success and saves cleanly - which would
    # leave this type with TravelSeconds 0, indistinguishable on screen from an aeroplane whose
    # gear is simply fixed. truck_tilt_seconds is read back FOR THE SAME REASON although it is
    # zero: if the name were wrong, the check would pass on the class default and say nothing.
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
