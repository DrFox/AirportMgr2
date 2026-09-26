"""Creates ABP_<Key>, parented to UAirsideAgentAnim and targeting its skeleton, and resolves
the rotation axis its AnimGraph must drive each bone about, for whichever key names a module
under aircraft/. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=Tools/Python/build_aircraft_anim.py
      -unattended -nosplash -nopause <key>

Every result line is prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.
NOTE those lines land in the log, not on stdout - the commandlet's stdout carries only
LogInit and errors, so a run that looks silent has usually worked.

ONE MECHANISM, FOURTEEN SPECS - Issue #294, `build_aircraft_type.py <key>`'s sibling for the
same `aircraft/<key>.py` spec (Issue #290 / PR #334). build_plane1_anim.py through
build_plane14_anim.py were 2,675 lines that were the same script fourteen times over: `run()`,
`verify()`, the create-or-load asset dance and the "read the axis plan / print the mode notes"
prose were byte-identical across all but two of the fleet, DRIVEN_BONES retyped what
airside_anim.report_bone_plan(joint_names(SOURCE)) already computes a page below it, and one
copy-paste survived unnoticed for a release: build_plane14_anim.py's RAKED was correctly ()
but its say() line insisted "ONE BONE IS RAKED ON PURPOSE" regardless, copied from plane9's or
plane12's script and never re-typed when RAKED was.

WHAT LIVES WHERE. This file is the MECHANISM: it knows how to create or load an Anim
Blueprint, read a rig's bone hierarchy off its own .glb, resolve each driven bone's rotation
axis against the imported skeleton, and write the result to Saved/<key>_axis_plan.json for
Tools/wire_plane_anim.py to wire. It knows nothing about any one aeroplane.
`Tools/Python/aircraft/<key>.py` is the SPEC build_aircraft_type.py already reads for the
TYPE asset - `mesh`, `source` and `abp` name the same rig for both scripts, because the rig
does not change shape depending on which script is asking. This file adds five anim-only
fields to that same `AircraftSpec` (see its own docstring): `raked`, `excluded`,
`anim_multiplier`, `wire_script` and `create_abp`.

DRIVEN_BONES IS NO LONGER TYPED. `airside_anim.driven_bone_plan()` derives the (bone,
variable) set AND its CHAIN ORDER from the .glb's own joint order - every joint a BONE_RULES
needle matches, less any name in `spec.excluded` - rather than a human copying
report_bone_plan()'s output into a second list by hand. See that function's own docstring for
the preorder-reversal argument and Issue #294's harness for the fourteen-rig check that backs
it.

THE MULTIPLIER COLUMN IS DERIVED TOO, from `spec.anim_multiplier` and the fleet's own
convention - see `airside_anim.anim_multiplier_for()`. Only plane8 supplies overrides; the
other thirteen get -1.0 (Blender/UE handedness) on every travelling bone and None on the
steer bone, exactly as their retired wire_plane<N>_anim.py scripts typed by hand.

TWO AIRCRAFT ARE NOT LIKE THE OTHER TWELVE. plane1 and plane2 (`spec.wire_script = False`)
were wired by hand in the editor before this tooling existed and carry no
Tools/wire_<key>_anim.py counterpart at all - this script prints their bone plan for a human
to wire and stops there, exactly as build_plane1_anim.py and build_plane2_anim.py did. plane4
(`spec.create_abp = False`) predates the tooling in the OTHER direction: ABP_Plane4 was
duplicated from ABP_Plane2 in the editor on 2026-09-19 and retargeted by hand, so this script
measures an asset that must already exist rather than inventing an empty one that would point
DA_Aircraft_Plane4 at a Blueprint driving nothing.
"""
import os
import sys

import unreal

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see
# import_models.py's header. Put it on before importing the shared modules.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import (  # noqa: E402
    anim_multiplier_for, driven_bone_plan, fail, joint_names, raked_line, report_bone_plan,
    resolve_axis, say, write_axis_plan)
from build_aircraft_type import spec_for  # noqa: E402


