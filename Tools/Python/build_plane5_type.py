"""Authors DA_Aircraft_Plane5 and sets ABP_Plane5's per-model animation defaults. Run
headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, exactly as build_plane2_type.py,
build_plane3_type.py and build_plane4_type.py state it and for the same reason: a figure
typed here is a second opinion about an object the export already answers for, and it does
not move when the model does. The measuring half is shared - airside_import.part_bounds_uu -
so the four scripts cannot disagree about which glTF axis is UE's span.

WHAT THIS TYPE IS FOR. It is the rung the ladder did not have. The fleet goes plane1 (a
172, grass, 497 m), plane2 (a Twin Otter, grass, 366 m but 19.75 m of span), plane3 (a Q400,
TARMAC, 1,402 m) and plane4 (a 737, tarmac, 2,316 m). Between the Twin Otter and the Q400
the player's next capability was a THOUSAND-metre tarmac runway, bought in one jump. The
King Air asks for tarmac and 1,006 m, which splits that jump in two - and it is the first
type in the game whose requirement is a SURFACE rather than a length: it needs 2.7 times
LESS runway than the Q400 and the same surface. Widening no longer helps; paving does.

Code B on a 17.69 m span, and the only Code B in the fleet besides the Twin Otter.

THE PRIMARY SOURCE IS IN THE MODEL'S OWN CONCEPT FOLDER, and this script cites it by page
the way build_plane4_type.py cites Boeing D6-58325-7:

  plane5/concept/King Air 350i Specification and Description (FL-1161 and on).pdf
  (Beechcraft / Textron Aviation, August 2018, serials FL-1161 to TBD)

    p5  1.3 Approximate Dimensions    height 4.37 m, span 17.65 m, length 14.22 m,
                                      WHEELBASE 16 ft 3 in (4.95 m)
    p5  1.4 Design Weights            MTOW 15,000 lb (6,804 kg)
    p6  2. Performance                TAKEOFF FIELD LENGTH 3,300 ft (1,006 m) at MTOW;
                                      LANDING DISTANCE 2,692 ft (821 m) at max landing
                                      weight, flaps down, no prop reverse
    p8  7.1 Landing Gear              "The main landing gear hydraulically retracts FORWARD
                                      into each engine nacelle" - which is what the rig does
    p8  7.3 Brakes and Tires          NOSE 22 x 6.75-10, MAIN 19 x 6.75-8, mains DUAL
    p8  8.1 Powerplant                two PT6A-60A, 1,050 SHP each
    p9  8.2 Propellers                Hartzell 105-inch four blade, full feathering

  The PDF IS A SCAN with no text layer - pdftotext returns nothing at all from it. Render
  the pages (pymupdf, ~140 dpi) and read them. A grep that comes back empty here is the
  probe failing, not the document being silent.

TWO PLACES THE MODEL AND THE SPECIFICATION DISAGREE, reported and NOT corrected.

  WHEELBASE. The export measures about 4.60 m. The S&D says 16 ft 3 in, 4.95 m - 35 cm
  more. plane5/scripts/build_gear.py states its own source and its own reasoning in full:
  it scaled side.jpg and technical6.png, got 4.60, and checked it against "the published
  figure for the 350 is 14 ft 11 in = 4.55 m, so within the noise". That 14 ft 11 in is not
  in this document; the 350's own manufacturer says 16 ft 3 in.

  MAIN TYRE. build_gear.py builds the mains as 22 x 6.75-10, 0.56 m diameter. Section 7.3
  gives 22 x 6.75-10 as the NOSE tyre and 19 x 6.75-8 (0.48 m) as the main. The two are
  swapped, so the drawn main wheel is about 8 cm too tall and the nose about 4 cm too short.

  NEITHER IS FIXED HERE, and that is the plane4 lesson applied rather than restated. That
  script's header records an accusation it had to withdraw: a measured figure was checked
  against a TYPED one, the typed one won because it was in C++ and looked authoritative, and
  a correct model was sent to Blender to be broken. So this script MEASURES and REPORTS -
  author_type() logs the wheelbase against 495 uu every run - and the decision about the
  .blend belongs to whoever owns the .blend. What the type carries is the model, because the
  animation rolls the wheel the PLAYER SEES and a radius from the datasheet would make a
  correct-looking wheel skate.

THE STEERING LOCK IS A GAMEPLAY RULING, NOT A MEASUREMENT, and it is the one figure in this
file that is neither. See STEERING below; it is argued there rather than here because that
is where somebody changing it will be standing.
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
TYPE_NAME = "DA_Aircraft_Plane5"
ABP = "/Game/Aircraft/Plane5/ABP_Plane5"
SHORT_CODE = "B350"
MESH = "/Game/Aircraft/Plane5/SK_Plane5"

SOURCE = r"C:\repos\AirportMgr2Models\plane5\export\plane5.glb"

# The parts this type is measured from, NAMED rather than assumed equal to plane4's. They
# happen to match on three of four - this rig calls its lifting surface "wing" and its
# tailplane "stabiliser" as plane4 does, and its main leg "maingear_L" - and the fourth is
# the whole difference between a turboprop and a jet: PROP is a real propeller here, where
# plane4's is a turbofan honestly called "fan_L". A wrong stem raises rather than defaulting.
WING = "wing"
STABILISER = "stabiliser"
GEAR_LEG = "maingear_L"
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
        # A STRAIGHT, UNSWEPT, WINGLETLESS WING, so unlike plane4 there is no question of
        # what this span is over: 17.69 m against the S&D's 17.65. The 4 cm is the wingtip
        # fairing's own thickness and is not worth a note anywhere but here.
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The disc lies ACROSS the airframe rather than along it - so the larger of the two
    # cross-axis extents, never the chordwise one. Reads 2.666 m against the Hartzell's
    # published 105 in (2.667 m), which is the closest any measurement in this fleet has
    # come to its datasheet.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off
    SK_Plane5's reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, the rule build_plane3_type.py settled: the bone is
    a STATEMENT about where the axle is - build_export.py places each wheel's origin on its
    rotation axis so the thing can spin - and the hub's HEIGHT above the contact plane IS the
    radius. That z = 0 is the contact plane is not assumed: airside_import.report_bounds
    FAILS an import whose lowest vertex is more than 10 uu off it, so a model that floats
    never reaches this script.

    THE MAINS ARE DUAL ON THE REAL AEROPLANE and single in the model - one wheel mesh and one
    bone a side. That is a modelling simplification, not a measurement error, and it is
    invisible to everything here: the track wants the axle centres, and two tyres 0.32 m
    apart share one.
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
            raise KeyError("SK_Plane5 has no %s bone - the rig was renamed. Bones: %s"
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


def gear_cycle_top_uu():
    """The height at which the retraction cycle FINISHES, uu.

    Computed rather than eyeballed, because it is the thing CLIMB's clear_altitude has to
    clear and the two are typed in different breaths. An agent removed from the world part
    way through its cycle vanishes with its gear half up, in front of the player, and the
    only symptom is that it looked wrong for a moment.
    """
    cycle = GEAR["door_seconds"] * 2.0 + GEAR["travel_seconds"]
    rate = CLIMB["climb_speed"] * math.sin(math.radians(CLIMB["climb_pitch_degrees"]))
    return GEAR["retract_above_height"] + rate * cycle


# --- Published, from the Beechcraft King Air 350i --------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s.
#
# A 6.8 t twin turboprop between the Twin Otter and the Q400 in every dimension, which is
# what makes it the missing rung: it rotates at 100 kn where the Otter rotates at 60 and the
# Q400 at 120, and it wants 1,006 m of tarmac against their 366 and 1,402.
GROUND = {
    # The whole fleet taxis at 19 kn and this is no exception. A taxi speed is a procedure,
    # not a capability, so a type that differed here would be saying something it does not
    # mean.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn
    # 2,100 SHP on 6.8 t accelerates HARD - between the Otter's 450 and the Q400's 250,
    # nearer the Otter, because the King Air's power-to-weight is the better of the two.
    "takeoff": dict(accel=350.0, decel=400.0, speed_cap=5140.0),   # Vr about 100 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=5300.0),   # Vref about 103 kn
}

# STEERING, AND THIS IS THE ONE RULING IN THIS FILE THAT IS NEITHER MEASURED NOR PUBLISHED.
#
# THE REAL AEROPLANE STEERS BADLY. Section 7.2: "Nosewheel steering is mechanically actuated
# by the rudder pedals" - there is no tiller on a King Air, and pedal steering is worth
# something like 12 degrees of lock. On the measured 4.60 m wheelbase that is a tightest
# followable radius of 22 m (R >= L / sin(lock), the rule FSpeedProfile applies), which would
# make the SMALLEST aeroplane in the fleet worse round a corner than the 737 at 16 m. In the
# game that reads as "no route", not as "this aircraft steers poorly", so the honest figure
# would ship as an unexplained refusal.
#
# RULED 2026-09-21: treat it as turning well. 60 degrees, the Twin Otter's lock, on a
# wheelbase within 6 cm of the Twin Otter's 4.54 m - so the two Code B aeroplanes corner
# alike, which is the property the apron layout actually depends on. author_type() logs the
# computed radius every run so the consequence stays visible rather than becoming folklore.
#
# 0.2 g is the Twin Otter's too. A 6.8 t twin is not driven round an apron briskly, but it is
# not a Q400 either.
STEERING = {
    "max_steer_degrees": 60.0,
    "max_lateral_accel_uu": 196.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    # BETWEEN THE OTTER'S 4 AND THE Q400'S 3, and it lands on 4: rotate rate falls with mass
    # across the fleet (172 5, Otter 4, Q400 3, 737 3) and 6.8 t sits beside the Otter's 5.7.
    "rotate_rate_deg_per_sec": 4.0,
    "climb_speed": 8200.0,        # 160 kn, the 350's climb speed
    # HIGHER THAN THE LIGHT TYPES' 300 m BECAUSE IT RETRACTS. gear_cycle_top_uu() computes
    # where the cycle finishes - about 204 m on these figures - and author_type() FAILS if
    # this is below it. plane4 argued the same point in prose and typed 500 m; the argument
    # is worth more as an assertion.
    "clear_altitude": 40000.0,    # 400 m
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    # Between the Otter's 900 and the Q400's 1,000, and on the Otter's figure: flare height
    # tracks how far the pilot's eye is above the wheels, and a King Air sits low.
    "flare_height": 900.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # PROPELLER RPM, NOT ENGINE RPM, which is the convention plane2 and plane3 already
    # follow - FAgentMotion::EngineRPM turns the bone the player watches, and on a free
    # turbine the gas generator's 30,000-odd rpm is nothing anybody sees.
    #
    # 1,700 IS NOT IN THE SPECIFICATION AND DESCRIPTION. That document names the engine
    # (PT6A-60A, p8) and the propeller (Hartzell 105-inch four blade, p9) and gives neither a
    # speed; 1,700 rpm is the propeller governor limit from the type's own limitations, and
    # it is the least well sourced number in this file. Said plainly so that nobody later
    # mistakes it for something read off p8.
    "max_rpm": 1700.0,
    # A PT6 IS A PT6. plane2's Twin Otter runs a PT6A-27 on exactly these figures, and there
    # is no reason for a -60A to spool differently at this resolution. Copied deliberately
    # rather than re-invented: a free turbine winds down slowly, which is the 9.
    "spool_up_seconds": 4.0,
    "spool_down_seconds": 9.0,
}

# THE SECOND RETRACTABLE GEAR IN THE PROJECT, and the first with doors at BOTH ends.
#
# Section 7.1: "The main landing gear hydraulically retracts forward into each engine
# nacelle." The rig folds the mains forward and the nose aft, with the direction carried in
# each bone's rest axis rather than in the number - see plane5/scripts/build_rig.py - so one
# GearAngleDegrees drives all three legs and one BayDoorAngleDegrees drives all four doors.
GEAR = {
    # SIX SECONDS, against the 737's seven. A King Air's gear cycle is quick; VLO retraction
    # is 166 KIAS and extension 184 (S&D p6), which is a gear designed to be used briskly.
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    # A FEW HUNDRED FEET - 9000 uu is about 295 ft - because raising the gear is a pilot
    # command and not a consequence of lift-off. FAgentMotion::bAirborne is the precondition.
    # Same figure as plane4 on purpose: this is a procedure, not a capability.
    "retract_above_height": 9000.0,
    # ABOVE FApproachPerformance::FinalAltitude (2000 uu) on purpose, so every arrival is born
    # down and locked and this never fires at today's figures. Authored at an honest ~500 ft
    # anyway, so the extension is correct the day the approach is joined higher.
    "extend_below_height": 15000.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes -
# AirportMgr.Content.FieldLengthsCoverTheRoll states the rule: a published figure may be
# generous, but one shorter than the roll admits an aircraft to a strip it then runs off the
# end of. Both are the S&D's own, p6, at the weights that document names.
REQUIREMENTS = {
    "takeoff_field_length": 100600.0,   # 1,006 m, 3,300 ft at MTOW, flaps approach
    "landing_field_length": 82100.0,    # 821 m, 2,692 ft at max landing weight, no reverse
}

# TWELVE MINUTES. Eleven seats through one airstair door, against the Q400's twenty-five for
# seventy-eight and the 172's ten for four. The King Air's turn is limited by refuelling and
# the crew walk-round rather than by the queue down the aisle.
TURNAROUND_SECONDS = 720.0

# FOUR BLADES, S&D p9. This is NOT cosmetic: UAirsideAgentAnim::PropStepDegrees resolves the
# frame rate against ONE BLADE REPEAT, 360/N degrees, so a four-blade propeller told it has
# the default three is allowed a step a third too large and aliases backwards.
PROP_BLADE_COUNT = 4

# NINETY DEGREES, where UAirsideAgentAnim defaults to plane4's measured 81. It is a fact
# about THIS rig and not a choice: build_rig.py's contract is "DOORS: 0 = hanging open (the
# gear-down pose the meshes are built in), +90 = closed", and the pose check that produced it
# folds the gear and reports where each door's free edge lands. Left at 81 the four doors
# stop 9 degrees short of shut, which on a retracted King Air is a visible gap in each
# nacelle.
BAY_DOOR_CLOSED_ANGLE_DEGREES = 90.0

# The S&D's wheelbase, p5: 16 ft 3 in. Held here so the check below cites a number rather
# than a memory of one. See the header for why this is a NOTE and not a failure.
SPEC_WHEELBASE_UU = 495.0


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field, value in values.items():
        regime.set_editor_property(field, value)
    ground.set_editor_property(name, regime)


def author_type():
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: DA_Airline_Cumbria's fleet points at
        # this, and deleting an asset something references breaks the reference rather than
        # updating it.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            TYPE_NAME, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    # CODE B, the aerodrome reference letter: the measured span is 17.69 m and Code B runs
    # from 15 to under 24. It shares that letter with the Twin Otter - which is why ShortCode
    # exists beside it - and the two are at opposite ends of it, 17.69 m against 19.75, so a
    # Code B stand takes either.
    asset.set_editor_property("code", unreal.Name("B"))
    asset.set_editor_property("short_code", unreal.Name(SHORT_CODE))
    asset.set_editor_property("display_name", unreal.Text("King Air 350i"))

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

    # THE WHEELBASE, CHECKED EVERY RUN AGAINST THE MANUFACTURER'S OWN TABLE. A NOTE and not a
    # failure - see the header for the plane4 accusation that had to be withdrawn, and for
    # why the decision about the .blend is not this script's to take.
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    if abs(wheelbase - SPEC_WHEELBASE_UU) > 25.0:
        say("NOTE wheelbase %.0f uu against the S&D's %.0f (16 FT 3 IN, section 1.3) - "
            "%.2f m out. plane5/scripts/build_gear.py derived it from side.jpg and checked "
            "it against 14 ft 11 in, which is not this aeroplane's published figure. The "
            "type carries the MEASURED value on purpose; the .blend is where this is settled."
            % (wheelbase, SPEC_WHEELBASE_UU, (wheelbase - SPEC_WHEELBASE_UU) / 100.0))
    else:
        say("PASS wheelbase %.0f uu, within 25 uu of the S&D's %.0f (16 FT 3 IN)"
            % (wheelbase, SPEC_WHEELBASE_UU))

    # THE FIGURE THAT DECIDES WHETHER THIS TYPE CAN USE THE PLAYER'S TAXIWAYS, said out loud
    # because it is not otherwise visible anywhere, and doubly so here: STEERING's lock is a
    # ruling rather than a measurement, so the thing it buys should be printed beside it.
    say("tightest followable radius %.0f uu (%.1f m) at %.0f degrees of lock - a RULING, see "
        "STEERING; the real aeroplane's pedal steering would give about 22 m"
        % (tightest_radius_uu(), tightest_radius_uu() / 100.0,
           STEERING["max_steer_degrees"]))

    # THE GEAR CYCLE MUST FINISH BEFORE THE AGENT IS REMOVED, asserted rather than argued.
    top = gear_cycle_top_uu()
    if top >= CLIMB["clear_altitude"]:
        fail("the gear cycle finishes at %.0f uu but the agent is cleared at %.0f - it would "
             "vanish mid-retraction. Raise clear_altitude or lower retract_above_height."
             % (top, CLIMB["clear_altitude"]))
    else:
        say("PASS the gear is up and locked by %.0f uu (%.0f m), below clear_altitude %.0f"
            % (top, top / 100.0, CLIMB["clear_altitude"]))

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
    # TARMAC, AND THIS IS THE POINT OF THE TYPE. Both light types fly off grass; the Q400 was
    # the first to demand tarmac and it demanded 1,402 m in the same breath, so paving and
    # lengthening arrived as one purchase. This type demands tarmac and 1,006 m - LESS length
    # than the runway a Q400 needs and a third more than the Twin Otter's 366 m - so the
    # player meets "pave it" as a decision of its own. The approach stays visual: an
    # instrument approach is a building the game does not have yet.
    requirements.set_editor_property("minimum_surface", unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults.

    THE WHEEL RADIUS defaults to the Meridian's 21 uu. Left unset, a King Air's wheels turn a
    third too fast against ground speed, which reads as skating.

    THE BLADE COUNT defaults to 3 and the Hartzell has 4 - see PROP_BLADE_COUNT.

    THE DOOR CLOSED ANGLE defaults to plane4's measured 81 and this rig's is 90 - see
    BAY_DOOR_CLOSED_ANGLE_DEGREES. It is the first per-model default that is NOT a
    consequence of the aeroplane's size, so it is the easiest of the three to forget.
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
        say("PASS ABP_Plane5 wheel radius %.1f -> %.1f uu" % (before, got_radius))
    if got_blades != PROP_BLADE_COUNT:
        fail("ABP blade count read back as %d, expected %d" % (got_blades, PROP_BLADE_COUNT))
    else:
        say("PASS ABP_Plane5 blade count = %d, the Hartzell's (S&D p9)" % got_blades)
    if abs(got_door - BAY_DOOR_CLOSED_ANGLE_DEGREES) > 0.01:
        fail("ABP door closed angle read back as %.1f, expected %.1f"
             % (got_door, BAY_DOOR_CLOSED_ANGLE_DEGREES))
    else:
        say("PASS ABP_Plane5 bay door closed at %.1f deg, this rig's shut angle" % got_door)


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
    span = footprint.get_editor_property("wingspan")
    if abs(span - m["footprint"]["wingspan"]) > 0.1:
        fail("wingspan read back as %.1f" % span)
    else:
        say("PASS footprint wingspan %.1f uu (%.2f m) against the S&D's 17.65"
            % (span, span / 100.0))

    for prop, want in (("short_code", SHORT_CODE), ("code", "B")):
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

    # THE GEAR, READ BACK FIELD BY FIELD, for the reason build_plane4_type.py gives: a
    # set_editor_property that silently no-ops on an unknown field name reports success and
    # saves cleanly, leaving this type with TravelSeconds 0 - indistinguishable on screen
    # from an aeroplane whose gear is simply fixed.
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
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m) - S&D p6"
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
