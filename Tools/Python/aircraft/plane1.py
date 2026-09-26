"""DA_Aircraft_Plane1 / ABP_Plane1 - the Cessna 172S Skyhawk SP.

GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED, as every aircraft in this package states it:
a figure typed here is a second opinion about an object the export already answers for, and it
does not move when the model does. The measuring half is shared - build_aircraft_type.measure()
and axles_and_radius(), off airside_import.part_bounds_uu - so no two specs can disagree about
which glTF axis is UE's span.

WHAT THIS TYPE IS FOR. It is the SMALLEST AEROPLANE IN THE GAME, and that is the whole entry:
11.00 m of span against the Meridian's 13.11 and the Twin Otter's 19.75, on a 1.63 m wheelbase
that will follow a 3.3 m corner. Nothing else in the fleet fits where this fits.

IT IS NOT THE SHORTEST-FIELD TYPE, and that surprise is worth writing down because it reverses
the obvious guess. A 172S needs 497 m over a 50 ft obstacle; plane2's Twin Otter needs 366 m. A
DHC-6 genuinely out-STOLs a Cessna 172 - it is a STOL aeroplane and this is a trainer - so the
ladder this type joins is the SPAN ladder, not the length one. Its published length lands within
3% of the Meridian's 510 m, so on runway length alone the two are interchangeable and the 172
offers wherever the Meridian does. That overlap is deliberate: the fleet gains variety here
rather than a new capability tier.

THE GEAR IS FIXED, so this type declares no gear cycle at all (`gear=None` below). A 172 has
nothing to retract and its rig has no gear or door bones to drive - see
docs/superpowers/specs/2026-09-19-gear-retraction-design.md for what a type that DOES retract
carries. plane4 remains the only retracting type, which is why DefaultGame.ini's Land key still
points at it.

THE FIGURES AGREE WITH THE REAL AEROPLANE TO WITHIN 5 CM, which is unusual enough to record
rather than assume will stay true. Cessna 172S Skyhawk SP against this export:

    length     8.28 m      measured 8.233
    span      11.00 m      measured 11.000
    height     2.72 m      measured 2.659   (see below)
    wheelbase  1.63 m      measured 1.630
    track      2.51 m      measured 2.540
    propeller  1.905 m     measured 1.919   (76 in McCauley, 2 blades)

HEIGHT READ 2.720 UNTIL 2026-09-19 AND THE CHANGE IS NOT A REGRESSION. The highest vertex in the
export was the tail beacon, at 272.0 uu; the FIN tip is at 265.9. The beacon and the two other
light meshes were removed from the export, so the figure now measures the aeroplane rather than
a lamp on top of it - which makes the 2.72 m agreement it used to show a coincidence worth not
having trusted. Nothing reads height: the footprint carries nose_x, tail_x and the two spans,
and the aerodrome code letter is decided by span and gear track.

The two wheel RADII are the loose ones - 0.198 m measured against a 6.00-6's 0.222, and 0.169
against a 5.00-5's 0.180 - and they are left alone. They are the figures the animation spins and
the tyre smoke sits on, so they must describe the model that is drawn; a published radius would
be right about the aeroplane and wrong about SK_Plane1.

THE RIG WAS FIXED AT THE EXPORT RATHER THAN WORKED AROUND HERE, three times on 2026-09-19 - one
`wheel` bone driving both mains from the right wheel's outer face, a `nosewheel` bone 12 uu off
the centreline, and the wing and tailplane merged into `fuselage` with nothing for the footprint
to measure. import_models.py's Spec carries the detail. The alternative each time was a special
case in this file, and a special case outlives the model it was written for.

`maingear` IS BOTH LEGS IN ONE OBJECT, and that is fine here: the only thing taken from it is
its HEIGHT, as the sanity bound on the wheel radius. A one-object leg would be wrong for an AXLE
- a leg's centre is not an axle - and nothing here asks it for one. It was called `wheelstrut`
until 2026-09-19, a name in the tyre namespace for a part that is not a tyre.
"""
from build_aircraft_type import AircraftSpec, say_published_table, say_tightest_radius

TYPE_NAME = "DA_Aircraft_Plane1"
ABP = "/Game/Aircraft/Plane1/ABP_Plane1"
MESH = "/Game/Aircraft/Plane1/SK_Plane1"
SOURCE = r"C:\repos\AirportMgr2Models\plane1\export\plane1.glb"

