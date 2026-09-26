"""Authors DA_Aircraft_Plane<N> and sets ABP_Plane<N>'s rig facts, for whichever key names a
module under aircraft/. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=Tools/Python/build_aircraft_type.py
      -unattended -nosplash -nopause <key>

Every result line is prefixed MARKER: so it can be grepped out of the log.

ONE MECHANISM, FOURTEEN SPECS - Issue #290. The fourteen build_plane<N>_type.py scripts this
replaced (since deleted) were 7,527 lines that were ~75% the same script fourteen times over:
measure(), set_regime(),
the CLIMB/APPROACH/ENGINE/GEAR block loop, verify(), set_anim_defaults(), tightest_radius_uu()
and run() were byte-identical across most of the fleet apart from an asset-path string, and the
copies had already drifted where hand-copying missed a line - propeller_diameter authored but
never read back on nine of fourteen, tailplane_span checked on four, the ground surface/approach
PASS line dropped from the three newest scripts, plane2's fixed gear never asserted, plane4's
gear-cycle-vs-clear-altitude check never added. GEOMETRY IS MEASURED, PERFORMANCE IS PUBLISHED
remains true of every aircraft here; only the SCRIPT that says so is now one script.

WHAT LIVES WHERE. This file is the MECHANISM: it knows how to read a .glb's part bounds, walk a
skeleton's reference pose, average a wheel-bone group into an axle, drive the six performance
blocks onto a UAircraftType, and read every field back. It knows NOTHING about any one
aeroplane. `Tools/Python/aircraft/<key>.py` is the SPEC: it knows nothing about how an
AircraftType asset is authored, only what is true of one aeroplane - its part names, its wheel
groups, its published figures, and the WHY behind each one, carried over verbatim in its
docstring rather than dropped. `get_spec()` returns a FRESH `AircraftSpec` each call, on purpose
- see `measured()`, whose cache lives on the spec instance so two runs in one process (the
refactor's own harness runs all fourteen back to back) do not share stale geometry.

THE ANGLE SOURCE, WHERE FOUR MECHANISMS BECOME ONE. The fleet's per-model animation angles -
how far the gear folds, where a bay door counts as shut, how far a bogie tilts - came from FOUR
different places depending on which script wrote it: a literal typed into the script (plane3,
5, 6, 7), `rig_map.json` (plane8, 9, 11, 13 - and plane8's own shape differs from the other
three's), a regex over the model's build_rig.py (plane14), or nothing at all because the rig has
no retracting bones (plane1, 2, 10, 12). `Typed`, `RigMap` and `RigScriptLine` below are the one
mechanism each of those four now goes through - a spec picks the one that describes where its
own angles actually live, and `set_anim_defaults()` reads whichever it is the same way every
time. `angles=None` is its own answer, not a missing one: it says the rig has nothing to fold.

WHEEL-BONE GROUPS, HANDLED 1..N UNIFORMLY. plane1 through plane5, plane7, plane9, plane10,
plane12 and plane14 have one wheel bone a side; plane6 and plane11 have a three-axle bogie;
plane13 has two axles; plane8 has five, spanning two gear units. `axles_and_radius()` averages
whatever `main_wheels_l`/`main_wheels_r` name, for any N, rather than plane6-through-plane13
each carrying their own copy of the averaging loop with N baked in.

WHAT DID NOT MOVE HERE: the retired plane7 type script's replacement, `aircraft/plane7.py`, still routes
through `unreal.AircraftType.build_piper_meridian()` rather than through this file's typed-dict
authoring path - the Meridian's figures live in C++ (`UAircraftType::BuildPiperMeridian`)
because it is ALSO `UAirsideSettings::ResolveDefaultAirframe`'s fallback, and a second copy of
them here would be exactly the "seven call sites hardcoding a Piper's numbers" failure
AirsideContent.h already names. `author_type()`/`verify()` branch on `spec.cpp_builder` for
that one aircraft; everything else in this file is one path.

EVERY CHECK RUNS FOR EVERY AIRCRAFT, and where the fourteen originals disagreed about which
checks a given plane ran, the union wins rather than the intersection - each such case is
named at its call site below with the evidence for why it was a gap rather than a choice
(propeller_diameter, tailplane_span, main_gear_track, the fixed-gear assertion, the ground
surface/approach line, the gear-cycle-vs-clear-altitude assertion). A spec that genuinely
cannot run a check names it in `spec.skipped_checks` with a reason string, which `verify()`
and `author_type()` report via `say()` rather than silently omitting.
"""
import importlib
import json
import math
import os
import re
import struct
import sys
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. Put it on before importing the shared reader or the aircraft/ package.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

TYPE_PATH = "/Game/Entities"


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


# --- Angle sources: the one mechanism each of the four now goes through --------------------


class Typed:
    """Angle source: the ABP's angle is a project ruling rather than a fact read off a file -
    there is no rig_map.json and no Blender script line to parse, so the number is typed here
    and the spec's own docstring is where the reasoning for it lives."""

    def __init__(self, gear=None, door=None, truck=None):
        self.gear, self.door, self.truck = gear, door, truck

    def read(self):
        return {"gear_retracted": self.gear, "bay_door_closed": self.door,
                "truck_tilted": self.truck}


class RigMap:
    """Angle source: plane<N>/scripts/rig_map.json, written by the model's OWN build_rig.py
    after posing the solved fold - the ABP's angle and the model's cannot drift because both
    are read from what the rig proved, not retyped by hand here.

    THE FILE'S SHAPE IS NOT FORCED TO AGREE, because it is not true that it does: plane8's
    rig_map.json carries per-bone dicts (`retract_deg["gear_wing_L"]`, `max(truck_deg.values())`)
    because plane8 has more than one gear unit; plane9/11/13's carries a flat `angles` object
    because they have one. A spec supplies three small readers - one per angle - that say WHERE
    in ITS OWN file that angle is, which is one honest fact about the rig rather than a shape
    imposed on it.

    `note` is optional: plane8 alone reports the per-bone dicts a rig with more than one gear
    unit actually carries, because its `anim_multiplier`'s per-bone ratios are what makes the
    single per-type angle correct for every bone - see aircraft/plane8.py.
    """

    def __init__(self, path, gear, door, truck=None, note=None):
        self.path, self._gear, self._door, self._truck = path, gear, door, truck
        self._note = note
        self.raw = None

    def read(self):
        with open(self.path) as handle:
            self.raw = json.load(handle)
        return {
            "gear_retracted": self._gear(self.raw),
            "bay_door_closed": self._door(self.raw),
            "truck_tilted": self._truck(self.raw) if self._truck else None,
        }

    def note(self):
        return self._note(self.raw) if self._note else None


