"""Imports the fuel depot's meshes - the shed's two parts and the tank - and dresses them in
the fleet's shared material. Run headless, EDITOR CLOSED:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Then run build_plot_kits.py, which points the kits at these meshes. Every result line is
prefixed MARKER: so it can be grepped out of Saved/Logs/AirportMgr.log.

WHAT THE MODELS ARE - AirportMgr2Models/FuelDepot1/{shed,fueltank}/SPEC.md, which are the
asset contracts. The pump is not here: it stays a grey box until its model is signed off.

NOT import_models.py. That table drives airside_import's glTF SKELETAL pipeline - wheel
nodes, rigs, axle checks - and these are three static FBX meshes, the fence's shape
(build_fence_content.py), whose import this follows.

RE-IMPORTED IN PLACE WHEN THE EXPORT IS NEWER than the .uasset, and kept otherwise. In
place - same package, replace_existing - so DA_Kit_FuelShed's reference survives; deleting
and re-importing would break it. The run lists the folder afterwards and fails on anything
it did not expect, because a re-import has stranded a stray mesh before (memory:
import_models.py is first-import only). Materials and flags are re-applied every run.

MEASURED AFTER IMPORT, not trusted. FBX unit and axis handling differs between exporters,
and a shed that arrives 5.4 uu tall is a correct-looking import of the wrong size. Every
mesh's bounds are checked against SPEC.md's figures, in the mesh's OWN frame (Unreal's
FBX import negates Blender's Y), and the run FAILS if they are off.

M_Fleet, NOT INTERCHANGE'S MATERIALS - see airside_import.rebuild_fleet_materials for why.
One instance per look, values SCRAPED FROM THE .glb so Blender stays the one source of
truth. They live beside the fleet's in /Game/Materials/Fleet, parented to the same master.

TWO USAGE FLAGS ARE SET HERE because these meshes are the first things drawn through
instanced components wearing them: UPlotPresenter pools one ISM per mesh. Without
bUsedWithInstancedStaticMeshes the renderer substitutes the default material in a game
world and says so once in the log - issue #265, which found the ghost material that way.
M_RoadGhost's flag is ALSO set by build_ghost_material.py, which re-creates that material;
it is set here too so this script alone leaves the depot drawable.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import unreal  # noqa: E402

from airside_import import read_gltf, sweep_orphan_materials  # noqa: E402

MODELS = r"C:\repos\AirportMgr2Models\FuelDepot1"
MESH_DIR = "/Game/Environment/FuelDepot"
MAT_DIR = "/Game/Materials/Fleet"
MASTER = "%s/M_Fleet" % MAT_DIR
GHOST = "/Game/Materials/M_RoadGhost"

# asset name, model folder, export stem, expected bounds in Unreal uu (min xyz, max xyz).
#
# THE EXPECTED BOUNDS ARE SPEC.md's, in the mesh's own frame after import, where Blender's
# +Y arrives as Unreal's -Y. The shed's openings face Blender -Y, so +Y here, and its bays
# tile along +X; UPlotModuleKit::MeshYawDeg turns that into the kit's frame, and
# AirportMgr.Content.DepotKitMeshesMatchTheirFootprints measures the result.
MESHES = [
    ("SM_ShedBay", "shed", "shed_bay",
     (0.0, -920.7, 0.0), (500.0, 20.7, 543.9)),
    ("SM_ShedEnd", "shed", "shed_end",
     (-75.0, -920.7, 0.0), (0.0, 20.7, 543.9)),
    ("SM_FuelTank", "fueltank", "fueltank",
     (-110.0, -240.0, 0.0), (110.0, 240.0, 263.9)),
]
TOLERANCE_UU = 0.5

_failed = False


def say(msg):
    # WARNING, for build_plot_kits.py's reason: a Display-level line does not reach the
    # commandlet's stdout, so a MARKER logged with unreal.log is invisible to a grep of it.
    unreal.log_warning("MARKER: " + str(msg))


def fail(msg):
    global _failed
    _failed = True
    unreal.log_error("MARKER: FAIL " + str(msg))


def camel(material_name):
    """shed_cladding -> ShedCladding, the fleet's instance naming (MI_Tyre, MI_BodyPlane2)."""
    return "".join(part.capitalize() for part in material_name.split("_"))


