"""Re-imports SK_Plane1 IN PLACE after the livery landed in the .blend. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED, or the save writes nothing.

WHY THIS EXISTS. plane1 imported on 2026-09-19 with EIGHT material slots and drew pure white.
Two separate causes, one re-export:

  * THE LIVERY WAS PAINTED IN THE SHADER, NOT ON THE FACES. plane1_body's Base Color was a
    Mix node - Color1 the body grey (0.85, 0.85, 0.84), Color2 the livery blue
    (0.02, 0.055, 0.16), Factor a twenty-node chain of Geometry -> Separate XYZ -> MATH
    position-band tests. It looked right in Blender and could not survive the export: glTF's
    baseColorFactor is ONE FLAT COLOUR per material, so the exporter emitted no factor at
    all, glTF defaulted it to white, and build_fleet_materials.py - which scrapes exactly
    that one factor - read white. plane1_prop_blade did the same thing with a radial mask
    for its white tips, which is why the blades were white too. ONE CAUSE, BOTH SYMPTOMS.
  * plane1_livery and plane1_intake already existed as flat materials, the form the pipeline
    wants, assigned to 50 and 35 faces on an object that was not the one being exported.

The .blend now flattens both Mix nodes to their Color1 and assigns the stripe to real faces,
so the export carries ten materials, every one with a baseColorFactor. A mesh SECTION's
material index is baked at import, so the asset has to read the source again to see them.

WHY NOT import_models.py. Its own docstring says it imports "the models that have a current
export AND NO ASSETS YET" - it is a first-import tool, and it now SKIPS plane1 for exactly
this reason. Run against an existing asset it calls clear_previous(), which delete_directory()s
the folder; reimport_plane3.py records at length what that did when it was tried, and the
delete failing was the level protecting itself. M_ModelYard places SK_Plane1, and
DA_Aircraft_Plane1 and ABP_Plane1 point at it. UInterchangeManager::ReimportAsset updates the
UObject in place, so its package path, GUID and every one of those references survive by
construction.

THE PART NAMES CHANGED IN THE SAME ROUND and this script is the first reader of the new ones:
the nose tyre went wheel_front_1 -> nosewheel, the fork wheel_front -> nosegear, and the main
leg wheelstrut -> maingear. The first was the only mesh in the fleet not named after its own
bone; the other two put a part that is not a tyre in the tyre's namespace. import_models.py's
Spec and aircraft/plane1.py's (build_aircraft_type.py's) GEAR_LEG moved with them.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402

MESH = "/Game/Aircraft/Plane1/SK_Plane1"
SOURCE = r"C:\repos\AirportMgr2Models\plane1\export\plane1.glb"

# The pipeline this script authors, uses and deletes.
PIPELINE_PATH = "/Game/Aircraft/Plane1/PL_Plane1_Reimport"

# Which bone each moving part must ride, checked against the SOURCE .glb.
#
# MEASURED OFF THE EXPORT AND THEN WRITTEN DOWN, not guessed from the names. The one worth
# stating is `nosegear`: the FORK rides nosewheel_steer and not nosewheel, because it turns
# with the steering and does not roll. Put it on nosewheel and the whole leg spins about the
# axle like a broken castor - the same failure airside_anim.BONE_RULES' STEER-BEFORE-WHEEL
# ordering exists to prevent, in geometry rather than in an animation variable.
#
# `maingear` rides root because a 172's gear is welded on. That is an assertion about a FIXED
# part, and it is here rather than assumed: a leg that started riding a wheel bone would be
# invisible in every count, extent and material slot the rest of this script measures.
EXPECTED_SKIN = {
    "nosewheel": "nosewheel",
    "nosegear": "nosewheel_steer",
    "wheel_L": "wheel_L",
    "wheel_R": "wheel_R",
    "prop": "prop",
    "maingear": "root",
}

# WHAT THIS REIMPORT IS FOR, as a check rather than as a hope. Eight slots before, ten after,
# and the two new ones are the whole point - a reimport that succeeded and left eight would
# mean the export is stale, and the aeroplane would still be plain.
EXPECTED_NEW_SLOTS = ["plane1_intake", "plane1_livery"]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def check_source():
    """The .glb carries ten materials, every one with a flat colour, and each moving part
    rides exactly one bone.

    READ STRAIGHT OUT OF THE FILE, before the asset is touched. Importing a stale export and
    then discovering it was stale costs a second reimport and leaves the asset wrong in
    between; reading it first costs nothing and refuses the run outright. That is
    reimport_plane3.py's rule and it is the right one.

    THE baseColorFactor CHECK IS THE NEW ONE, and it is the reason this file exists. A
    material with no factor is not an error in glTF - the spec defaults it to [1,1,1,1] - so
    nothing downstream complains and the asset simply renders white. An absent field is
    exactly the kind of thing no count or extent would catch.
    """
    import json
    import struct

    data = open(SOURCE, "rb").read()
    _, _, total = struct.unpack("<III", data[:12])
    off, chunks = 12, {}
    while off < total:
        length, kind = struct.unpack("<I4s", data[off:off + 8])
        chunks[kind.strip(b"\x00").decode()] = data[off + 8:off + 8 + length]
        off += 8 + length
    doc = json.loads(chunks["JSON"].decode("utf-8"))
    blob = chunks.get("BIN", b"")

    ok = True

    # 1. EVERY MATERIAL DECLARES ITS COLOUR.
    colourless = [m.get("name", "?") for m in doc.get("materials", [])
                  if "baseColorFactor" not in m.get("pbrMetallicRoughness", {})]
    if colourless:
        fail("%d material(s) carry no baseColorFactor, so glTF defaults them to pure white "
             "and the fleet scrape reads white: %s. Flatten the Base Color to a value in the "
             ".blend - a Mix node cannot be expressed as a factor."
             % (len(colourless), ", ".join(colourless)))
        ok = False
    else:
        say("PASS all %d material(s) declare a baseColorFactor"
            % len(doc.get("materials", [])))

    for wanted in EXPECTED_NEW_SLOTS:
        if not any(m.get("name") == wanted for m in doc.get("materials", [])):
            fail("%s is not in the export - the .blend has it but no faces are assigned, so "
                 "the exporter dropped it" % wanted)
            ok = False

    # 2. THE LIVERY REACHES REAL FACES. A material present in the table but on no primitive
    #    is the failure this reimport is fixing, one step earlier: it never reaches a slot.
    mats = [m.get("name", "?") for m in doc.get("materials", [])]
    worn = set()
    for node in doc.get("nodes", []):
        index = node.get("mesh")
        if index is None:
            continue
        for prim in doc["meshes"][index]["primitives"]:
            if "material" in prim:
                worn.add(mats[prim["material"]])
    for wanted in EXPECTED_NEW_SLOTS:
        if wanted in mats and wanted not in worn:
            fail("%s is declared but sits on no instanced mesh - it would not reach a slot"
                 % wanted)
            ok = False
    if ok:
        say("PASS the livery reaches real faces: %s"
            % ", ".join(sorted(w for w in worn if w in EXPECTED_NEW_SLOTS)))

    # 3. EVERY MOVING PART RIDES ONE BONE, AND THE RIGHT ONE.
    fmt_of = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
              5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
    count_of = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

    def read(index):
        acc = doc["accessors"][index]
        fmt, size = fmt_of[acc["componentType"]]
        n = count_of[acc["type"]]
        view = doc["bufferViews"][acc["bufferView"]]
        base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = view.get("byteStride") or size * n
        return [struct.unpack_from("<" + fmt * n, blob, base + i * stride)
                for i in range(acc["count"])]

    if not doc.get("skins"):
        fail("%s declares no skin - nothing in it would animate" % SOURCE)
        return False
    joints = [doc["nodes"][j].get("name") for j in doc["skins"][0]["joints"]]

    # Node names carry Blender's duplicate suffix (nosewheel.001), since the modelling objects
    # already own the clean names. Match on the stem, as airside_import does.
    def stem(name):
        return name.rsplit(".", 1)[0] if name and name[-3:].isdigit() else name

    seen = {}
    for node in doc.get("nodes", []):
        index = node.get("mesh")
        if index is None:
            continue
        key = stem(node.get("name", ""))
        if key not in EXPECTED_SKIN:
            continue
        tally = {}
        for prim in doc["meshes"][index]["primitives"]:
            attrs = prim["attributes"]
            if "JOINTS_0" not in attrs:
                tally["(unskinned)"] = tally.get("(unskinned)", 0) + 1
                continue
            for jj, ww in zip(read(attrs["JOINTS_0"]), read(attrs["WEIGHTS_0"])):
                best = max(range(len(ww)), key=lambda k: ww[k])
                name = joints[jj[best]] if ww[best] > 0.0 else "(zero weight)"
                tally[name] = tally.get(name, 0) + 1
        seen[key] = tally
        want = EXPECTED_SKIN[key]
        if list(tally) != [want]:
            fail("%s rides %s in the .glb, expected every vertex on %s - the export is stale "
                 "or the object is skinned to more than one bone"
                 % (key, ", ".join("%s=%d" % kv for kv in sorted(tally.items())), want))
            ok = False
        else:
            say("PASS %-10s %d vert(s), all on %s" % (key, tally[want], want))

    for key in EXPECTED_SKIN:
        if key not in seen:
            fail("%s is not in %s - the part was renamed again, or build_export.py did not "
                 "run" % (key, SOURCE))
            ok = False
    return ok


def slot_report(mesh, when):
    slots = mesh.get_editor_property("materials")
    names = [str(s.material_slot_name) for s in slots]
    empty = [str(s.material_slot_name) for s in slots if s.material_interface is None]
    say("%-7s %d slot(s): %s" % (when, len(names), ", ".join(names)))
    if empty:
        say("%-7s %d slot(s) with NO material: %s" % (when, len(empty), ", ".join(empty)))
    return names, empty


def main():
    say("=" * 78)

    if not os.path.isfile(SOURCE):
        fail("missing %s" % SOURCE)
        say("DONE")
        return

    if not check_source():
        fail("the export is not what the reimport expects - nothing was imported")
        say("DONE")
        return

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)
    if not isinstance(mesh, unreal.SkeletalMesh):
        fail("%s is not a SkeletalMesh" % MESH)
        say("DONE")
        return

    before, _ = slot_report(mesh, "before")

    manager = unreal.InterchangeManager.get_interchange_manager_scripted()
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.replace_existing = True
    # OVERRIDDEN, not left to the default stack - see airside_import.reimport_pipeline for
    # what bUpdateSkeletonReferencePose defaulting to False cost plane2.
    pipeline_path = airside_import.reimport_pipeline(PIPELINE_PATH)
    if pipeline_path is None:
        say("DONE")
        return
    params.override_pipelines = [unreal.SoftObjectPath(pipeline_path)]
    try:
        manager.reimport_asset(mesh, params)
    except Exception as exc:
        airside_import.drop_reimport_pipeline(PIPELINE_PATH)
        fail("reimport_asset raised %s - do NOT fall back to deleting, M_ModelYard places "
             "this mesh and DA_Aircraft_Plane1 points at it" % exc)
        say("DONE")
        return
    airside_import.drop_reimport_pipeline(PIPELINE_PATH)

    mesh = unreal.EditorAssetLibrary.load_asset(MESH)      # read the package back
    after, empty = slot_report(mesh, "after")

    ok = True
    if empty:
        fail("%d slot(s) carry no material: %s - those sections draw the engine default"
             % (len(empty), ", ".join(empty)))
        ok = False
    else:
        say("PASS every one of %d slot(s) carries a material" % len(after))

    # THE SLOTS THE REIMPORT WAS RUN FOR, named rather than counted. A count would pass on
    # any two new slots; this fails unless the livery is one of them.
    for wanted in EXPECTED_NEW_SLOTS:
        if wanted in after:
            say("PASS %s is now a slot on SK_Plane1%s"
                % (wanted, " (new)" if wanted not in before else ""))
        else:
            fail("%s did not become a slot - the reimport reported success and changed "
                 "nothing, which is this project's most familiar failure" % wanted)
            ok = False

    refs = unreal.EditorAssetLibrary.find_package_referencers_for_asset(MESH, False)
    say("PASS %s still referenced by %s" % (MESH.split("/")[-1],
        ", ".join(sorted(r.split("/")[-1] for r in refs)) or "nothing"))

    # THE SKELETON TOO, not the mesh alone: reimport_pipeline updates the Skeleton asset, and a
    # Skeleton left unsaved is the stale bone tree the next session re-merges and asks to save
    # - see airside_import.save_mesh_and_skeleton.
    if not airside_import.save_mesh_and_skeleton(MESH):
        ok = False

    # A reimport regenerates plane1's materials and reassigns its slots, so the shared set has
    # to be rebuilt or the aeroplane comes back wearing a private uber-graph per flat colour.
    #
    # The import is at module scope. Leaving a second one HERE made `airside_import` a local
    # of this whole function in reimport_plane3.py, so its call at the top raised
    # UnboundLocalError - the reimport never ran and the log's last line was a slot report
    # that looked healthy.
    airside_import.rebuild_fleet_materials()

    say("plane1: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("THEN RUN clean_orphan_materials.py - a reimport generates a per-asset UMaterial per "
        "slot and nothing here sweeps them. That step is not automatic and was missed once.")
    say("DONE")


main()