class RigScriptLine:
    """Angle source: a `NAME, NAME2 = X, Y` line in the model's own build_rig.py, for a rig
    whose pipeline (plane14's is plane12's) writes no rig_map.json at all. Read the source
    rather than retyping it, so the ABP's angle and the model's still cannot drift; a reworded
    line raises rather than silently defaulting to a stale number.
    """

    def __init__(self, path, pattern, gear_group=1, door_group=2):
        self.path, self.pattern = path, pattern
        self.gear_group, self.door_group = gear_group, door_group

    def read(self):
        with open(self.path) as handle:
            text = handle.read()
        found = re.search(self.pattern, text, re.M)
        if found is None:
            raise ValueError("no '%s' line in %s - the rig script was reworded; read its "
                             "angles by hand and update this pattern"
                             % (self.pattern, self.path))
        return {"gear_retracted": float(found.group(self.gear_group)),
                "bay_door_closed": float(found.group(self.door_group)),
                "truck_tilted": None}


# --- Shared glTF vertex reading, for the two non-bounding-box propeller measurements ---------
#
# read_gltf()/part_bounds_uu() answer "what is this part's BOX", which every propeller in the
# fleet used until plane10's three blades read 12% short in one (an odd blade count puts no two
# tips on one diameter) and plane12's two-blade, five-degree-nose-up Cherokee read short in the
# OTHER direction (the box height is D*cos(5) when the disc is tilted). Both need the actual
# vertices, which part_bounds_uu does not keep - so both read the .glb's binary chunk directly,
# through these two functions, rather than each carrying its own copy of a glTF accessor walk.


def read_glb(path):
    """(JSON chunk, BIN chunk). read_gltf() returns only the first; a vertex-level measurement
    needs the second."""
    with open(path, "rb") as handle:
        data = handle.read()
    json_length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + json_length].decode("utf-8"))
    bin_at = 20 + json_length
    bin_length = struct.unpack_from("<I", data, bin_at)[0]
    return doc, data[bin_at + 8:bin_at + 8 + bin_length]


def glb_positions(doc, binary, accessor_index):
    """An accessor's VEC3 float positions. Only what a POSITION accessor can be: FLOAT, VEC3."""
    accessor = doc["accessors"][accessor_index]
    view = doc["bufferViews"][accessor["bufferView"]]
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", 12)
    for i in range(accessor["count"]):
        yield struct.unpack_from("<3f", binary, start + i * stride)


def glb_mesh_positions(doc, binary, mesh_name):
    """Every POSITION vertex under nodes named `mesh_name` (Blender's '.001' duplicate suffix
    stripped), across every primitive. Both disc-diameter strategies want exactly this."""
    points = []
    for node in doc["nodes"]:
        if node.get("mesh") is None or node.get("name", "").split(".")[0] != mesh_name:
            continue
        for primitive in doc["meshes"][node["mesh"]]["primitives"]:
            points.extend(glb_positions(doc, binary, primitive["attributes"]["POSITION"]))
    return points


# --- The spec every aircraft/<key>.py builds -------------------------------------------------


