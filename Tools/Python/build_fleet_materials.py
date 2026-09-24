"""One parameterised master material for every flat-colour asset - aircraft AND ground
vehicles - plus one instance per distinct look, with every value SCRAPED FROM THE .glb.
Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

WHY THIS EXISTS, given that import_plane2.py says materials are NOT authored here.

That note is right about what it was defending against: hand-typing base colours into Unreal
is a second transcription of numbers that already live in Blender, and the two drift the
first time the model is re-exported. Nothing below types a colour. Values are read out of the
exported .glb, so Blender stays the single source of truth and a re-export followed by a
re-run of this script propagates a change with no hand editing.

What changed is the requirement. Interchange's glTF pipeline delivered exactly what it was
asked for, and it is unusable for three reasons:

  - NOT DRIVEABLE AT RUNTIME. The generated assets are full UMaterials, not instances -
    `"bCreateMaterialInstanceForParent": false` is visible in each .uasset - with the factors
    wired in as constants. No named parameter to set, and a UMaterial cannot be changed at
    runtime anyway. Per-airline liveries need a parameter and a dynamic instance, which means
    a master this project owns.
  - ONE UBER-GRAPH PER FLAT COLOUR. ~48 KB each, carrying iridescence, anisotropy, normals,
    occlusion, specular and UV transforms to express three constants.
  - DUPLICATED ACROSS ASSETS. Six aircraft looks were identical pairs; across the vehicles it
    is worse - six assets each carry their own private black tyre.

TWO-SIDED, AND THIS IS WHAT TURNED plane2's WING BLACK. Every material in every export is
`doubleSided: true` (61 of 61 across seven .glb files), so Interchange set two_sided on each
generated material. The first version of this script left the master at the default, false.
Backfaces were then culled, and a thin open surface like plane2's wing renders as the unlit
inside of the far skin - black. The flag is asserted from the source below rather than
hard-coded true, so a future single-sided export is reported rather than silently forced.

MERGED BY VALUE, NOT BY NAME, WITH A TOLERANCE. Grouping by name alone is wrong twice over:
`metal` means chrome on plane3 (0.62, M 0.90) and dull steel on the trucks (0.55, M 0.85),
while `tyre` is the SAME black on all six assets but written 0.0150 on some and 0.0152 on
others. That 0.0002 is copy-by-hand drift, not intent - the fleet palette was propagated by
lifting numbers between .blend files. So looks are clustered by value within MERGE_TOL, and
any merge that was not exact is reported, because the drift is worth fixing upstream even
though it is invisible on screen.

NOT THE PIPER, which ships real texture maps and hand-authored M_PiperMeridian/M_PiperGlass -
a master of three constants has nothing to offer it. It is excluded by absence from FLEET
rather than by a rule, so adding a textured asset stays a decision rather than an accident.

PLANE1 USED TO BE EXCLUDED HERE TOO, "superseded by plane3 and not in Content", and the
entry stood for a week after all three of its claims stopped being true. It was re-exported
on 2026-09-19 with a skin, ten named plane1_* materials and a Cessna 172's own dimensions,
and it is not superseded by anything - a 172 and a Dash 8 are not the same aeroplane at
different sizes. An exclusion note is a claim about the world, and this one aged badly
because nothing re-reads it when the world changes.

THE SKELETAL FLAG, ONCE. build_aircraft_looks.py documents the trap where a material without
bUsedWithSkeletalMesh makes the renderer substitute the default and the model draws grey clay,
and has to walk every material on every mesh setting it. With one master it is set once here
and every instance inherits it.

LINEAR, NOT sRGB. glTF baseColorFactor is linear and unreal.LinearColor is linear, so values
go straight across. Converting would visibly lighten the whole fleet.

IN PLACE, never delete-and-recreate. Instances reference the master and meshes reference the
instances, so deleting either strands live references rather than updating them.
"""
import json
import os
import struct
import sys