# --- Published, from the Cessna 172S Skyhawk SP POH ------------------------------------
#
# Speeds are uu per second: 100 uu/s is 1 m/s, and a knot is 51.4 uu/s. All figures are at
# 2,550 lb, sea level, ISA, no wind - POH section 5, the same conditions the other three types
# in the fleet are published at.
#
# A 1,157 kg piston single with 180 hp, and every figure below is what separates it from the
# lightest thing already in the game. It rotates at 55 kn where plane2 rotates at 60 and plane3
# at 120, and climbs out at 74 against 85 and 170 - so it leaves the ground sooner and slower
# than anything else, and then goes nowhere in a hurry.
GROUND = {
    # Taxi as the others do: a 172 does not taxi faster than a Twin Otter. 19 kn is the whole
    # fleet's figure and a difference here would be a difference nobody asked for.
    "taxi":    dict(accel=100.0, decel=200.0, speed_cap=1000.0),   # 19 kn, as the rest
    # 140 uu/s^2 IS CHOSEN TO REPRODUCE THE PUBLISHED GROUND ROLL, not guessed: Vr^2 / 2a with
    # Vr = 2827 gives 285 m against the POH's 293 m, and FTakeoffRun adds the rotation on top to
    # reach about 332 m. Compare plane2's 450 - a Twin Otter accelerates more than three times
    # as hard as a trainer, which is most of why it out-STOLs one.
    "takeoff": dict(accel=140.0, decel=400.0, speed_cap=2827.0),   # Vr 55 kn
    "landing": dict(accel=100.0, decel=400.0, speed_cap=3135.0),   # Vref 61 kn, flaps 30
}

# STEERING, published rather than measured - nothing in the mesh knows how far the nosewheel
# turns or how hard a pilot will corner.
#
# 30 DEGREES, THE LOWEST LOCK IN THE GAME, PRODUCING THE TIGHTEST CORNER IN THE GAME, which is
# only a contradiction if the wheelbase is forgotten. A 172 has no tiller: the nosewheel steers
# about 10 degrees off the rudder pedals and the rest comes from differential braking, so 30 is
# an effective authority rather than a mechanical limit. On the measured 1.63 m wheelbase that
# still makes R >= L / sin(lock) about 3.3 m - against plane2's 5.2 and plane3's 15.0. There is
# no taxiway this type can be refused for being unable to turn.
#
# 0.25 g is the light-single end of the range, shared with the Meridian - plane2 corners at 0.2
# and an airliner at 0.15. The lightest aeroplane on the apron is driven round it hardest.
STEERING = {
    "max_steer_degrees": 30.0,
    "max_lateral_accel_uu": 245.0,
}

CLIMB = {
    "lift_angle_at_rotate_degrees": 8.0,
    "climb_pitch_degrees": 10.0,
    # 5 degrees a second, the quickest in the fleet: a trainer's elevator is light and its tail
    # is a metre and a half behind the mains. plane2 rotates at 4, plane3 at 3.
    "rotate_rate_deg_per_sec": 5.0,
    "climb_speed": 3804.0,        # 74 kn, Vy at sea level
    # THE SAME CIRCUIT AS plane2, deliberately. Both are light aeroplanes flying a visual
    # circuit off the same field, and a 172 leaving it at a different height from a Twin Otter
    # would be a distinction with no rule behind it.
    "clear_altitude": 30000.0,
}

APPROACH = {
    "glideslope_degrees": 3.0,
    "final_altitude": 2000.0,
    # A 172 flares lower than anything else here - plane2 at 900, plane3 at 1000 - because the
    # flare height scales with how far the pilot's eye is above the mains, and on this aeroplane
    # that is under two metres.
    "flare_height": 800.0,
    "flare_rate_deg_per_sec": 3.0,
}

ENGINE = {
    # DIRECT DRIVE, WHICH IS WHY THIS IS 2700 AND NOT A REDUCTION OF IT. The Lycoming IO-360-
    # L2A turns the propeller itself - there is no gearbox - so engine redline and propeller rpm
    # are the same number. plane2's 2200 and plane3's 1020 are both PROP rpm behind a reduction
    # gear, which is the figure this field wants in every case.
    "max_rpm": 2700.0,
    # A piston single spools in about two seconds. The turboprops take 4 and 5.
    "spool_up_seconds": 2.0,
    "spool_down_seconds": 4.0,
}

# PUBLISHED FIELD LENGTHS, and they may never be shorter than the roll the model computes -
# Airside.Model.FieldLengthsCoverTheRoll states the rule: a published figure may be generous,
# but one shorter than the roll admits an aircraft to a strip it then runs off the end of.
#
# Both are the POH's TOTAL over a 50 ft obstacle rather than the ground roll. Taking the 172's
# 293 m ground roll instead would have made it the shortest-field type in the game, and it
# would have done so by measuring a different thing from every other type in the same field.
REQUIREMENTS = {
    "takeoff_field_length": 49700.0,   # 497 m, POH 1,630 ft over 50 ft
    "landing_field_length": 40700.0,   # 407 m, POH 1,335 ft over 50 ft
}

# Four seats and a door on each side. Ten minutes, the shortest in the game - plane2 turns in
# fifteen, plane3 in twenty-five, plane4 in forty.
TURNAROUND_SECONDS = 600.0

# TWO BLADES, COUNTED OFF THE EXPORT rather than taken from the datasheet: the prop_blade
# primitive's vertices fall in two lobes 180 degrees apart about the hub, with nothing in the
# four sectors between. UAirsideAgentAnim defaults to 3, and the blade count sets the largest
# per-frame step the animation may take before the disc aliases backwards.
PROP_BLADE_COUNT = 2