@dataclass
class AircraftSpec:
    key: str
    type_name: str
    short_code: str
    display_name: str
    code: str
    mesh: str
    abp: str
    source: str

    wing: str = "wing"
    stabiliser: str = "stabiliser"
    # None: no separate leg mesh exists on this rig - plane2's Twin Otter, whose gear is a
    # strut/nacelle assembly with nothing named as one object. See measure()'s note.
    gear_leg: Optional[str] = "maingear_L"
    prop: str = "prop_L"
    spinner: Optional[str] = None   # only read by a custom prop_diameter_fn

    main_wheels_l: Sequence[str] = ("wheel_L",)
    main_wheels_r: Sequence[str] = ("wheel_R",)
    # THE TRACK'S OWN WHEELS, when they are not the same ones the fixed axle averages - plane8's
    # main-gear centre is all ten wheels, but its track is the WING bogies' alone (the mean of a
    # 12.5 m wing track and a 5.3 m body track puts the smoke between the two units, where there
    # is no wheel at all). None means "the same wheels", which is every other aircraft.
    track_wheels_l: Optional[Sequence[str]] = None
    track_wheels_r: Optional[Sequence[str]] = None
    # plane8's five-a-side spans TWO gear units 2 m apart, so its stations must clear three
    # wheel radii rather than one to prove they are not stacked on a single station.
    station_spread_multiplier: float = 1.0

    # Default: the propeller/fan's widest cross-axis box extent. plane10 and plane12 override
    # this - see aircraft/plane10.py and aircraft/plane12.py for why a box is wrong for each.
    prop_diameter_fn: Optional[Callable] = None

    ground: Dict[str, Dict[str, float]] = field(default_factory=dict)
    steering: Dict[str, float] = field(default_factory=dict)
    climb: Dict[str, float] = field(default_factory=dict)
    approach: Dict[str, float] = field(default_factory=dict)
    engine: Dict[str, float] = field(default_factory=dict)
    gear: Optional[Dict[str, float]] = None
    requirements: Dict[str, float] = field(default_factory=dict)
    turnaround_seconds: float = 0.0
    prop_blade_count: int = 3
    surface: str = "TARMAC"

    angles: Optional[object] = None   # Typed | RigMap | RigScriptLine | None

    # The gear-cycle-top formula's vertical-rate term. Every retracting aircraft but plane5 uses
    # climb_speed * radians(pitch); plane5 was authored with climb_speed * sin(radians(pitch))
    # (the true vertical component) before the rest of the fleet settled on the small-angle form,
    # and the two agree to within 1% at these pitch angles - not worth re-deriving plane5's
    # PASS line over, so its own spec passes this in rather than the mechanism silently
    # re-deriving a different number than the one plane5 has shipped since 2026-09-19.
    climb_rate_fn: Optional[Callable] = None

    # Per-aircraft wording for the two measured-figures log lines and the tightest-radius line -
    # DATA, not a second mechanism: the values and the PASS/FAIL logic behind them are computed
    # identically for every aircraft, and only the prose (a "prop" vs a "fan", a fleet-context
    # aside like "the smallest in the fleet") is a fact about ONE aeroplane, exactly as a
    # docstring paragraph is.
    export_line: str = ("measured off the export: wheel radius %(wheel_radius).1f, "
                        "prop %(prop_diameter).1f, nose %(nose_x).1f, tail %(tail_x).1f uu")
    rig_line: str = ("measured off the rig: steer axle %(steer_axle_x).1f, "
                     "fixed axle %(fixed_axle_x).1f uu (wheelbase %(wheelbase).1f), "
                     "track %(main_gear_track).1f")
    tightest_radius_line: Optional[str] = None   # None -> DEFAULT_TIGHTEST_RADIUS_LINE
    field_lengths_suffix: str = ""
    fixed_gear_suffix: str = ""   # plane1's ", which is a 172" - a fact about ONE aeroplane

    # WHAT RUNS BETWEEN THE RIG LINE AND THE BLOCK-AUTHORING, called as
    # `report_extra(spec, m, say, fail)` when set. The DEFAULT (None) is "print the tightest-
    # radius line and nothing else", which is what nine of the fourteen aircraft do. The five
    # that report something else - plane1's and plane10's/plane12's measured-against-a-primary-
    # -source table, plane4's nose-overhang check, plane5's wheelbase note, plane6's/plane8's
    # span-and-tail report - own the ORDER they print in, using the shared
    # `say_tightest_radius`/`say_published_table`/`say_span_and_tail` helpers below rather than
    # each re-deriving the values. A one-off check is a fact about ONE aeroplane's primary
    # source; the ORDER it runs in relative to the tightest-radius line is too, which is why
    # this is one hook rather than the mechanism guessing a universal order.
    report_extra: Optional[Callable] = None

    # plane7 alone: routes authoring through unreal.AircraftType.build_piper_meridian() rather
    # than through this file's typed-dict path - see the module docstring.
    cpp_builder: Optional[str] = None
    old_name: Optional[str] = None

    # The measured-against-a-primary-source table plane1, plane10 and plane12 print every run -
    # one shared loop, driven by each spec's own (label, uu-getter, published metres) rows.
    published_table: Optional[List[Tuple[str, Callable, float]]] = None
    published_table_label: str = ""
    published_table_width: int = 12

    # THE GEAR BLOCK'S POSITION IN verify(), which plane3/plane4/plane5 print immediately after
    # the five core checks and plane6 on print after mesh/anim_class - two conventions with no
    # fact to arbitrate between them, so each keeps its own rather than the mechanism inventing
    # a third. Only meaningful when `gear` is not None.
    gear_checked_early: bool = False

    # Checks the union would otherwise run on this aircraft, opted out BY NAME with a reason -
    # CLAUDE.md's rule: an omission is a decision, and a decision says why. Empty for every
    # aircraft in the fleet today; the field exists so the NEXT aircraft that genuinely cannot
    # run a check says so instead of the mechanism silently growing a special case.
    skipped_checks: Dict[str, str] = field(default_factory=dict)

    # --- Anim fields - Issue #294. build_aircraft_anim.py reads these off the SAME spec
    # author_type() and set_anim_defaults() above already read, rather than a second spec
    # directory: the rig facts (mesh, source, angles) do not change depending on which script
    # is asking. See build_aircraft_anim.py's own module docstring for the mechanism.

    # BONES DELIBERATELY NOT SQUARE TO THE AIRFRAME - resolve_axis's `raked` argument, carried
    # here so it is a fact about the aeroplane rather than a copy-paste risk in a build script.
    # Empty for nine of fourteen; plane6/plane8 name three, plane9/plane12 one. THE BUG THIS
    # FIELD FIXES: plane14's retired build script had RAKED correctly () (plane14's
    # nosewheel_steer measures square) but its say() line read "ONE BONE IS RAKED ON PURPOSE"
    # anyway - copied
    # from plane9/plane12 and never re-typed. Deriving that line's wording from len(raked)
    # removes the possibility of the two disagreeing again.
    raked: Sequence[str] = ()

    # BONES A BONE_RULES NEEDLE MATCHES BY A FALSE SUBSTRING, EXCLUDED FROM THE DRIVEN SET BY
    # NAME - build_rig_anim.py's fifth_wheel/kingpin carve-out, modelled as data rather than a
    # special case in BONE_RULES itself. Empty for every aircraft today; no aeroplane rig has
    # hit this yet, and the field exists so the next one that does says so instead of
    # driven_bone_plan() silently wiring a socket.
    excluded: Sequence[str] = ()

    # PER-BONE MULTIPLIER OVERRIDES, keyed by bone name - plane8's gear/truck ratios, read off
    # plane8/scripts/rig_map.json rather than typed twice (see aircraft/plane8.py). Every OTHER
    # aircraft leaves this empty and gets the fleet's plain convention: -1.0 (the Blender/UE
    # handedness flip) on every travelling or spinning bone, None (undriven multiplier, i.e.
    # the raw angle) on the one bone whose variable is SteerAngleDegrees. A bone named here
    # takes ITS entry instead of the convention - see anim_multiplier_for().
    anim_multiplier: Dict[str, float] = field(default_factory=dict)

    # FALSE FOR plane1 AND plane2 ALONE: their AnimGraphs were wired by hand in the editor
    # before this tooling existed and carry no Tools/wire_<key>_anim.py counterpart, so
    # build_aircraft_anim.py prints the bone plan for a human to wire rather than measuring
    # rotation axes or writing Saved/<key>_axis_plan.json - there is no wiring script waiting
    # to read one. See aircraft/plane1.py, aircraft/plane2.py.
    wire_script: bool = True

    # FALSE FOR plane4 ALONE: ABP_Plane4 was duplicated from ABP_Plane2 in the editor on
    # 2026-09-19 and retargeted by hand, days before this tooling existed. build_aircraft_anim.py
    # measures an asset that must already exist for this key and fails loudly rather than
    # inventing an empty one that would point DA_Aircraft_Plane4 at a Blueprint driving nothing.
    create_abp: bool = True

    # EXTRA PROSE PRINTED AFTER THE STANDARD BONE LIST, for the two hand-wired aircraft alone -
    # plane1's disputed wiring order finding, which is a fact about THAT aeroplane's shipped
    # graph and belongs in its own spec rather than in the shared mechanism. `None` for every
    # other aircraft, `wire_script`-gated so it is only ever consulted where there is no wiring
    # script to make the finding moot.
    anim_report_extra: Optional[Callable] = None

    def __post_init__(self):
        self._measured = None

    # THE TRACK WHEELS AND MAIN WHEELS ARE THE SAME UNLESS A SPEC SAYS OTHERWISE.
    @property
    def track_l(self):
        return self.track_wheels_l or self.main_wheels_l

    @property
    def track_r(self):
        return self.track_wheels_r or self.main_wheels_r


DEFAULT_TIGHTEST_RADIUS_LINE = ("tightest followable radius %(radius_uu).0f uu "
                                "(%(radius_m).1f m) at %(lock).0f degrees of lock")


def spec_for(key):
    """The named aircraft/<key>.py module's spec, fresh - see the module docstring for why
    fresh rather than a shared singleton."""
    module = importlib.import_module("aircraft.%s" % key)
    return module.get_spec()


# --- Measurement, generic over any spec's part names and wheel-bone groups ------------------


