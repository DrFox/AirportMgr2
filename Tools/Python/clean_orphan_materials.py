"""Deletes the per-asset materials that build_fleet_materials.py made redundant. Headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

WHY THESE EXIST. Interchange generates one full UMaterial per glTF material on every import -
~48 KB each, carrying the entire glTF uber-graph (iridescence, anisotropy, normals, occlusion,
specular, UV transforms) to express three constants. build_fleet_materials.py repointed every
mesh slot at a shared instance of M_Fleet, so these are now referenced by nothing.

NOTHING IS DELETED ON THE STRENGTH OF BEING IN THE RIGHT FOLDER. Each candidate is deleted
only if find_package_referencers_for_asset returns nothing for it. A material still worn by
something is reported and kept, because the failure mode of getting this wrong is an asset
that renders as the default grey and a mesh slot that can only be repaired by re-importing.

KEEP is checked first and covers the two things that must never be swept up:
  - /Game/Materials/Fleet   - the shared set itself
  - PiperMeridian           - hand-authored M_PiperMeridian / M_PiperGlass against real
                              texture maps, which the flat-colour master cannot replace

REFERENCERS ARE PACKAGE-LEVEL. find_package_referencers_for_asset answers "which packages
point at this one", so a material referenced only by its own package reads as unreferenced,
which is what we want. A redirector left by an earlier rename counts as a referencer, so
run this AFTER fixing up redirectors if any rename has happened, or it will report a false
positive and keep an asset that is genuinely dead.
"""
import unreal

SCAN = ["/Game/Aircraft", "/Game/Vehicles"]
KEEP_PREFIXES = ["/Game/Materials/Fleet", "/Game/Aircraft/PiperMeridian"]
DELETE = True          # False makes this a dry run


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def kept(path):
    return any(path.startswith(p) for p in KEEP_PREFIXES)


def run():
    candidates = []
    for root in SCAN:
        if not unreal.EditorAssetLibrary.does_directory_exist(root):
            continue
        for path in unreal.EditorAssetLibrary.list_assets(root, recursive=True):
            clean = path.split(".")[0]
            if kept(clean):
                continue
            asset = unreal.EditorAssetLibrary.load_asset(clean)
            if isinstance(asset, (unreal.Material, unreal.MaterialInstanceConstant)):
                candidates.append(clean)

    say("%d material asset(s) outside the keep list" % len(candidates))
    dead, live = [], []
    for path in sorted(set(candidates)):
        refs = [r for r in
                unreal.EditorAssetLibrary.find_package_referencers_for_asset(path, False)
                if r.split(".")[0] != path]
        if refs:
            live.append((path, refs))
        else:
            dead.append(path)

    for path, refs in sorted(live):
        say("KEEP  %-52s still worn by %s"
            % (path, ", ".join(sorted(r.split("/")[-1] for r in refs))))

    removed = 0
    for path in dead:
        if not DELETE:
            say("WOULD DELETE %s" % path)
            continue
        loaded = unreal.EditorAssetLibrary.load_asset(path)
        ok = (unreal.EditorAssetLibrary.delete_loaded_asset(loaded) if loaded is not None
              else unreal.EditorAssetLibrary.delete_asset(path))
        if ok:
            removed += 1
            say("DELETED %s" % path)
        else:
            fail("could not delete %s" % path)

    say("%d unreferenced, %d deleted, %d kept" % (len(dead), removed, len(live)))
    say("DONE")


run()