def skeleton_path(spec):
    """spec.mesh's own skeleton - every rig in the fleet names it MESH + "_Skeleton", checked
    against all fourteen build_plane<N>_anim.py's own SKELETON constants before this replaced
    them, so this is read off the one path already in the spec rather than typed a second
    time."""
    return spec.mesh + "_Skeleton"


def abp_path_and_name(spec):
    path, name = spec.abp.rsplit("/", 1)
    return path, name


def _say_modes():
    say("")
    say("  ON EVERY ONE OF THOSE NODES:")
    say("    Translation Mode = IGNORE            <- leave it alone")
    say("    Rotation Mode    = ADD TO EXISTING   <- not Replace")
    say("    Scale Mode       = IGNORE")
    say("    Rotation Space   = Bone Space")
    say("")
    say("  TWO MODES BITE, and both were got wrong once on ABP_Plane2:")
    say("   * Translation on Replace writes the default (0,0,0), snapping each bone to its")
    say("     parent - root, on the ground - so the props and wheels detach and lie under")
    say("     the aeroplane. Their bind-pose positions ARE the spin axes.")
    say("   * Rotation on Replace discards the bone's bind-pose ORIENTATION and sets it to")
    say("     the rotator given, so every part reorients the same way and they all end up")
    say("     pointing up. Add to Existing turns each part from where the rig left it.")
    say("  Add is right because the angle is an ACCUMULATED absolute value, wrapped to")
    say("  0..360 - bind plus angle each frame, not a per-frame delta that adding would")
    say("  integrate twice.")


def report_plan(spec):
    """The graph this asset wants, as a list rather than a memory of one."""
    abp_name = abp_path_and_name(spec)[1]
    say("")
    if spec.wire_script:
        say("THE GRAPH THIS ASSET WANTS. Tools/wire_plane_anim.py %s authors it against the"
            % spec.key)
        say("  RUNNING editor and reads it back; this list is what it should agree with.")
        say("  One Transform (Modify) Bone per row, Rotation driven by:")
    else:
        # HAND-WIRED, LONG BEFORE THIS TOOLING EXISTED - plane1/plane2 alone. There is no
        # wire_plane_anim.py support for either, so the framing says what it always did:
        # the editor's `unreal` module cannot make graph nodes, and this is a by-hand job.
        say("STILL TO DO - this commandlet's `unreal` module cannot make graph nodes.")
        say("  MCP CAN, against the running editor: docs/2026-09-20-animgraph-authoring.md.")
        say("  By hand, it is:")
        say("  open %s and, in AnimGraph, add one Transform (Modify) Bone per row:" % abp_name)

    unrecognised = report_bone_plan(joint_names(spec.source))
    say("")
    if unrecognised:
        fail("%d joint(s) above match no rule - either the rig gained a part the sim cannot "
             "drive, or a bone was renamed" % unrecognised)
    else:
        say("PASS every joint maps to a UAirsideAgentAnim property; nothing is unwirable")

    if not spec.wire_script:
        # No axis measurement, no Saved/<key>_axis_plan.json, because there is no wiring
        # script waiting to read one - see AircraftSpec.wire_script.
        if spec.anim_report_extra is not None:
            spec.anim_report_extra(say)
        _say_modes()
        return

    say("")
    say("  THE ROTATION AXIS PER BONE, resolved off %s's reference pose in UE's own frame."
        % spec.mesh.rsplit("/", 1)[-1])
    say("    Wiring a bone about the wrong axis reads as a modelling fault, not a wiring")
    say("    one, so it is measured, not assumed. %s" % raked_line(spec.raked))

    matched = driven_bone_plan(spec.source, excluded=spec.excluded)
    driven = [bone for bone, _ in matched]
    variable_of = dict(matched)
    resolved = resolve_axis(spec.mesh, driven, raked=spec.raked)
    for bone, component, sign in resolved:
        say("    %-16s  %-5s  x %+.0f" % (bone, component, sign))
    if len(resolved) != len(driven):
        fail("resolved %d of %d driven bones - the rest are logged above"
             % (len(resolved), len(driven)))
    else:
        say("    PASS all %d driven bones resolve to one local axis" % len(resolved))
        rows = [(bone, variable_of[bone], component, sign,
                 anim_multiplier_for(bone, variable_of[bone], spec.anim_multiplier))
                for bone, component, sign in resolved]
        write_axis_plan(spec.key, spec.mesh, rows)

    _say_modes()
    say("")
    say("  ORDER: a bone goes AFTER every bone it is the PARENT of - CHAIN ORDER, derived")
    say("    from the .glb's own joint hierarchy by driven_bone_plan() rather than typed.")