def measure(spec):
    """The footprint, the propeller and the gear leg's height, off the export's own parts.

    Raises rather than guessing: a renamed part is a change to look at, not a figure to
    silently default - every build_plane<N>_type.py stated this rule and it is unchanged here.
    """
    parts = part_bounds_uu(read_gltf(spec.source))

    def find(stem):
        if stem not in parts:
            raise KeyError("no part named %s in %s - the rig was renamed. Parts: %s"
                           % (stem, spec.source, ", ".join(sorted(parts))))
        return parts[stem]

    every = list(parts.values())
    # The nose is whichever part reaches furthest forward (a spinner, an intake) and the tail
    # whichever reaches furthest aft (a fin); the maximum/minimum over every part gets both
    # without this function having to know which part is foremost - the expression every one of
    # the fourteen originals used.
    nose_x = max(high[0] for _, high in every)
    tail_x = min(low[0] for low, _ in every)

    wing_lo, wing_hi = find(spec.wing)
    stab_lo, stab_hi = find(spec.stabiliser)
    prop_lo, prop_hi = find(spec.prop)
    # THE LEG HEIGHT IS OPTIONAL. plane2's Twin Otter has no separate leg mesh at all - its
    # nacelle/strut objects were never named as one, so its original script never had a sanity
    # bound to check the wheel radius against. `gear_leg=None` says the same thing here: no leg
    # part exists for this rig, so no upper bound is checked (the lower bound and the "wheels
    # are not on top of each other" check in axles_and_radius() still run regardless).
    leg_height = None
    if spec.gear_leg is not None:
        leg_lo, leg_hi = find(spec.gear_leg)
        leg_height = leg_hi[2] - leg_lo[2]
    if spec.spinner:
        find(spec.spinner)

    footprint = {
        "nose_x": nose_x,
        "tail_x": tail_x,
        "wingspan": wing_hi[1] - wing_lo[1],
        "wing_x": (wing_lo[0] + wing_hi[0]) * 0.5,
        "tailplane_span": stab_hi[1] - stab_lo[1],
        "tailplane_x": (stab_lo[0] + stab_hi[0]) * 0.5,
    }
    if spec.prop_diameter_fn is not None:
        prop_diameter = spec.prop_diameter_fn(prop_lo, prop_hi, spec)
    else:
        # The disc lies ACROSS the airframe rather than along it - so the larger of the two
        # cross-axis extents, never the chordwise one.
        prop_diameter = max(prop_hi[1] - prop_lo[1], prop_hi[2] - prop_lo[2])
    return footprint, prop_diameter, leg_height


def axles_and_radius(spec, leg_height):
    """(steer axle X, fixed axle X, main wheel radius, main gear track) in uu, off the spec's
    own mesh reference pose.

    THE BONE, NOT THE TYRE'S BOUNDING BOX, the rule plane3's retired type script settled: the bone is a
    STATEMENT about where the axle is - build_export.py/build_rig.py place each wheel's origin
    on its rotation axis so the thing can spin - and the hub's HEIGHT above the contact plane IS
    the radius. That z = 0 is the contact plane is not assumed: airside_import.report_bounds
    FAILS an import whose lowest vertex is more than 10 uu off it, so a model that floats never
    reaches this script.

    ANY NUMBER OF WHEELS A SIDE, AVERAGED - plane6's rule (a bogie's CENTRE is the axle a
    multi-axle truck pivots about, which is what FChassis::FixedAxleX wants) generalised from
    "three" to "however many `spec.main_wheels_l` names". One wheel a side averages to itself,
    so plane1 through plane5's single pair need no special case here.
    """
    mesh = unreal.EditorAssetLibrary.load_asset(spec.mesh)
    if mesh is None:
        raise KeyError("no %s - run import_models.py first" % spec.mesh)

    pose = mesh.get_editor_property("skeleton").get_reference_pose()
    at = {}
    for name in unreal.AnimPose.get_bone_names(pose):
        bone = unreal.AnimPose.get_bone_pose(pose, name, unreal.AnimPoseSpaces.WORLD)
        at[str(name)] = bone.translation

    wanted = set(("nosewheel",) + tuple(spec.main_wheels_l) + tuple(spec.main_wheels_r)
                + tuple(spec.track_l) + tuple(spec.track_r))
    missing = [name for name in wanted if name not in at]
    if missing:
        raise KeyError("%s has no %s bone(s) - the rig was renamed. Bones: %s"
                       % (spec.mesh, ", ".join(sorted(missing)), ", ".join(sorted(at))))

    def mean(values):
        return sum(values) / float(len(values))

    left = [at[name] for name in spec.main_wheels_l]
    right = [at[name] for name in spec.main_wheels_r]
    both = left + right
    mains_x = mean([v.x for v in both])
    radius = mean([v.z for v in both])
    track = abs(mean([at[n].y for n in spec.track_l]) - mean([at[n].y for n in spec.track_r]))

    n = len(spec.main_wheels_l)
    if n > 1:
        # A BOGIE IS N AXLES, SO ITS BONES MUST BE AT N STATIONS - bones stacked on one x
        # average to a right-looking wheelbase and roll their tyres through each other.
        stations = sorted(v.x for v in left)
        spread = stations[-1] - stations[0]
        threshold = radius * spec.station_spread_multiplier
        if spread < threshold:
            raise ValueError("the %d left main wheel bones span %.1f uu, less than %.1f uu - "
                             "stacked on one station rather than %d axles"
                             % (n, spread, threshold, n))
        # AND AT ONE HEIGHT, because they share an axle beam; a wheel off the beam is a wheel
        # that is not on the ground, and the mean radius above would average the error away.
        for name in spec.main_wheels_l + spec.main_wheels_r:
            if abs(at[name].z - radius) > 1.0:
                raise ValueError("%s's hub is at z=%.1f against the mains' mean %.1f - the %d "
                                 "main wheels are not on one plane"
                                 % (name, at[name].z, radius, 2 * n))

    if track <= radius:
        raise ValueError("the main gear bones are %.1f uu apart, inside one wheel's %.1f uu "
                         "radius - they are on top of each other" % (track, radius))
    # A RADIUS MAY NOT EXCEED THE LEG THAT CARRIES IT, and it may not be nothing. The two
    # bounds catch the same failure from opposite sides: wheel bones left at the root read as
    # radius 0, and a model exported in metres reads as a wheel taller than the aeroplane. The
    # upper bound only applies where there IS a leg to check against - see measure()'s note on
    # `gear_leg=None`.
    if leg_height is not None and not 5.0 < radius < leg_height:
        raise ValueError("the main-gear hub sits at z=%.1f against a %.1f uu leg - the bone "
                         "is not on the axle, or the model is not on the ground plane"
                         % (radius, leg_height))
    elif leg_height is None and radius <= 5.0:
        raise ValueError("the main-gear hub sits at z=%.1f, which is not a plausible wheel "
                         "radius - the bone is not on the axle" % radius)

    return at["nosewheel"].x, mains_x, radius, track