# THE SCRIPT'S OWN DIRECTORY IS NOT ON sys.path under -run=pythonscript - see import_models.py
# for the full note. __file__ is present both when run as -script= and when airside_import's
# rebuild_fleet_materials() exec's this file, because that exec passes one in.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import FLEET, MERGE_TOL, PRETTY, glb_path  # noqa: E402

import unreal

MAT_DIR = "/Game/Materials/Fleet"
MASTER_NAME = "M_Fleet"
MASTER_PATH = "%s/%s" % (MAT_DIR, MASTER_NAME)

# Where the first version of this script put things, before ground vehicles joined and
# "/Game/Aircraft/..." stopped being an honest home for a tyre shared with a fuel truck.
OLD_DIR = "/Game/Aircraft/Materials"
OLD_MASTER = "%s/M_Aircraft" % OLD_DIR

# FLEET, PRETTY, MERGE_TOL and glb_path now come from airside_import - see the note there.
# FLEET and MERGE_TOL lived here, with a second copy in verify_fleet_materials.py, until
# plane1 was added to one and not the other; glb_path was a THIRD copy that silently ignored
# the FLEET tuple's own stem field (see airside_import.glb_path's own comment).
EXACT_TOL = 1e-6

# Slot names in Content that a LATER export renamed. Interchange names a slot after the
# source material, so renaming a material in Blender leaves the already-imported mesh
# holding the old name and the remap below stops matching it - the slot silently keeps
# whatever it had. Renaming the slot here fixes that without a re-import, and is safe
# because mesh sections index slots by ORDER, never by name.
#
# plane3_metal -> plane3_strut: `metal` already meant dull steel on the trucks (0.55,
# M 0.85) while plane3's is chrome (0.62, M 0.90), which is the look plane2 calls `strut`.
# One name for two values is what produced MI_Metal_FuelTruck1 / MI_Metal_Plane3.
#
# An entry here can be deleted once that mesh has been re-imported from the newer export.
SLOT_RENAMES = {
    "/Game/Aircraft/Plane3/SK_Plane3": {"plane3_metal": "plane3_strut"},
}


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def gltf_materials(path):
    """name -> (rgb, metallic, roughness, double_sided), read from the file itself.

    A glb is a 12-byte header then a length-prefixed JSON chunk; no dependency needed, and
    the editor's Python has no glTF reader. Same approach import_plane2.py uses for the skin.
    """
    out = {}
    try:
        with open(path, "rb") as handle:
            handle.read(12)
            length = struct.unpack("<I4s", handle.read(8))[0]
            doc = json.loads(handle.read(length).decode("utf-8"))
    except Exception as exc:
        fail("could not read %s: %s" % (path, exc))
        return out
    for mat in doc.get("materials", []):
        pbr = mat.get("pbrMetallicRoughness", {})
        base = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
        out[mat.get("name", "?")] = (
            tuple(float(c) for c in base[:3]),
            float(pbr.get("metallicFactor", 1.0)),
            float(pbr.get("roughnessFactor", 1.0)),
            bool(mat.get("doubleSided", False)),
        )
    return out


def look_of(asset, material_name):
    head = asset + "_"
    return material_name[len(head):] if material_name.startswith(head) else material_name


def flat(values):
    rgb, metallic, rough = values[0], values[1], values[2]
    return (rgb[0], rgb[1], rgb[2], metallic, rough)


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(flat(a), flat(b)))


def instance_name(look, cluster_lead, shared):
    """MI_Tyre when one cluster serves everybody; MI_Body_Plane2 when a look has several
    genuinely different values and the name alone can no longer identify one."""
    camel = "".join(part.capitalize() for part in look.split("_"))
    return "MI_%s" % camel if shared else "MI_%s_%s" % (camel, PRETTY[cluster_lead])