# THE MEASURED-AGAINST-PUBLISHED TABLE, PRINTED EVERY RUN, which is the habit plane4's own
# nose overhang check argued for: a disagreement between the model and the real aeroplane is
# worth SEEING each time rather than discovering. Nothing here compensates for one - the model
# is the authority for everything the game draws, and a fudge factor would outlive the fix.
PUBLISHED_TABLE = [
    ("span", lambda m: m["footprint"]["wingspan"], 11.00),
    ("length", lambda m: m["footprint"]["nose_x"] - m["footprint"]["tail_x"], 8.28),
    ("wheelbase", lambda m: abs(m["fixed_axle_x"] - m["steer_axle_x"]), 1.63),
    ("track", lambda m: m["main_gear_track"], 2.51),
    ("propeller", lambda m: m["prop_diameter"], 1.905),
    ("main wheel radius", lambda m: m["wheel_radius"], 0.222),
]


def _report(spec, m, say, fail):
    say_tightest_radius(spec, m, say)
    say_published_table(spec, m, say)


def _anim_report_extra(say):
    """plane1's OWN wiring finding, printed by build_aircraft_anim.py after the bone list -
    see AircraftSpec.wire_script and .anim_report_extra. Carried over from plane1's own
    retired build script's report_plan() verbatim; it is a fact about THIS aeroplane's shipped
    graph, not something the shared mechanism could derive for every hand-wired asset."""
    say("")
    say("  TWO BONES, NEVER BOTH ONTO ONE: the steer bone is the nose wheel's PARENT.")
    say("  ORDER IS DISPUTED AND THE ASSETS WIN. This line read 'wire nosewheel_steer")
    say("  BEFORE nosewheel' until 2026-09-20, when a graph authored to it diffed against")
    say("  ABP_Plane1 and ABP_Plane2 and found BOTH ship the reverse - steer LAST. The")
    say("  engine backs the assets: FCSPose::SafeSetCSBoneTransforms refreshes children")
    say("  already in component space, so steering the parent last carries the rolled")
    say("  wheel with it. Wire steer AFTER nosewheel unless someone rules otherwise.")
    say("")
    say("  THEN RUN build_aircraft_type.py plane1, which needs this asset's generated class")
    say("  to exist before it can point DA_Aircraft_Plane1 at it or set its wheel radius.")


def get_spec():
    return AircraftSpec(
        key="plane1",
        type_name=TYPE_NAME,
        short_code="C172",
        display_name="Cessna 172 Skyhawk",
        # CODE A, THE FIRST MEASURED ONE IN THE GAME, and it is the letter this type exists to
        # introduce. Code A is under 15 m and under 4.5 m of gear track, and this aeroplane
        # measures 11.00 and about 2.74. The Meridian is A as well, and is hand-built in C++.
        code="A",
        mesh=MESH,
        abp=ABP,
        source=SOURCE,
        wing="wing",
        stabiliser="stabiliser",
        gear_leg="maingear",
        prop="prop",
        # rig_line's trailing " uu" is this spec's own wording, kept rather than normalised to
        # the fleet's usual bare "%.1f" - the mechanism's DEFAULT_TIGHTEST_RADIUS_LINE and every
        # other say() line is otherwise unchanged.
        rig_line=("measured off the rig: steer axle %(steer_axle_x).1f, "
                 "fixed axle %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f), "
                 "track %(main_gear_track).1f uu"),
        tightest_radius_line=("tightest followable radius %(radius_uu).0f uu (%(radius_m).1f m) "
                             "at %(lock).0f degrees of lock - the smallest in the fleet"),
        ground=GROUND,
        steering=STEERING,
        climb=CLIMB,
        approach=APPROACH,
        engine=ENGINE,
        gear=None,   # THE GEAR IS FIXED - see the header. Leaving this None is the declaration.
        requirements=REQUIREMENTS,
        turnaround_seconds=TURNAROUND_SECONDS,
        prop_blade_count=PROP_BLADE_COUNT,
        surface="GRASS",   # a 172 flies off a farm strip; this type must never force tarmac.
        angles=None,       # no retract bones, no bay doors - nothing for the ABP to fold.
        published_table=PUBLISHED_TABLE,
        published_table_label="the 172S's",
        published_table_width=18,
        fixed_gear_suffix=", which is a 172",
        report_extra=_report,
        # HAND-WIRED BEFORE THIS TOOLING EXISTED - no per-key wiring script, no measured
        # axis plan. See AircraftSpec.wire_script and _anim_report_extra above.
        wire_script=False,
        anim_report_extra=_anim_report_extra,
        pushback_need="SELF_MANOEUVRE",
        pushback_reason=(
            "a 172 is pushed off a stand by one person leaning on the strut, and the class default "
            "of VehicleTug would gate the smallest aeroplane in the game behind the depot"
        ),
        yard_label="Plane1 (Cessna 172)",
    )