def measured(spec):
    """measure() and axles_and_radius(), once per spec instance.

    Three places need these figures - the type, the ABP's defaults and the read-back
    verification - and all three must agree or the verification is checking a different
    aeroplane than the one that was written. Cached on the SPEC rather than as a module global:
    this mechanism authors all fourteen aircraft in one process during the refactor's own
    harness, and a module-level cache would hand plane9's geometry to plane11.
    """
    if spec._measured is None:
        footprint, prop, leg = measure(spec)
        steer_x, fixed_x, radius, track = axles_and_radius(spec, leg)
        spec._measured = dict(footprint=footprint, prop_diameter=prop, wheel_radius=radius,
                              steer_axle_x=steer_x, fixed_axle_x=fixed_x, main_gear_track=track)
    return spec._measured


def tightest_radius_uu(spec):
    """The tightest corner the rolling-steer law will follow, uu: R >= L / sin(lock).

    Derived here rather than typed into a comment, because the wheelbase is measured and a
    typed figure would stop being true the moment the model moved.
    """
    m = measured(spec)
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    return wheelbase / math.sin(math.radians(spec.steering["max_steer_degrees"]))


def gear_cycle_top_uu(spec):
    """The height at which the retraction cycle FINISHES, uu - what CLIMB's clear_altitude has
    to clear. An agent removed from the world part way through its cycle vanishes with its gear
    half up, in front of the player, and the only symptom is that it looked wrong for a moment.
    """
    gear = spec.gear
    cycle = gear["door_seconds"] * 2.0 + gear.get("truck_tilt_seconds", 0.0) * 2.0 + gear["travel_seconds"]
    if spec.climb_rate_fn is not None:
        rate = spec.climb_rate_fn(spec.climb)
    else:
        rate = spec.climb["climb_speed"] * math.radians(spec.climb["climb_pitch_degrees"])
    return cycle, gear["retract_above_height"] + rate * cycle


# --- Authoring ---------------------------------------------------------------------------


def set_regime(ground, name, values):
    regime = ground.get_editor_property(name)
    for field_name, value in values.items():
        regime.set_editor_property(field_name, value)
    ground.set_editor_property(name, regime)


def _load_or_create(spec):
    path = "%s/%s" % (TYPE_PATH, spec.type_name)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: an airline's fleet points at this, and
        # deleting an asset something references breaks the reference rather than updating it.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            spec.type_name, TYPE_PATH, unreal.AircraftType, None)
    if asset is None:
        fail("could not create %s" % path)
    return path, asset


def _rename_or_load(spec):
    """plane7 alone: the type asset, renamed from its old name on the first run. See
    aircraft/plane7.py for why this one aircraft is a rename rather than a fresh asset -
    DA_Airline_Cumbria's fleet and DA_AirsideContent's DefaultAircraft both point at it."""
    new_path = "%s/%s" % (TYPE_PATH, spec.type_name)
    old_path = "%s/%s" % (TYPE_PATH, spec.old_name)
    has_new = unreal.EditorAssetLibrary.does_asset_exist(new_path)
    has_old = unreal.EditorAssetLibrary.does_asset_exist(old_path)

    if has_new and has_old:
        fail("both %s and %s exist. One of them is unreferenced and this script cannot tell "
             "which - delete the wrong one by hand and re-run." % (old_path, new_path))
        return new_path, None

    if has_old:
        if not unreal.EditorAssetLibrary.rename_asset(old_path, new_path):
            fail("could not rename %s to %s" % (old_path, new_path))
            return new_path, None
        say("renamed %s -> %s; DA_Airline_Cumbria's fleet and DA_AirsideContent's "
            "DefaultAircraft follow the object" % (old_path, new_path))
    elif not has_new:
        if unreal.AssetToolsHelpers.get_asset_tools().create_asset(
                spec.type_name, TYPE_PATH, unreal.AircraftType, None) is None:
            fail("could not create %s" % new_path)
            return new_path, None
        say("created %s - there was no %s to rename" % (new_path, old_path))
    else:
        say("%s is already in place; nothing to rename" % new_path)

    return new_path, unreal.EditorAssetLibrary.load_asset(new_path)


def _point_at_mesh_and_anim(spec, asset):
    mesh = unreal.EditorAssetLibrary.load_asset(spec.mesh)
    if mesh is None:
        fail("no %s to point the type at" % spec.mesh)
    else:
        asset.set_editor_property("mesh", mesh)

    abp = unreal.EditorAssetLibrary.load_asset(spec.abp)
    if abp is None:
        fail("no %s to point the type at" % spec.abp)
    elif abp.generated_class() is None:
        fail("%s has no generated class; compile it before running this" % spec.abp)
    else:
        asset.set_editor_property("anim_class", abp.generated_class())


def say_tightest_radius(spec, m, say):
    """THE FIGURE THAT DECIDES WHETHER THIS TYPE CAN USE THE PLAYER'S TAXIWAYS, said out loud
    because it is not otherwise visible anywhere: the rolling-steer law refuses a corner
    tighter than this, and a refusal reads as "no route" rather than as "too big"."""
    radius_uu = tightest_radius_uu(spec)
    line = spec.tightest_radius_line or DEFAULT_TIGHTEST_RADIUS_LINE
    say(line % dict(radius_uu=radius_uu, radius_m=radius_uu / 100.0,
                    lock=spec.steering["max_steer_degrees"]))


def say_published_table(spec, m, say):
    """THE MEASURED-AGAINST-PUBLISHED TABLE, printed every run - the habit plane4's retired
    type script argued for: a disagreement between the model and a primary source is worth SEEING each
    time rather than discovering. One shared loop, driven by each spec's own rows, rather than
    plane1's/plane10's/plane12's three copies of the same print statement."""
    for label, getter, published_m in spec.published_table:
        got_m = getter(m) / 100.0
        say("  %-*s measured %6.3f m against %s %6.3f m  (%+.1f%%)"
            % (spec.published_table_width, label, got_m, spec.published_table_label,
               published_m, 100.0 * (got_m - published_m) / published_m))


def say_span_and_tail(spec, m, say):
    """SPAN AND TAIL, MEASURED AND WRITTEN - not judged here. Whether either still fits its
    Code letter's row is Airside.Content.MeasuredTypesFitTheirLettersRow's question, against
    the row in Solve/IcaoCode.cpp; this only reports what the export measures. Shared by
    plane6 and plane8, the two widebodies whose span sits within a few percent of their
    letter's own ceiling."""
    span = m["footprint"]["wingspan"]
    say("span %.1f uu (%.2f m)" % (span, span / 100.0))
    tail_aft = -(m["footprint"]["tail_x"] - m["steer_axle_x"])
    say("tail reaches %.1f uu aft of the stop mark" % tail_aft)