def collect():
    """look -> [cluster], each cluster = (values, [asset], [slot name], exact?)."""
    per_look, sided = {}, []
    for asset in sorted(FLEET):
        path = glb_path(FLEET[asset][0])
        if not os.path.exists(path):
            fail("%s missing - %s not scraped" % (path, asset))
            continue
        found = gltf_materials(path)
        say("%-11s %2d material(s) scraped" % (asset, len(found)))
        for name, values in sorted(found.items()):
            sided.append((asset, name, values[3]))
            per_look.setdefault(look_of(asset, name), []).append((asset, name, values))

    single = [(a, n) for a, n, d in sided if not d]
    if single:
        fail("not every material is doubleSided (%s) - one two-sided master can no longer "
             "serve the fleet; see the docstring" % ", ".join("%s.%s" % q for q in single))
    say("doubleSided: %d/%d material(s)" % (len(sided) - len(single), len(sided)))

    out = {}
    for look, members in sorted(per_look.items()):
        clusters = []
        for asset, name, values in members:
            for cluster in clusters:
                if close(cluster["values"], values, MERGE_TOL):
                    cluster["assets"].append(asset)
                    cluster["slots"].append(name)
                    if not close(cluster["values"], values, EXACT_TOL):
                        cluster["exact"] = False
                    break
            else:
                clusters.append({"values": values, "assets": [asset], "slots": [name],
                                 "exact": True})
        out[look] = clusters
    return out, len(single) == 0


def ensure_master(two_sided):
    lib = unreal.MaterialEditingLibrary
    # Migrate the aircraft-only master rather than making a second one; rename_asset fixes
    # up the instances that already point at it.
    if (not unreal.EditorAssetLibrary.does_asset_exist(MASTER_PATH)
            and unreal.EditorAssetLibrary.does_asset_exist(OLD_MASTER)):
        if unreal.EditorAssetLibrary.rename_asset(OLD_MASTER, MASTER_PATH):
            say("moved %s -> %s" % (OLD_MASTER, MASTER_PATH))
        else:
            fail("could not move %s" % OLD_MASTER)

    if unreal.EditorAssetLibrary.does_asset_exist(MASTER_PATH):
        master = unreal.EditorAssetLibrary.load_asset(MASTER_PATH)
        say("%s exists; graph left alone (instances reference it)" % MASTER_NAME)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        master = tools.create_asset(MASTER_NAME, MAT_DIR, unreal.Material,
                                    unreal.MaterialFactoryNew())
        if master is None:
            fail("create_asset returned None for %s" % MASTER_PATH)
            return None
        base = lib.create_material_expression(
            master, unreal.MaterialExpressionVectorParameter, -400, -200)
        base.set_editor_property("parameter_name", "BaseColor")
        base.set_editor_property("default_value", unreal.LinearColor(0.85, 0.85, 0.84, 1.0))
        metallic = lib.create_material_expression(
            master, unreal.MaterialExpressionScalarParameter, -400, 0)
        metallic.set_editor_property("parameter_name", "Metallic")
        metallic.set_editor_property("default_value", 0.0)
        rough = lib.create_material_expression(
            master, unreal.MaterialExpressionScalarParameter, -400, 120)
        rough.set_editor_property("parameter_name", "Roughness")
        rough.set_editor_property("default_value", 0.32)
        lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
        lib.connect_material_property(metallic, "", unreal.MaterialProperty.MP_METALLIC)
        lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
        say("built %s" % MASTER_PATH)

    changed = False
    if master.get_editor_property("two_sided") != two_sided:
        master.set_editor_property("two_sided", two_sided)
        say("%s two_sided -> %s (this is what fixes the black wing)" % (MASTER_NAME, two_sided))
        changed = True
    if not master.get_editor_property("used_with_skeletal_mesh"):
        master.set_editor_property("used_with_skeletal_mesh", True)
        say("%s flagged for skeletal use" % MASTER_NAME)
        changed = True
    if changed:
        lib.recompile_material(master)
    unreal.EditorAssetLibrary.save_asset(MASTER_PATH, only_if_is_dirty=False)
    return master