def import_mesh(name, folder, stem):
    path = "%s/%s" % (MESH_DIR, name)
    source = os.path.join(MODELS, folder, "export", stem + ".fbx")
    if not os.path.exists(source):
        fail("no export at %s - run the model's build script in the models repo" % source)
        return None
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        on_disk = os.path.join(unreal.Paths.project_content_dir(),
                               MESH_DIR[len("/Game/"):], name + ".uasset")
        if os.path.getmtime(source) <= os.path.getmtime(on_disk):
            say("kept existing %s (export not newer)" % path)
            return unreal.EditorAssetLibrary.load_asset(path)
        say("export newer than %s - re-importing in place" % path)
    task = unreal.AssetImportTask()
    task.filename = source
    task.destination_path = MESH_DIR
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    say("imported %s from %s" % (path, source))
    return unreal.EditorAssetLibrary.load_asset(path)


def disable_nanite(name, mesh):
    """NANITE OFF on every depot mesh: sub-2k-triangle props drawn through instanced
    components, where Nanite buys nothing. It was turned off on 2026-09-22 while chasing the
    tank's widened bounds and was NOT their cause (see measure()); it stays off on its own
    merits. Saved here so the setting sticks even when measure() then fails."""
    settings = mesh.get_editor_property("nanite_settings")
    if settings.enabled:
        settings.enabled = False
        mesh.set_editor_property("nanite_settings", settings)
        unreal.EditorAssetLibrary.save_asset("%s/%s" % (MESH_DIR, name), only_if_is_dirty=False)
        say("%s Nanite off, %d triangle(s) at LOD0" % (name, mesh.get_num_triangles(0)))


def measure(name, mesh, want_min, want_max):
    """The GEOMETRY's extent, from the mesh description's vertices - not get_bounding_box().

    TWO DIFFERENT ANSWERS, by session: straight after an import the engine returns the mesh
    description's exact bounds (UStaticMesh::CachedMeshDescriptionBounds); on an asset loaded
    from disk it returns the render data's, which for the curved tank came back 1.6 uu wider a
    side and 2 uu below ground (2026-09-22) with the vertices still exactly +-110. This check
    exists to catch unit and axis mistakes in the export, so it reads the vertices."""
    desc = mesh.get_static_mesh_description(0)
    points = [desc.get_vertex_position(unreal.VertexID(i)) for i in range(desc.get_vertex_count())]
    got_min = (min(p.x for p in points), min(p.y for p in points), min(p.z for p in points))
    got_max = (max(p.x for p in points), max(p.y for p in points), max(p.z for p in points))
    say("%s bounds min (%.1f, %.1f, %.1f) max (%.1f, %.1f, %.1f)" % ((name,) + got_min + got_max))
    ok = True
    for axis, got, want in zip("xyzxyz", got_min + got_max, want_min + want_max):
        if abs(got - want) > TOLERANCE_UU:
            fail("%s %s is %.1f, SPEC.md says %.1f - an FBX unit or axis mismatch"
                 % (name, axis, got, want))
            ok = False
    return ok


def ensure_usage_flags(path):
    material = unreal.EditorAssetLibrary.load_asset(path)
    if material is None:
        fail("no %s" % path)
        return
    if not material.get_editor_property("used_with_instanced_static_meshes"):
        material.set_editor_property("used_with_instanced_static_meshes", True)
        unreal.MaterialEditingLibrary.recompile_material(material)
        say("%s flagged for instanced static meshes" % path)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # READ BACK after the save: a headless save that wrote nothing has reported success here
    # before (memory: authoring uassets headlessly).
    back = unreal.EditorAssetLibrary.load_asset(path)
    if not back.get_editor_property("used_with_instanced_static_meshes"):
        fail("%s's instanced flag did not stick" % path)
    else:
        say("PASS %s used_with_instanced_static_meshes" % path)