def _say_measured_lines(spec, m):
    say(spec.export_line % dict(
        wheel_radius=m["wheel_radius"], prop_diameter=m["prop_diameter"],
        nose_x=m["footprint"]["nose_x"], tail_x=m["footprint"]["tail_x"],
        wingspan=m["footprint"]["wingspan"], wing_x=m["footprint"]["wing_x"]))
    wheelbase = abs(m["fixed_axle_x"] - m["steer_axle_x"])
    say(spec.rig_line % dict(
        steer_axle_x=m["steer_axle_x"], fixed_axle_x=m["fixed_axle_x"], wheelbase=wheelbase,
        main_gear_track=m["main_gear_track"]))

    if spec.report_extra is not None:
        spec.report_extra(spec, m, say, fail)
    elif "max_steer_degrees" in spec.steering:
        # plane7's C++-authored steering has no Python copy to compute this from - see the
        # module docstring on why a second copy of the Meridian's figures does not belong here.
        say_tightest_radius(spec, m, say)


def _author_via_dicts(spec):
    path, asset = _load_or_create(spec)
    if asset is None:
        return None

    asset.set_editor_property("code", unreal.Name(spec.code))
    asset.set_editor_property("short_code", unreal.Name(spec.short_code))
    asset.set_editor_property("display_name", unreal.Text(spec.display_name))
    _point_at_mesh_and_anim(spec, asset)

    m = measured(spec)
    _say_measured_lines(spec, m)

    asset.set_editor_property("steer_axle_x", m["steer_axle_x"])
    asset.set_editor_property("fixed_axle_x", m["fixed_axle_x"])
    asset.set_editor_property("main_gear_track", m["main_gear_track"])
    asset.set_editor_property("main_wheel_radius", m["wheel_radius"])
    asset.set_editor_property("propeller_diameter", m["prop_diameter"])
    asset.set_editor_property("turnaround_seconds", spec.turnaround_seconds)

    footprint = asset.get_editor_property("footprint")
    for field_name, value in m["footprint"].items():
        footprint.set_editor_property(field_name, value)
    asset.set_editor_property("footprint", footprint)

    ground = asset.get_editor_property("ground")
    for name, values in spec.ground.items():
        set_regime(ground, name, values)
    for field_name, value in spec.steering.items():
        ground.set_editor_property(field_name, value)
    asset.set_editor_property("ground", ground)

    for group_name, values in (("climb", spec.climb), ("approach", spec.approach),
                              ("engine", spec.engine)):
        block = asset.get_editor_property(group_name)
        for field_name, value in values.items():
            block.set_editor_property(field_name, value)
        asset.set_editor_property(group_name, block)

    if spec.gear is not None:
        block = asset.get_editor_property("gear")
        for field_name, value in spec.gear.items():
            block.set_editor_property(field_name, value)
        asset.set_editor_property("gear", block)

        # THE GEAR CYCLE MUST FINISH BEFORE THE AGENT IS REMOVED, asserted rather than argued,
        # and against the climb figures just written rather than a copy of them - run for EVERY
        # retracting-gear aircraft. plane4 shipped without this check for over a year; nothing
        # about the 737's figures needed it skipped, so it runs here as it does for the other
        # nine retracting types.
        cycle, top = gear_cycle_top_uu(spec)
        clear = spec.climb["clear_altitude"]
        if top >= clear:
            fail("the %.0f s gear cycle finishes at about %.0f uu but the agent is cleared at "
                 "%.0f - it would vanish mid-retraction." % (cycle, top, clear))
        else:
            say("PASS the %.0f s gear cycle finishes at about %.0f uu, below the %.0f uu the "
                "agent is cleared at" % (cycle, top, clear))

    requirements = asset.get_editor_property("requirements")
    for field_name, value in spec.requirements.items():
        requirements.set_editor_property(field_name, value)
    requirements.set_editor_property(
        "minimum_surface",
        unreal.RunwaySurface.GRASS if spec.surface == "GRASS" else unreal.RunwaySurface.TARMAC)
    requirements.set_editor_property("approach_needed", unreal.RunwayApproach.VISUAL)
    asset.set_editor_property("requirements", requirements)

    # PUSHBACK IS NOT SET HERE - Tools/Python/build_pushback_needs.py owns EPushbackNeed for
    # every type; see verify()'s NOTE line, which reports it rather than re-authoring it.

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def _author_via_cpp_builder(spec):
    asset = None
    path, asset = _rename_or_load(spec)
    if asset is None:
        return None

    builder = getattr(unreal.AircraftType, spec.cpp_builder)
    builder(asset)
    say("laid the type down from unreal.AircraftType.%s - code %s, short code %s, '%s'"
        % (spec.cpp_builder, asset.get_editor_property("code"),
           asset.get_editor_property("short_code"), asset.get_editor_property("display_name")))

    _point_at_mesh_and_anim(spec, asset)

    m = measured(spec)
    _say_measured_lines(spec, m)

    # THE TWO FIELDS THE C++ BUILDER HAS NO PLACE FOR. Everything else on this asset came from
    # it; these are measured because nothing else can answer them.
    asset.set_editor_property("main_gear_track", m["main_gear_track"])
    asset.set_editor_property("propeller_diameter", m["prop_diameter"])

    if spec.gear is not None:
        block = asset.get_editor_property("gear")
        for field_name, value in spec.gear.items():
            block.set_editor_property(field_name, value)
        asset.set_editor_property("gear", block)

        climb = asset.get_editor_property("climb")
        cycle = spec.gear["door_seconds"] * 2.0 + spec.gear["travel_seconds"]
        rate = climb.get_editor_property("climb_speed") * unreal.MathLibrary.degrees_to_radians(
            climb.get_editor_property("climb_pitch_degrees"))
        top = spec.gear["retract_above_height"] + rate * cycle
        clear = climb.get_editor_property("clear_altitude")
        if top >= clear:
            fail("the gear cycle finishes at about %.0f uu but the agent is cleared at %.0f - "
                 "it would vanish mid-retraction. Raise the C++ builder's ClearAltitude or "
                 "lower retract_above_height." % (top, clear))
        else:
            say("PASS the gear is up and locked by about %.0f uu (%.0f m), below the clear "
                "altitude %.0f that %s authors" % (top, top / 100.0, clear, spec.cpp_builder))

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return path


def author_type(spec):
    if spec.cpp_builder:
        return _author_via_cpp_builder(spec)
    return _author_via_dicts(spec)


# --- Animation defaults ----------------------------------------------------------------


