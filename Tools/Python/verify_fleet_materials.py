"""Reads the fleet material set BACK off disk and checks it against the .glb. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Separate from build_fleet_materials.py on purpose: that script's verify can only report what
the same process just wrote, and the failure guarded against here is silent.
unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value NO-OPS when the
parameter name does not exist on the parent - no exception, no return value to test - so a
typo'd name yields an instance that saves cleanly, reports success, and renders with the
master's defaults. The whole fleet would be one shade of white.

So the question is not "did the write happen" but "does the value on disk equal the value in
the .glb", which can only come out right if the parameter exists, took the value, and
survived the save.

Also asserts the two things that are invisible until they bite:

  - TWO_SIDED on the master. Every source material is doubleSided; without it, backfaces are
    culled and thin open surfaces render as the unlit inside of the far skin. That is what
    turned plane2's wing black.
  - bUsedWithSkeletalMesh. Without it the renderer substitutes the default material and the
    model draws grey clay - see build_aircraft_looks.py.

TOLERANCE MATCHES THE BUILD. Instances are merged by value within MERGE_TOL, so a slot may
legitimately read a value up to that far from its own .glb entry. Checking tighter than the
build merges would fail every merged look. The worst deviation seen is reported so the drift
stays visible rather than being hidden by the tolerance that permits it.
"""
import json
import os
import struct
import sys

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import FLEET, MERGE_TOL, glb_path  # noqa: E402

import unreal

MAT_DIR = "/Game/Materials/Fleet"
MASTER_PATH = "%s/M_Fleet" % MAT_DIR
OLD_DIR = "/Game/Aircraft/Materials"
# FLEET, THE TOLERANCE AND glb_path COME FROM airside_import, and all three used to be typed
# or duplicated here - glb_path assumed folder == file, which is wrong for tankTrailer1 (see
# airside_import.glb_path's own comment).
#
# THE COPY WAS NOT HARMLESS. This file's FLEET listed plane2 onwards; when plane1 joined the
# BUILDER on 2026-09-19 it was not added here, so this script checked seven assets, printed
# seven PASS lines and reported "DONE with 0 problem(s)" while the eighth went unexamined. A
# verifier that silently skips what it does not know about is worse than no verifier, because
# the green line is read as coverage. It now iterates whatever the builder built.
#
# The tolerance was the same shape - "0.005 + 1e-6  # must equal build_fleet_materials.
# MERGE_TOL", a comment asking a human to keep two numbers equal. The 1e-6 stays here,
# because it is this file's own concern: a slot merged at exactly MERGE_TOL must not fail for
# a floating-point last bit.
TOL = MERGE_TOL + 1e-6


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def gltf_materials(stem):
    path = glb_path(stem)
    out = {}
    with open(path, "rb") as handle:
        handle.read(12)
        length = struct.unpack("<I4s", handle.read(8))[0]
        doc = json.loads(handle.read(length).decode("utf-8"))
    for mat in doc.get("materials", []):
        pbr = mat.get("pbrMetallicRoughness", {})
        base = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
        out[mat.get("name", "?")] = ([float(c) for c in base[:3]],
                                     float(pbr.get("metallicFactor", 1.0)),
                                     float(pbr.get("roughnessFactor", 1.0)))
    return out


def run():
    lib = unreal.MaterialEditingLibrary
    problems, worst = 0, (0.0, "")

    master = unreal.EditorAssetLibrary.load_asset(MASTER_PATH)
    if master is None:
        fail("no %s" % MASTER_PATH)
        return
    vectors = [str(n) for n in lib.get_vector_parameter_names(master)]
    scalars = [str(n) for n in lib.get_scalar_parameter_names(master)]
    say("M_Fleet parameters: vector %s scalar %s" % (vectors, scalars))
    for want, have in (("BaseColor", vectors), ("Metallic", scalars), ("Roughness", scalars)):
        if want not in have:
            fail("M_Fleet has no '%s' parameter - a dynamic instance could not drive it" % want)
            problems += 1
    if not master.get_editor_property("two_sided"):
        fail("M_Fleet is not two_sided - thin surfaces will render black")
        problems += 1
    if not master.get_editor_property("used_with_skeletal_mesh"):
        fail("M_Fleet lacks bUsedWithSkeletalMesh - the fleet draws grey clay")
        problems += 1

    for asset, (stem, mesh_path) in sorted(FLEET.items()):
        source = gltf_materials(stem)
        mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
        if mesh is None:
            say("%s absent; skipped" % mesh_path)
            continue
        slots = mesh.get_editor_property("materials")
        checked = 0
        for slot in slots:
            slot_name = str(slot.material_slot_name)
            mi = slot.material_interface
            expected = source.get(slot_name)
            if expected is None:
                fail("%s: slot '%s' is not in the glb" % (asset, slot_name))
                problems += 1
                continue
            if mi is None or not isinstance(mi, unreal.MaterialInstanceConstant):
                fail("%s: slot '%s' is not a material instance" % (asset, slot_name))
                problems += 1
                continue
            base = mi.get_base_material()
            if base is None or base.get_path_name().split(".")[0] != MASTER_PATH:
                fail("%s: slot '%s' -> %s is not parented to M_Fleet"
                     % (asset, slot_name, mi.get_name()))
                problems += 1
                continue
            c = lib.get_material_instance_vector_parameter_value(mi, "BaseColor")
            m = lib.get_material_instance_scalar_parameter_value(mi, "Metallic")
            r = lib.get_material_instance_scalar_parameter_value(mi, "Roughness")
            rgb, metallic, rough = expected
            delta = max(abs(c.r - rgb[0]), abs(c.g - rgb[1]), abs(c.b - rgb[2]),
                        abs(m - metallic), abs(r - rough))
            if delta > worst[0]:
                worst = (delta, "%s.%s -> %s" % (asset, slot_name, mi.get_name()))
            if delta > TOL:
                fail("%s: slot '%s' -> %s is %.4f off its glb value"
                     % (asset, slot_name, mi.get_name(), delta))
                problems += 1
            else:
                checked += 1
        say("PASS %-11s %d/%d slot(s) within %.3f of the glb" % (asset, checked, len(slots), TOL))

    # Instances holding the same numbers under different names: one look that two assets
    # spell differently upstream. Not an error - it is a Blender naming job.
    seen = {}
    for path in unreal.EditorAssetLibrary.list_assets(MAT_DIR, recursive=False):
        mi = unreal.EditorAssetLibrary.load_asset(path)
        if not isinstance(mi, unreal.MaterialInstanceConstant):
            continue
        c = lib.get_material_instance_vector_parameter_value(mi, "BaseColor")
        key = (round(c.r, 3), round(c.g, 3), round(c.b, 3),
               round(lib.get_material_instance_scalar_parameter_value(mi, "Metallic"), 3),
               round(lib.get_material_instance_scalar_parameter_value(mi, "Roughness"), 3))
        seen.setdefault(key, []).append(mi.get_name())
    for key, users in sorted(seen.items()):
        if len(users) > 1:
            say("NOTE %s hold identical values %s - one look under two names upstream"
                % (" and ".join(sorted(users)), key))

    leftovers = [p.split("/")[-1] for p in unreal.EditorAssetLibrary.list_assets(
        OLD_DIR, recursive=False)] if unreal.EditorAssetLibrary.does_directory_exist(OLD_DIR) else []
    if leftovers:
        say("NOTE %s still holds %d orphan(s): %s"
            % (OLD_DIR, len(leftovers), ", ".join(sorted(leftovers))))

    say("worst deviation %.5f at %s" % worst)
    say("DONE with %d problem(s)" % problems)


run()
