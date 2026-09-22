"""Authors the three fuel depot kits and maps them on DA_AirsideContent. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

THE FIGURES ARE THE MESHES' SINCE 2026-09-22, for the shed and the tank. They were the
grey-box table until then, deliberately - content landing and the numbers moving were kept
as two changes - and the meshes are what moved them: measured off the imported assets
(build_depot_content.py prints the bounds) and checked every test run by
AirportMgr.Content.DepotKitMeshesMatchTheirFootprints. The pump keeps its grey box.

  shed  9.42 x 5.00 m a bay (roof eaves included), 0.75 m per end cap, 5.44 m to the ridge
  tank  2.20 x 4.80 m, long axis along the run - parallel to the frontage, as the concept
        sheet stands it - and 2.64 m to the hatch

WEIGHTS AND RUN CAPS ARE NEW, and they are the design doc's:

  shed  weight 3, cap 3   a run of up to three bays, the thing runs exist for
  tank  weight 2, cap 1   never grouped: two tanks are two objects
  pump  weight 1, cap 1   small, and one is usually enough

A WEIGHT SETS A CEILING, NOT A STRATEGY. The player buys in whatever order they like up to
it, so a generous weight costs nothing and a mean one silently forbids a build they wanted.

THE SHED IS PARTS, THE TANK IS BAKED. The shed ships as one bay and one end (FuelDepot1/shed/
SPEC.md) and the presenter lays cap, bays and the cap turned half a turn along the run; the tank is one
mesh that never groups. The pump's BakedMeshes stays empty, which is the grey box.

EVERY FIELD IS SET ON EVERY RUN, defaults included - these are authored in place, so a field
left alone keeps whatever was last saved (memory: authoring uassets headlessly).
"""
import os
import sys

import unreal

KIT_PATH = "/Game/Entities"
CONTENT_ASSET = "/Game/DA_AirsideContent"
MESH_DIR = "/Game/Environment/FuelDepot"

# name, display, length uu, width uu, height uu, back fence, weight, run cap, apron x uu
#
# ONLY THE SHED HAS AN APRON. A truck stands in front of a shed and drives out of it, so the
# ground there is not somewhere another module may go. A tank is plumbed and a pump is walked
# up to; neither needs more than the clearance every module already gets.
KITS = [
    ("DA_Kit_FuelShed", "Vehicle shed", 942.0, 500.0, 544.0, True, 3, 3, 400.0),
    ("DA_Kit_FuelTank", "Fuel tank", 220.0, 480.0, 264.0, False, 2, 1, 0.0),
    ("DA_Kit_FuelPump", "Fuel pump", 300.0, 200.0, 150.0, False, 1, 1, 0.0),
]

# name -> (assembly, baked meshes, cap mesh, bay mesh, cap uu, mesh yaw degrees).
#
# THE SHED'S YAW IS 90: its openings face the mesh's +Y and its bays tile along +X, and the
# kit wants the opening towards the gate (-X) and the run along +Y. A quarter turn does both.
# The tank lies along its mesh's Y already, which is the kit's run axis, so it takes none.
LOOKS = {
    "DA_Kit_FuelShed": ("PARTS", [], "SM_ShedEnd", "SM_ShedBay", 75.0, 90.0),
    "DA_Kit_FuelTank": ("BAKED", ["SM_FuelTank"], None, None, 0.0, 0.0),
    "DA_Kit_FuelPump": ("BAKED", [], None, None, 0.0, 0.0),
}

# EDepotModule, in the enum's own order - the index into KITS above IS the enum value, the
# same agreement DepotKitSpecs relies on. A map built the other way round would dress every
# module as a shed and look almost right.
MODULES = ["SHED", "TANK", "PUMP"]

_failed = False


def log(msg):
    # WARNING, NOT Display, and not for emphasis: a Display-level LogPython line does not
    # reach the commandlet's stdout under -run=pythonscript, so a MARKER written with
    # unreal.log is invisible to whoever is grepping for it - which makes "script executed
    # successfully" the only evidence, and that is not evidence the asset was written.
    unreal.log_warning("MARKER: " + str(msg))


def fail(msg):
    global _failed
    _failed = True
    unreal.log_error("MARKER: FAIL " + str(msg))


def author_kit(name, display, length, width, height, back_fence, weight, run_cap, apron_x):
    path = "%s/%s" % (KIT_PATH, name)
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        # Load-or-create, never delete-and-recreate: DA_AirsideContent comes to point at
        # this, and deleting an asset something references breaks the reference rather than
        # updating it.
        asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, KIT_PATH, unreal.PlotModuleKit, None)
    if asset is None:
        fail("could not create %s" % path)
        return None

    asset.set_editor_property("display_name", unreal.Text(display))
    asset.set_editor_property("footprint", unreal.Vector2D(length, width))
    asset.set_editor_property("height_uu", height)
    # NOT "b_against_the_back_fence": UE strips a bool's b prefix on the way to Python, so
    # bAgainstTheBackFence arrives without it. Setting the wrong name raises rather than
    # silently doing nothing, which is the one mercy here.
    asset.set_editor_property("against_the_back_fence", back_fence)
    asset.set_editor_property("reserve_weight", weight)
    asset.set_editor_property("run_cap", run_cap)
    assembly, baked, cap, bay, cap_uu, yaw = LOOKS[name]

    def mesh(stem):
        if stem is None:
            return None
        found = unreal.EditorAssetLibrary.load_asset("%s/%s" % (MESH_DIR, stem))
        if not isinstance(found, unreal.StaticMesh):
            fail("%s names %s/%s, which is not a static mesh - run build_depot_content.py"
                 % (name, MESH_DIR, stem))
        return found

    asset.set_editor_property("assembly", getattr(unreal.KitAssembly, assembly))
    asset.set_editor_property("baked_meshes", [mesh(m) for m in baked])
    asset.set_editor_property("part_cap_mesh", mesh(cap))
    asset.set_editor_property("part_bay_mesh", mesh(bay))
    asset.set_editor_property("part_cap_uu", cap_uu)
    asset.set_editor_property("mesh_yaw_deg", yaw)

    # X ONLY. The apron reaches towards the gate; nothing yet needs ground kept clear to its
    # sides, and a Y an author could not explain is a number the layout would silently obey.
    asset.set_editor_property("apron_uu", unreal.Vector2D(apron_x, 0.0))

    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    log("%s: %.0f x %.0f uu, apron %.0f, weight %d, run cap %d, %s, cap %.0f, yaw %.0f"
        % (name, length, width, apron_x, weight, run_cap, assembly, cap_uu, yaw))
    return asset


def main():
    content = unreal.EditorAssetLibrary.load_asset(CONTENT_ASSET)
    if content is None:
        fail("no %s" % CONTENT_ASSET)
        return

    kits = {}
    for index, spec in enumerate(KITS):
        asset = author_kit(*spec)
        if asset is None:
            return
        kits[MODULES[index]] = asset

    # THE MAP IS REPLACED WHOLE rather than added to. A stale entry for a module that has
    # been renamed would keep resolving, and the failure - a shed where a pump should be -
    # looks like a solver bug rather than like stale content.
    mapping = unreal.Map(unreal.DepotModule, unreal.PlotModuleKit)
    for name, asset in kits.items():
        mapping[getattr(unreal.DepotModule, name)] = asset

    content.set_editor_property("depot_kits", mapping)
    unreal.EditorAssetLibrary.save_asset(CONTENT_ASSET, only_if_is_dirty=False)
    log("DA_AirsideContent maps %d module(s)" % len(mapping))


main()
if _failed:
    log("FAILED")
else:
    log("OK")