def ensure_instance(master, material_name, values):
    lib = unreal.MaterialEditingLibrary
    name = "MI_%s" % camel(material_name)
    path = "%s/%s" % (MAT_DIR, name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        mi = unreal.EditorAssetLibrary.load_asset(path)
    else:
        mi = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, MAT_DIR, unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew())
    if mi is None:
        fail("could not create %s" % path)
        return None
    rgb, metallic, rough = values
    lib.set_material_instance_parent(mi, master)
    lib.set_material_instance_vector_parameter_value(
        mi, "BaseColor", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
    lib.set_material_instance_scalar_parameter_value(mi, "Metallic", metallic)
    lib.set_material_instance_scalar_parameter_value(mi, "Roughness", rough)
    lib.update_material_instance(mi)
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)

    # READ BACK: set_material_instance_*_parameter_value no-ops on an unknown name and still
    # saves cleanly (memory: fleet materials).
    back = lib.get_material_instance_vector_parameter_value(mi, "BaseColor")
    if abs(back.r - rgb[0]) > 1e-3 or abs(back.g - rgb[1]) > 1e-3 or abs(back.b - rgb[2]) > 1e-3:
        fail("%s BaseColor read back (%.3f, %.3f, %.3f), wrote (%.3f, %.3f, %.3f)"
             % ((name, back.r, back.g, back.b) + tuple(rgb)))
    return mi


def looks_of(folder, stem):
    """material name -> (linear rgb, metallic, roughness), from the .glb beside the .fbx."""
    doc = read_gltf(os.path.join(MODELS, folder, "export", stem + ".glb"))
    out = {}
    for mat in (doc or {}).get("materials", []):
        pbr = mat.get("pbrMetallicRoughness", {})
        if "baseColorFactor" not in pbr:
            # glTF's default is pure white - see the fleet materials memory: a missing factor
            # is a Blender export that lost its colour, never a white material.
            fail("%s.glb material %s has no baseColorFactor" % (stem, mat.get("name")))
            continue
        out[mat["name"]] = (tuple(pbr["baseColorFactor"][:3]),
                            float(pbr.get("metallicFactor", 1.0)),
                            float(pbr.get("roughnessFactor", 1.0)))
    return out


def dress(name, mesh, looks, master):
    """Every slot onto its M_Fleet instance, MATCHED BY SLOT NAME - never by order, which is
    what the mesh sections index."""
    slots = mesh.get_editor_property("static_materials")
    out = []
    for slot in slots:
        slot_name = str(slot.material_slot_name)
        values = looks.get(slot_name)
        if values is None:
            fail("%s slot %s has no look in any of its model's .glb files" % (name, slot_name))
            out.append(slot)
            continue
        mi = ensure_instance(master, slot_name, values)
        out.append(unreal.StaticMaterial(material_interface=mi, material_slot_name=slot.material_slot_name))
    mesh.set_editor_property("static_materials", out)
    unreal.EditorAssetLibrary.save_asset("%s/%s" % (MESH_DIR, name), only_if_is_dirty=False)
    say("%s: %d slot(s) on M_Fleet instances" % (name, sum(1 for s in out if s.material_interface
        and s.material_interface.get_path_name().startswith(MAT_DIR))))


def run():
    master = unreal.EditorAssetLibrary.load_asset(MASTER)
    if master is None:
        fail("no %s - run build_fleet_materials.py first" % MASTER)
        return
    ensure_usage_flags(MASTER)
    ensure_usage_flags(GHOST)

    for name, folder, stem, want_min, want_max in MESHES:
        mesh = import_mesh(name, folder, stem)
        if not isinstance(mesh, unreal.StaticMesh):
            fail("%s is not a StaticMesh after import (got %r)" % (name, mesh))
            continue
        disable_nanite(name, mesh)
        if not measure(name, mesh, want_min, want_max):
            continue
        # EVERY .glb OF THE MODEL, not just this mesh's: the .glb drops a slot no face uses
        # (shed_end has no trim or rooflight) while the .fbx keeps it, and a slot left on its
        # generated material keeps that material alive past the sweep below.
        looks = {}
        for other in MESHES:
            if other[1] == folder:
                looks.update(looks_of(folder, other[2]))
        dress(name, mesh, looks, master)

    # Interchange generated a full UMaterial per slot beside the meshes; nothing wears them
    # now. Checked on disk and by referencer, never by folder alone - see the sweeper.
    swept = sweep_orphan_materials([MESH_DIR])
    say("swept %d generated material(s) from %s" % (swept, MESH_DIR))

    # NOTHING BUT THE MESHES may be left in the folder - see the docstring on re-imports.
    expected = {m[0] for m in MESHES}
    found = {p.split("/")[-1].split(".")[0]
             for p in unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=True)}
    stray = sorted(found - expected)
    if stray:
        fail("%s holds unexpected asset(s): %s" % (MESH_DIR, ", ".join(stray)))
    else:
        say("PASS %s holds exactly %s" % (MESH_DIR, ", ".join(sorted(expected))))


run()
say("FAILED" if _failed else "OK")
