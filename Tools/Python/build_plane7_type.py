"""Renames DA_Aircraft_Piper to DA_Aircraft_Plane7, points it at the new mesh and Anim
Blueprint, and authors what only the export can answer. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

THIS SCRIPT TYPES NO PERFORMANCE FIGURE, AND THAT IS THE DIFFERENCE BETWEEN IT AND ITS FIVE
SIBLINGS. build_plane1_type.py through build_plane5_type.py each carry their aeroplane's
published numbers, because those types exist nowhere else. The Meridian is not like that: it
is ALSO UAirsideSettings::ResolveDefaultAirframe's fallback, so its figures live in C++ -
UAircraftType::BuildPiperMeridian and the PiperMeridian*() statics beside it - and a second
copy here is precisely the failure AirsideContent.h names in as many words: "#30 was seven
call sites hardcoding a Piper's numbers while the MESH already came from here".

So this calls build_piper_meridian() to lay the type down from the one description, exactly
as build_stand_asset.py did when the asset was first created, and then sets only:

  * THE MESH AND THE ANIM CLASS. Content, which Airside may not name - Check-Architecture
    enforces the include direction. See build_aircraft_looks.py's header for the full note.
  * THE FIGURES FAircraft's C++ STRUCT HAS NO FIELD FOR - main gear track and propeller
    diameter - MEASURED off the export.
  * THE GEAR CYCLE, which is a new capability rather than a new number: see GEAR below.

OWNERSHIP MOVED HERE FROM build_stand_asset.py, which authored DA_Aircraft_Piper as one of
three aircraft it needed to size a stand. Its reason for the asset existing at all is worth
carrying rather than losing: M_Starter's runway is 15 m wide and admits a 15 m wingspan, so
neither Code C type can ever land there and every offer was refused at the gate. A light type
an airline can actually list is what gives a GA field traffic - and what makes the jets a
REASON to build a wider runway rather than a broken inbox.

THE RENAME, NOT A NEW ASSET. DA_Airline_Cumbria's fleet and DA_AirsideContent's
DefaultAircraft both point at this object. A create-and-delete would break both references
rather than update them; a rename fixes them up by construction and is what
EditorAssetLibrary.rename_asset does. It runs once and is a no-op on every later run.

WHAT THIS DOES NOT TOUCH. TurnaroundSeconds and PushbackNeed are authored on the existing
asset and are decisions about the AEROPLANE, not about the model that replaced its mesh.
Re-deciding them in the same breath as an import would be exactly the scope creep that hides
a real change inside a mechanical one. They are READ BACK and reported, so a reader can see
what survived.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. Put it on before importing the shared reader.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"
OLD_NAME = "DA_Aircraft_Piper"
TYPE_NAME = "DA_Aircraft_Plane7"
ABP = "/Game/Aircraft/Plane7/ABP_Plane7"
MESH = "/Game/Aircraft/Plane7/SK_Plane7"

SOURCE = r"C:\repos\AirportMgr2Models\plane7\export\plane7.glb"

# The parts this type is measured from, NAMED rather than assumed equal to plane5's. Two of
# the four differ and a wrong stem raises rather than defaulting: this rig calls its tailplane
# `tailplane` where plane5 calls it `stabiliser`, and its single propeller `prop` where a twin
# has `prop_L`. The main leg is `maingear_L` on both.
WING = "wing"
STABILISER = "tailplane"
GEAR_LEG = "maingear_L"
PROP = "prop"

# THE GEAR CYCLE - A NEW CAPABILITY, NOT A NEW NUMBER.
#
# SM_PiperMeridian had no retract bones at all, so this type has shipped with an empty Gear
# block: a Meridian's gear stayed down through the whole climb, on screen, because there was
# nothing in the rig to fold. plane7 retracts and has doors at both ends, so the block can be
# authored for the first time - and UAircraftType::BuildPiperMeridian deliberately does NOT
# author it, for the reason Airside.Model.Gear737IsAuthoredAndTravels pins in both directions:
# the ASSET declares a cycle and the C++ builder does not, so an edit that "completes" the
# builder by copying these figures back goes red.
#
# SIX SECONDS AND ONE, plane5's figures and for plane5's reason. A PA-46's gear is
# hydraulically actuated and quick; VLO is 168 KIAS, which is a gear designed to be used
# briskly. Copying a fleet-mate's procedure timings is right where nothing distinguishes the
# two aeroplanes - the alternative is inventing a difference to avoid looking like a copy.
GEAR = {
    "travel_seconds": 6.0,
    "door_seconds": 1.0,
    # A FEW HUNDRED FEET - 9000 uu is about 295 ft - because raising the gear is a pilot
    # command and not a consequence of lift-off. FAgentMotion::bAirborne is the precondition.
    # The same figure as plane4 and plane5 on purpose: this is a procedure, not a capability.
    "retract_above_height": 9000.0,
    # ABOVE FApproachPerformance::FinalAltitude on purpose, so every arrival is born down and
    # locked and this never fires at today's figures. Authored at an honest ~500 ft anyway, so
    # the extension is correct the day the approach is joined higher.
    "extend_below_height": 15000.0,
}

# FOUR BLADES - a Hartzell HC-E4N-3Q, TCDS A25SO, and what plane7/scripts/build_prop.py
# arrays. This is NOT cosmetic: UAirsideAgentAnim::PropStepDegrees resolves the frame rate
# against ONE BLADE REPEAT, 360/N degrees, so a four-blade propeller told it has the default
# three is allowed a step a third too large and aliases backwards.
PROP_BLADE_COUNT = 4

# NINETY DEGREES, where UAirsideAgentAnim defaults to plane4's measured 81. It is a fact about
# THIS rig and not a choice: plane7/scripts/build_rig.py's CYCLE keys the doors at 0 (hanging
# open, the pose the meshes are built in) and 90 (closed), and its pose_check() folds the gear
# and reports where each door's free edge lands. Left at 81 the four doors stop 9 degrees short
# of shut, which on a retracted aeroplane is a visible gap at each bay.
BAY_DOOR_CLOSED_ANGLE_DEGREES = 90.0


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
        # A STRAIGHT, UNSWEPT, WINGLETLESS WING, so there is no question of what this span is
        # over: 13.110 m, which build_wing.py matches to the published 43 ft 0 in BY
        # CONSTRUCTION rather than by coincidence. It is the one dimension on this aeroplane
        # that is exact, and it is the one stands and gates are sized from.
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    # The disc lies ACROSS the airframe rather than along it - so the larger of the two
    # cross-axis extents, never the chordwise one. Reads 2.096 m, which is the TCDS maximum of
    # 82.5 in exactly; the download's own disc is 2.170 and 3.5% over it. build_prop.py records
    # why measuring a propeller off a Y-slice reads 1.814 for that same disc.
    prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_hi[2] - leg_lo[2]


def axles_and_radius(leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off SK_Plane7's
    reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, the rule build_plane3_type.py settled: the bone is
    a STATEMENT about where the axle is - build_export.py places each wheel's origin on its
    rotation axis so the thing can spin - and the hub's HEIGHT above the contact plane IS the
    radius. That z = 0 is the contact plane is not assumed: airside_import.report_bounds FAILS
    an import whose lowest vertex is more than 10 uu off it, so a model that floats never
    reaches this script.

    THESE ARE THE FIGURES BuildPiperMeridian NOW TYPES, and that is the point of measuring them
    again here rather than trusting them: the C++ carries 0.0 and -237.8 so that the fallback
    and the tests have a footprint with no content loaded, and this is what proves the two
    still describe one aeroplane. verify() fails on a disagreement.
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
            raise KeyError("SK_Plane7 has no %s bone - the rig was renamed. Bones: %s"
                           % (wanted, ", ".join(sorted(at))))

    mains_x = (at["wheel_L"].x + at["wheel_R"].x) * 0.5
    radius = (at["wheel_L"].z + at["wheel_R"].z) * 0.5

    # THE TRACK, MEASURED, for the same reason the axles are: it is a fact about the model that
    # is drawn, not about the aeroplane in the datasheet. Anything hung off it - tyre smoke at
    # touchdown, a tug lining up - lands under the wheels the player can see.
    track = abs(at["wheel_L"].y - at["wheel_R"].y)
    if track <= radius:
        raise ValueError("wheel_L and wheel_R are %.1f uu apart, which is inside one wheel's "
                         "%.1f uu radius - the two main gear bones are on top of each other"
                         % (track, radius))

    # A RADIUS MAY NOT EXCEED THE LEG THAT CARRIES IT, and it may not be nothing. The two
    # bounds catch the same failure from opposite sides: wheel bones left at the root read as
    # radius 0, and a model exported in metres reads as a wheel taller than the aeroplane.
    if not 5.0 < radius < leg_height:
        raise ValueError("the main-gear hub sits at z=%.1f against a %.1f uu leg - the bone is "
                         "not on the axle, or the model is not on the ground plane"
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


def rename_or_load():
    """The type asset, renamed from DA_Aircraft_Piper on the first run. None on failure.

    ORDER MATTERS AND IS ASSERTED. If both names exist something has already gone wrong -
    most likely a half-run that created a new asset instead of renaming - and carrying on
    would leave DA_Airline_Cumbria pointing at whichever one this script did not touch.
    """
    new_path = "%s/%s" % (TYPE_PATH, TYPE_NAME)
    old_path = "%s/%s" % (TYPE_PATH, OLD_NAME)
    has_new = unreal.EditorAssetLibrary.does_asset_exist(new_path)
    has_old = unreal.EditorAssetLibrary.does_asset_exist(old_path)

    if has_new and has_old:
        fail("both %s and %s exist. One of them is unreferenced and this script cannot tell "
             "which - delete the wrong one by hand and re-run." % (old_path, new_path))
        return None

    if has_old:
        if not unreal.EditorAssetLibrary.rename_asset(old_path, new_path):
            fail("could not rename %s to %s" % (old_path, new_path))
            return None
        say("renamed %s -> %s; DA_Airline_Cumbria's fleet and DA_AirsideContent's "
            "DefaultAircraft follow the object" % (old_path, new_path))
    elif not has_new:
        # Load-or-create, never delete-and-recreate - see the header. A fresh checkout that
        # never had the Piper asset still gets a type.
        if unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                TYPE_NAME, TYPE_PATH, unreal.AircraftType, None) is None:
            fail("could not create %s" % new_path)
            return None
        say("created %s - there was no %s to rename" % (new_path, old_path))
    else:
        say("%s is already in place; nothing to rename" % new_path)

    return unreal.EditorAssetLibrary.load_asset(new_path)


def author_type():
    asset = rename_or_load()
    if asset is None:
        return None
    path = "%s/%s" % (TYPE_PATH, TYPE_NAME)

    # THE ONE DESCRIPTION. Code, ShortCode, DisplayName, the footprint, both axles, the wheel
    # radius and every performance block come from here and from nowhere else in this file.
    unreal.AircraftType.build_piper_meridian(asset)
    say("laid the type down from UAircraftType::BuildPiperMeridian - code %s, short code %s, "
        "'%s'" % (asset.get_editor_property("code"), asset.get_editor_property("short_code"),
                  asset.get_editor_property("display_name")))

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
        "track %.1f" % (m["steer_axle_x"], m["fixed_axle_x"],
                        abs(m["fixed_axle_x"] - m["steer_axle_x"]), m["main_gear_track"]))

    # THE TWO FIELDS THE C++ BUILDER HAS NO PLACE FOR. Everything else on this asset came from
    # it; these are measured because nothing else can answer them.
    asset.set_editor_property("main_gear_track", m["main_gear_track"])
    asset.set_editor_property("propeller_diameter", m["prop_diameter"])

    gear = asset.get_editor_property("gear")
    for field, value in GEAR.items():
        gear.set_editor_property(field, value)
    asset.set_editor_property("gear", gear)

    # THE GEAR CYCLE MUST FINISH BEFORE THE AGENT IS REMOVED, asserted rather than argued -
    # and against the climb figures the C++ just wrote, not against a copy of them. An agent
    # removed from the world part way through its cycle vanishes with its gear half up, in
    # front of the player, and the only symptom is that it looked wrong for a moment.
    climb = asset.get_editor_property("climb")
    cycle = GEAR["door_seconds"] * 2.0 + GEAR["travel_seconds"]
    rate = climb.get_editor_property("climb_speed") * unreal.MathLibrary.degrees_to_radians(
        climb.get_editor_property("climb_pitch_degrees"))
    top = GEAR["retract_above_height"] + rate * cycle
    clear = climb.get_editor_property("clear_altitude")
    if top >= clear:
        fail("the gear cycle finishes at about %.0f uu but the agent is cleared at %.0f - it "
             "would vanish mid-retraction. Raise PiperMeridianClimb's ClearAltitude or lower "
             "retract_above_height." % (top, clear))
    else:
        say("PASS the gear is up and locked by about %.0f uu (%.0f m), below the clear "
            "altitude %.0f that PiperMeridianClimb authors" % (top, top / 100.0, clear))

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def set_anim_defaults():
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults.

    THE WHEEL RADIUS defaults to 21 uu, which was THIS AEROPLANE'S OWN placeholder figure -
    SM_PiperMeridian's 0.420 m mains. plane7 builds the published 6.00-6 instead, so the
    default is now wrong for the one type it was measured from, and left unset the wheels turn
    about 6% fast against ground speed.

    THE BLADE COUNT defaults to 3 and the Hartzell has 4 - see PROP_BLADE_COUNT.

    THE DOOR CLOSED ANGLE defaults to plane4's measured 81 and this rig's is 90 - see
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
        say("PASS ABP_Plane7 wheel radius %.1f -> %.1f uu" % (before, got_radius))
    if got_blades != PROP_BLADE_COUNT:
        fail("ABP blade count read back as %d, expected %d" % (got_blades, PROP_BLADE_COUNT))
    else:
        say("PASS ABP_Plane7 blade count = %d, the Hartzell HC-E4N-3Q's" % got_blades)
    if abs(got_door - BAY_DOOR_CLOSED_ANGLE_DEGREES) > 0.01:
        fail("ABP door closed angle read back as %.1f, expected %.1f"
             % (got_door, BAY_DOOR_CLOSED_ANGLE_DEGREES))
    else:
        say("PASS ABP_Plane7 bay door closed at %.1f deg, this rig's shut angle" % got_door)


def verify(path):
    """Read back from disk. An asset edit that reports success and writes nothing is this
    project's most familiar failure."""
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("%s did not survive the save" % path)
        return

    m = measured()

    # THE C++ AGAINST THE MESH, which is the check this whole script exists to make possible.
    # BuildPiperMeridian types these four; the rig answers them independently. A disagreement
    # means the export moved and the builder was not re-measured - the half-run that
    # Airside.Content.FootprintMatchesTheMesh catches for the other five types, caught here at
    # authoring time as well because for THIS type the figures are in code.
    footprint = asset.get_editor_property("footprint")
    typed = [
        ("footprint.nose_x", footprint.get_editor_property("nose_x"),
         m["footprint"]["nose_x"]),
        ("footprint.tail_x", footprint.get_editor_property("tail_x"),
         m["footprint"]["tail_x"]),
        ("footprint.wingspan", footprint.get_editor_property("wingspan"),
         m["footprint"]["wingspan"]),
        ("footprint.wing_x", footprint.get_editor_property("wing_x"),
         m["footprint"]["wing_x"]),
        ("footprint.tailplane_span", footprint.get_editor_property("tailplane_span"),
         m["footprint"]["tailplane_span"]),
        ("footprint.tailplane_x", footprint.get_editor_property("tailplane_x"),
         m["footprint"]["tailplane_x"]),
        ("steer_axle_x", asset.get_editor_property("steer_axle_x"), m["steer_axle_x"]),
        ("fixed_axle_x", asset.get_editor_property("fixed_axle_x"), m["fixed_axle_x"]),
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"),
         m["wheel_radius"]),
    ]
    for name, got, want in typed:
        if abs(got - want) > 0.5:
            fail("%s is %.1f in BuildPiperMeridian and %.1f on the mesh - the C++ was not "
                 "re-measured after the export moved" % (name, got, want))
        else:
            say("PASS %-24s = %8.1f, and the mesh agrees to %.2f uu"
                % (name, got, abs(got - want)))

    measured_only = [
        ("main_gear_track", asset.get_editor_property("main_gear_track"),
         m["main_gear_track"]),
        ("propeller_diameter", asset.get_editor_property("propeller_diameter"),
         m["prop_diameter"]),
    ]
    for name, got, want in measured_only:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %-24s = %8.1f" % (name, got))

    for prop in ("mesh", "anim_class"):
        got = asset.get_editor_property(prop)
        if got is None:
            fail("%s is unset - the agent would fall back to the game-wide default mesh, "
                 "which is how a Twin Otter arrived as a Meridian" % prop)
        else:
            say("PASS %s = %s" % (prop, got.get_name()))

    # THE GEAR, READ BACK FIELD BY FIELD, for the reason build_plane4_type.py gives: a
    # set_editor_property that silently no-ops on an unknown field name reports success and
    # saves cleanly, leaving this type with TravelSeconds 0 - indistinguishable on screen from
    # an aeroplane whose gear is simply fixed, which is what this one used to be.
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

    # NOT AUTHORED HERE - see the header. Reported so a reader can see what the rename carried
    # through rather than having to open the asset to find out.
    say("carried through the rename: turnaround %.0f s, pushback %s"
        % (asset.get_editor_property("turnaround_seconds"),
           asset.get_editor_property("pushback_need")))

    requirements = asset.get_editor_property("requirements")
    say("from the C++: field lengths take-off %.0f uu, landing %.0f uu; surface %s, "
        "approach %s"
        % (requirements.get_editor_property("takeoff_field_length"),
           requirements.get_editor_property("landing_field_length"),
           requirements.get_editor_property("minimum_surface"),
           requirements.get_editor_property("approach_needed")))


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