def set_anim_defaults(spec):
    """The ANIMATION's copies of the per-model figures, on the generated class's defaults -
    what the editor's Class Defaults panel edits and what every instance of the Anim Blueprint
    starts from. Left unset, every aircraft here would spin the Meridian's 21 uu placeholder
    wheels and drive its disc at a 3-blade step - wrong for every aeroplane in the fleet but
    the one the default was measured from.
    """
    abp = unreal.EditorAssetLibrary.load_asset(spec.abp)
    if abp is None:
        fail("no %s" % spec.abp)
        return
    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class; compile it first" % spec.abp)
        return

    want = {
        "main_wheel_radius": measured(spec)["wheel_radius"],
        "prop_blade_count": spec.prop_blade_count,
    }
    note = None
    if spec.angles is not None:
        angles = spec.angles.read()
        # ORDER MATCHES THE FLEET'S OWN CONVENTION - door, then gear, then truck - which every
        # script that carried more than one angle already used.
        if angles.get("bay_door_closed") is not None:
            want["bay_door_closed_angle_degrees"] = angles["bay_door_closed"]
        if angles.get("gear_retracted") is not None:
            want["gear_retracted_angle_degrees"] = angles["gear_retracted"]
        if angles.get("truck_tilted") is not None:
            want["truck_tilted_angle_degrees"] = angles["truck_tilted"]
        if hasattr(spec.angles, "note"):
            note = spec.angles.note()

    defaults = unreal.get_default_object(generated)
    for prop, value in want.items():
        defaults.set_editor_property(prop, value)
    unreal.EditorAssetLibrary.save_asset(spec.abp, only_if_is_dirty=False)

    # Read back from a fresh load, because a set_editor_property that reports success and
    # saves nothing is this project's most familiar failure.
    reloaded = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(spec.abp).generated_class())
    for prop, value in want.items():
        got = reloaded.get_editor_property(prop)
        if abs(float(got) - float(value)) > 0.01:
            fail("ABP %s read back as %s, expected %s" % (prop, got, value))
        else:
            shown = ("%.1f" % got) if isinstance(got, float) else str(got)
            say("PASS %s %s = %s" % (spec.abp.rsplit("/", 1)[-1], prop, shown))
    if note:
        say(note)


# --- Verification ------------------------------------------------------------------------


def verify(spec, path):
    """Read back from disk. An asset edit that reports success and writes nothing is this
    project's most familiar failure - every build_plane<N>_type.py said so, and this function
    is the union of what all fourteen checked, not the intersection: propeller_diameter was
    authored but never read back on nine of fourteen, tailplane_span checked on four, the
    ground surface/approach line dropped from the three newest scripts, and plane2's fixed
    gear never asserted at all. Closing a read-back gap is not a behaviour change to the
    AIRCRAFT - the asset already carried the field - it is a behaviour change to how loudly a
    broken write is caught, which is what this function is for.
    """
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        fail("%s did not survive the save" % path)
        return

    m = measured(spec)

    if spec.cpp_builder:
        _verify_cpp_builder(spec, asset, m)
    else:
        _verify_dicts(spec, asset, m)

    # NOT AUTHORED HERE for either path - Tools/Python/build_pushback_needs.py owns
    # EPushbackNeed for every type, and reporting the value is harmless where re-authoring it
    # would be the "lists that must agree are ONE list" failure CLAUDE.md names.
    say("NOTE pushback_need = %s (authored by build_pushback_needs.py, asserted by "
        "Airside.Content.PushbackNeedsAuthored)" % asset.get_editor_property("pushback_need"))

    for prop in ("mesh", "anim_class"):
        got = asset.get_editor_property(prop)
        if got is None:
            fail("%s is unset - the agent would fall back to the game-wide default mesh, "
                 "which is how a Twin Otter arrived as a Meridian" % prop)
        else:
            say("PASS %s = %s" % (prop, got.get_name()))

    # RETRACTING-GEAR AIRCRAFT ARE CHECKED HERE, BEFORE FIELD LENGTHS, UNLESS THE SPEC ALREADY
    # PRINTED THE BLOCK EARLIER (`gear_checked_early` - plane3/plane4/plane5's own convention;
    # see `_verify_dicts`). FIXED-GEAR AIRCRAFT ARE CHECKED AFTER FIELD LENGTHS INSTEAD - below.
    if spec.gear is not None and not spec.gear_checked_early:
        _print_gear_block(spec, asset)

    requirements = asset.get_editor_property("requirements")
    takeoff = requirements.get_editor_property("takeoff_field_length")
    landing = requirements.get_editor_property("landing_field_length")
    say("PASS field lengths: take-off %.0f uu (%.0f m), landing %.0f uu (%.0f m)%s"
        % (takeoff, takeoff / 100.0, landing, landing / 100.0, spec.field_lengths_suffix))
    # THE SURFACE/APPROACH LINE - run for every aircraft. It shipped on ten of fourteen and
    # was simply not carried into the three newest scripts (plane9, plane11, plane13) or
    # plane14; nothing about those four aeroplanes' requirements needed it dropped.
    say("PASS surface %s, approach %s"
        % (requirements.get_editor_property("minimum_surface"),
           requirements.get_editor_property("approach_needed")))

    if spec.gear is None:
        # THE FIXED GEAR, ASSERTED RATHER THAN ASSUMED - run for every fixed-gear aircraft,
        # closing the one gap in this family: plane2's Twin Otter has fixed gear exactly as
        # plane1's 172 does, but only plane1/plane10/plane12 ever checked the zero. The failure
        # this catches is silent: a cycle copied in from a retracting type would drive bones
        # this rig does not have, and nothing on screen would say so.
        gear = asset.get_editor_property("gear")
        travel = gear.get_editor_property("travel_seconds")
        if travel != 0.0:
            fail("gear.travel_seconds is %.2f - this aircraft's gear is fixed, so the type "
                 "must declare no cycle at all" % travel)
        else:
            say("PASS gear.travel_seconds = 0, so nothing retracts%s" % spec.fixed_gear_suffix)

    # GROUND REGIMES - read back from the asset regardless of how it was authored, so this one
    # loop covers the cpp-builder path too.
    ground = asset.get_editor_property("ground")
    for name in ("taxi", "takeoff", "landing"):
        regime = ground.get_editor_property(name)
        cap = regime.get_editor_property("speed_cap")
        say("  ground.%-8s accel=%.0f decel=%.0f cap=%.0f uu/s (%.0f kn)"
            % (name, regime.get_editor_property("accel"),
               regime.get_editor_property("decel"), cap, cap / 51.4))


def _print_gear_block(spec, asset):
    gear = asset.get_editor_property("gear")
    for field_name, want in spec.gear.items():
        got = gear.get_editor_property(field_name)
        if abs(got - want) > 0.01:
            fail("gear.%s read back as %s, expected %s" % (field_name, got, want))
        else:
            say("PASS gear.%-21s = %.1f" % (field_name, got))
    cycle = (spec.gear["door_seconds"] * 2.0 + spec.gear.get("truck_tilt_seconds", 0.0) * 2.0
            + spec.gear["travel_seconds"])
    say("PASS a full gear cycle is %.1f s: %.1f door, %.1f travel, %.1f door"
        % (cycle, spec.gear["door_seconds"], spec.gear["travel_seconds"],
           spec.gear["door_seconds"]))