def verify(spec, asset, skeleton):
    """Check what is on disk, whether this run created it or found it."""
    abp_name = abp_path_and_name(spec)[1]
    got = asset.get_editor_property("target_skeleton")
    if got != skeleton:
        fail("target_skeleton is %s, expected %s" % (got, skeleton))
    else:
        say("PASS %s targets %s" % (abp_name, skeleton_path(spec).rsplit("/", 1)[-1]))

    parent = unreal.BlueprintEditorLibrary.get_blueprint_parent_class(asset)
    say("parent class: %s" % parent)
    if parent is None or "AirsideAgentAnim" not in str(parent):
        fail("parent is not UAirsideAgentAnim - the graph would see none of the angles")
    else:
        say("PASS parent is UAirsideAgentAnim, so the graph sees the angles it applies")

    graphs = [str(g) for g in unreal.BlueprintEditorLibrary.list_graph_names(asset)]
    say("graphs: %s" % ", ".join(graphs))


def run(key):
    spec = spec_for(key)
    skeleton = unreal.EditorAssetLibrary.load_asset(skeleton_path(spec))
    if skeleton is None:
        fail("no skeleton at %s - run import_models.py first" % skeleton_path(spec))
        say("DONE")
        return

    path = spec.abp
    existing = unreal.EditorAssetLibrary.load_asset(path)
    if existing is not None:
        # LOAD, never replace: DA_Aircraft_<Key> points at this asset's generated class, and
        # Tools/wire_plane_anim.py rebuilds the GRAPH inside it without replacing it.
        say("%s already exists; leaving it alone so its authored graph survives" % path)
        verify(spec, existing, skeleton)
        report_plan(spec)
        say("DONE")
        return

    if not spec.create_abp:
        # plane4 ALONE - see the module docstring. An ABP_Plane4 that is MISSING is a checkout
        # problem or a deletion, and inventing an empty one would point DA_Aircraft_Plane4 at
        # a Blueprint that drives nothing while reporting success.
        fail("no %s. This script measures an asset that already exists; it does not create "
             "one for this aircraft. Restore it from git rather than letting a fresh empty "
             "Blueprint take its place." % path)
        say("DONE")
        return

    abp_folder, abp_name = abp_path_and_name(spec)
    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    # THE PARENT IS THE POINT: UAirsideAgentAnim is what computes the angles; an Anim
    # Blueprint left on plain UAnimInstance compiles, runs, and exposes none of them.
    factory.set_editor_property("parent_class", unreal.AirsideAgentAnim)

    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        abp_name, abp_folder, unreal.AnimBlueprint, factory)
    if asset is None:
        fail("could not create %s" % path)
        say("DONE")
        return

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # Read back from disk: an asset that reports success and saves nothing is this
    # project's most familiar failure.
    reloaded = unreal.EditorAssetLibrary.load_asset(path)
    if reloaded is None:
        fail("%s did not survive the save" % path)
        say("DONE")
        return

    say("created %s" % path)
    verify(spec, reloaded, skeleton)
    report_plan(spec)
    say("DONE")


def _resolve_key_from_argv():
    """The aircraft key, from the command line - see build_aircraft_type.py's own copy of
    this function for the argv-shape note this is unchanged from."""
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
    raise SystemExit("build_aircraft_anim.py needs an aircraft key, e.g. "
                     "'-script=Tools/Python/build_aircraft_anim.py plane9'")


if __name__ == "__main__":
    run(_resolve_key_from_argv())