def ensure_instance(master, name, values):
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, name)
    old = "%s/%s" % (OLD_DIR, name)
    if (not unreal.EditorAssetLibrary.does_asset_exist(path)
            and unreal.EditorAssetLibrary.does_asset_exist(old)):
        unreal.EditorAssetLibrary.rename_asset(old, path)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mi = unreal.EditorAssetLibrary.load_asset(path)
    else:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        mi = tools.create_asset(name, MAT_DIR, unreal.MaterialInstanceConstant,
                                unreal.MaterialInstanceConstantFactoryNew())
        if mi is None:
            fail("create_asset returned None for %s" % path)
            return None
    lib.set_material_instance_parent(mi, master)
    rgb, metallic, rough = values[0], values[1], values[2]
    lib.set_material_instance_vector_parameter_value(
        mi, "BaseColor", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
    lib.set_material_instance_scalar_parameter_value(mi, "Metallic", metallic)
    lib.set_material_instance_scalar_parameter_value(mi, "Roughness", rough)
    lib.update_material_instance(mi)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    return mi


def remap(mesh_path, by_slot):
    """Point every slot at its instance, MATCHED BY SLOT NAME. Slot ORDER is preserved and
    never relied on: it is what the mesh sections index, so reordering repaints at random."""
    mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    if mesh is None:
        say("%s absent; skipped" % mesh_path)
        return 0, []
    renames = SLOT_RENAMES.get(mesh_path, {})
    original = mesh.get_editor_property("materials")
    out, moved, left, renamed = [], 0, [], []
    for slot in original:
        slot_name = str(slot.material_slot_name)
        if slot_name in renames:
            renamed.append("%s->%s" % (slot_name, renames[slot_name]))
            slot_name = renames[slot_name]
        mi = by_slot.get(slot_name)
        if mi is None:
            left.append(slot_name)
            out.append(slot)
            continue
        out.append(unreal.SkeletalMaterial(
            material_interface=mi, material_slot_name=unreal.Name(slot_name)))
        moved += 1
    if renamed:
        say("%s: slot(s) renamed to match the newer export: %s"
            % (mesh_path.split("/")[-1], ", ".join(renamed)))

    # SAVE ONLY WHEN A SLOT ACTUALLY CHANGED, not unconditionally like this used to.
    #
    # THE INCIDENT THIS GUARDS AGAINST (2026-09-24): adding truckCab1/tankTrailer1 to FLEET
    # meant re-running this across the WHOLE fleet (the established workflow -
    # import_models.py's own tail does the same after every import), and the unconditional
    # set_editor_property + save_asset below touched all 14 meshes and every pre-existing
    # MI_* instance regardless of whether that particular asset's own slots changed - 36
    # files of binary re-save noise landed in a PR whose only real content was two new
    # assets, and had to be reverted by hand afterwards. set_editor_property on the materials
    # TArray marks the package dirty even when the rebuilt array is VALUE-IDENTICAL to what
    # was already there, and save_asset(only_if_is_dirty=False) writes regardless of
    # dirtiness anyway - so nothing here previously distinguished "this mesh's slots changed"
    # from "this mesh was merely visited by the loop". Comparing the rebuilt list against
    # what was already on the mesh, index for index, is what makes that distinction; an
    # untouched mesh is skipped and never dirtied at all.
    changed = (len(out) != len(original) or any(
        a.material_interface != b.material_interface
        or str(a.material_slot_name) != str(b.material_slot_name)
        for a, b in zip(out, original)))
    if not changed:
        say("%s: no slot changed; left untouched" % mesh_path.split("/")[-1])
        return moved, left

    mesh.set_editor_property("materials", out)
    unreal.EditorAssetLibrary.save_asset(mesh_path, only_if_is_dirty=False)
    return moved, left


def run():
    looks, uniform_sided = collect()
    if not looks:
        fail("nothing scraped; stopping before touching any asset")
        return
    master = ensure_master(uniform_sided)
    if master is None:
        return

    by_slot, made, inexact = {}, [], []
    for look, clusters in sorted(looks.items()):
        shared = len(clusters) == 1
        for cluster in clusters:
            lead = sorted(cluster["assets"])[0]
            name = instance_name(look, lead, shared)
            mi = ensure_instance(master, name, cluster["values"])
            if mi is None:
                continue
            made.append(name)
            for slot_name in cluster["slots"]:
                by_slot[slot_name] = mi
            rgb = cluster["values"][0]
            say("%-22s rgb %.4f %.4f %.4f  M %.2f  R %.2f  <- %s%s"
                % (name, rgb[0], rgb[1], rgb[2], cluster["values"][1], cluster["values"][2],
                   ", ".join(sorted(cluster["assets"])),
                   "" if cluster["exact"] else "   [merged with drift]"))
            if not cluster["exact"]:
                inexact.append((name, sorted(cluster["assets"])))

    total, orphans = 0, []
    for asset, (_folder, mesh_path) in sorted(FLEET.items()):
        moved, left = remap(mesh_path, by_slot)
        total += moved
        say("%-14s %d slot(s) repointed%s"
            % (mesh_path.split("/")[-1], moved,
               "" if not left else "; left as imported: %s" % ", ".join(left)))
    say("%d slot(s) repointed across %d asset(s)" % (total, len(FLEET)))

    # Prune instances this run did not produce. Cluster NAMES move when the clustering
    # changes - MI_Metal_Plane3 became MI_Strut the moment plane3's chrome stopped being
    # called `metal` - and the leftover keeps the folder looking like it holds duplicates.
    # Referencers are still checked: an instance something is wearing is reported, not
    # deleted, because the folder being tidy is worth less than a mesh keeping its material.
    wanted = set(made)
    for path in unreal.EditorAssetLibrary.list_assets(MAT_DIR, recursive=False):
        clean = path.split(".")[0]
        name = clean.split("/")[-1]
        if name in wanted or name == MASTER_NAME:
            continue
        asset = unreal.EditorAssetLibrary.load_asset(clean)
        if not isinstance(asset, unreal.MaterialInstanceConstant):
            continue
        refs = [r for r in unreal.EditorAssetLibrary.find_package_referencers_for_asset(
            clean, False) if r.split(".")[0] != clean]
        if refs:
            say("STALE %s is no longer produced but is still worn by %s - kept"
                % (name, ", ".join(sorted(r.split("/")[-1] for r in refs))))
        elif unreal.EditorAssetLibrary.delete_loaded_asset(asset):
            say("pruned stale %s" % name)
        else:
            fail("could not prune %s" % name)

    for name, assets in inexact:
        say("DRIFT %s merged values that differ by up to %.4f across %s - identical on "
            "screen, worth making exactly equal in Blender" % (name, MERGE_TOL, ", ".join(assets)))
    say("%d instance(s): %s" % (len(made), ", ".join(sorted(made))))
    say("DONE")


# GUARDED BY NAME, NOT BY "__main__" - and that distinction matters here. This is the one
# script another script legitimately IMPORTS: airside_import.rebuild_fleet_materials() runs it
# with `exec(compile(...), {"__name__": "__from_import__", ...})` specifically so it is NOT
# "build_fleet_materials" (the name a plain `import build_fleet_materials` would set) - that
# was already true before this guard existed, it just had nothing to distinguish itself from.
# An unguarded run() at module scope meant `import build_fleet_materials` ALONE reran the FULL
# fleet rebuild as a side effect of the import statement - which is exactly how a "read the
# mesh's current slots" verification script silently re-touched all 14 fleet meshes and
# recreated MI_Livery during Task 3's fix round, undoing a revert that had just been checked
# clean. See remap()'s own comment for the rest of that incident. `!= "build_fleet_materials"`
# (rather than `== "__main__"`) is what keeps BOTH the direct `-script=` invocation AND
# rebuild_fleet_materials()'s exec working exactly as before, and only a bare Python import
# inert.
if __name__ != "build_fleet_materials":
    run()