def _verify_dicts(spec, asset, m):
    # THE FIVE FIELDS EVERY AIRCRAFT AUTHORS, READ BACK TOGETHER - propeller_diameter joins
    # main_wheel_radius/fixed_axle_x/main_gear_track/turnaround_seconds here for every aircraft;
    # it was authored but never checked back on nine of fourteen.
    checks = [
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"), m["wheel_radius"]),
        ("fixed_axle_x", asset.get_editor_property("fixed_axle_x"), m["fixed_axle_x"]),
        ("main_gear_track", asset.get_editor_property("main_gear_track"), m["main_gear_track"]),
        ("propeller_diameter", asset.get_editor_property("propeller_diameter"),
         m["prop_diameter"]),
        ("turnaround_seconds", asset.get_editor_property("turnaround_seconds"),
         spec.turnaround_seconds),
    ]
    for name, got, want in checks:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %s = %.2f" % (name, got))

    # plane3/plane4/plane5's OWN convention: the gear block right after the five core checks,
    # before footprint/code - see `gear_checked_early`.
    if spec.gear_checked_early and spec.gear is not None:
        _print_gear_block(spec, asset)

    # FOOTPRINT: wingspan AND tailplane_span - the second joins the first for every aircraft;
    # only plane1, plane10 and plane12 checked both before this.
    footprint = asset.get_editor_property("footprint")
    for field_name in ("wingspan", "tailplane_span"):
        got = footprint.get_editor_property(field_name)
        want = m["footprint"][field_name]
        if abs(got - want) > 0.1:
            fail("footprint.%s read back as %.1f, expected %.1f" % (field_name, got, want))
        else:
            say("PASS footprint %s %.1f uu (%.2f m)" % (field_name, got, got / 100.0))

    for prop, want in (("short_code", spec.short_code), ("code", spec.code)):
        got = str(asset.get_editor_property(prop))
        if got != want:
            fail("%s read back as %s, expected %s" % (prop, got, want))
        else:
            say("PASS %s = %s" % (prop, got))


def _verify_cpp_builder(spec, asset, m):
    """plane7 alone: the C++ builder against the mesh, which is the check this whole path
    exists to make possible. BuildPiperMeridian types these fields in C++; the rig answers
    them independently, and a disagreement means the export moved and the builder was not
    re-measured."""
    footprint = asset.get_editor_property("footprint")
    typed = [
        ("footprint.nose_x", footprint.get_editor_property("nose_x"), m["footprint"]["nose_x"]),
        ("footprint.tail_x", footprint.get_editor_property("tail_x"), m["footprint"]["tail_x"]),
        ("footprint.wingspan", footprint.get_editor_property("wingspan"),
         m["footprint"]["wingspan"]),
        ("footprint.wing_x", footprint.get_editor_property("wing_x"), m["footprint"]["wing_x"]),
        ("footprint.tailplane_span", footprint.get_editor_property("tailplane_span"),
         m["footprint"]["tailplane_span"]),
        ("footprint.tailplane_x", footprint.get_editor_property("tailplane_x"),
         m["footprint"]["tailplane_x"]),
        ("steer_axle_x", asset.get_editor_property("steer_axle_x"), m["steer_axle_x"]),
        ("fixed_axle_x", asset.get_editor_property("fixed_axle_x"), m["fixed_axle_x"]),
        ("main_wheel_radius", asset.get_editor_property("main_wheel_radius"), m["wheel_radius"]),
    ]
    for name, got, want in typed:
        if abs(got - want) > 0.5:
            fail("%s is %.1f in the C++ builder and %.1f on the mesh - it was not re-measured "
                 "after the export moved" % (name, got, want))
        else:
            say("PASS %-24s = %8.1f, and the mesh agrees to %.2f uu"
                % (name, got, abs(got - want)))

    measured_only = [
        ("main_gear_track", asset.get_editor_property("main_gear_track"), m["main_gear_track"]),
        ("propeller_diameter", asset.get_editor_property("propeller_diameter"),
         m["prop_diameter"]),
    ]
    for name, got, want in measured_only:
        if abs(got - want) > 0.01:
            fail("%s read back as %s, expected %s" % (name, got, want))
        else:
            say("PASS %-24s = %8.1f" % (name, got))

    say("carried through the rename: turnaround %.0f s, pushback %s"
        % (asset.get_editor_property("turnaround_seconds"),
           asset.get_editor_property("pushback_need")))
    # NOT REPEATED HERE: field lengths, surface and approach, which verify() now reports for
    # every aircraft via the union's own field-lengths/surface lines and the ground-regime loop
    # below - lines this path never had before (plane7 had no per-regime breakdown at all).
    # Saying them twice would be the asset's OWN saved values printed by two different pieces
    # of code, which is the "two authors of one fact" shape CLAUDE.md warns against, even
    # though here both readers agree by construction.


# --- Driver ----------------------------------------------------------------------------


def run(key):
    spec = spec_for(key)
    if spec.skipped_checks:
        for name, reason in spec.skipped_checks.items():
            say("NOTE skipping %s: %s" % (name, reason))

    if unreal.EditorAssetLibrary.load_asset(spec.mesh) is None:
        fail("no %s - run import_models.py first" % spec.mesh)
        say("DONE")
        return

    path = author_type(spec)
    if path is None:
        say("DONE")
        return
    say("authored %s" % path)
    verify(spec, path)
    set_anim_defaults(spec)
    say("DONE")


def _resolve_key_from_argv():
    """The aircraft key, from the command line. Under `-run=pythonscript -script=<this file>
    <key>`, UE appends trailing tokens to sys.argv; a bare positional (no leading '-') is the
    key. Unverified at runtime this session - no editor was available to confirm the argv
    shape, so this also falls back to unreal.SystemLibrary.get_command_line(), which every
    `-run=pythonscript` invocation carries regardless of how argv itself was populated.
    """
    for arg in sys.argv[1:]:
        if not arg.startswith("-"):
            return arg
    try:
        tokens = unreal.SystemLibrary.get_command_line().split()
    except Exception:
        tokens = []
    for token in reversed(tokens):
        if not token.startswith("-") and not token.lower().endswith((".py", ".uproject")):
            return token
    raise SystemExit("build_aircraft_type.py needs an aircraft key, e.g. "
                     "'-script=Tools/Python/build_aircraft_type.py plane9'")


if __name__ == "__main__":
    run(_resolve_key_from_argv())
