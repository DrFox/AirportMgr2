"""Authors /Game/Entities/DA_Vehicle_* - one UVehicleType per ground vehicle, with its geometry
MEASURED off the model. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Run AFTER import_models.py (the meshes), reimport_fueltruck1.py and build_vehicle_anims.py (the
Anim Blueprints). Every result line is prefixed MARKER: and lands in Saved/Logs/AirportMgr.log.

RE-AUTHORED IN PLACE, NEVER DELETED AND RECREATED. A data asset is referenced by things - the
yard catalogue finds these by class, but the next consumer may hold a pointer - and a
delete-and-recreate dies on references (see the project's note on authoring .uassets
headlessly). So an existing asset is loaded and every field is written over.

WHAT IS MEASURED AND WHERE FROM
-------------------------------
AXLES off the .glb's own wheel and steer JOINTS, which is what the import reproduces exactly
(import_models.py: "imported at the export's own size, worst axis off by 0.0 uu") and what
AirportMgr.Content.VehicleTypes.AxlesMatchTheMesh then holds against the imported skeleton.
The fixed axle is wheel_RL's, or the mean of a numbered group's (wheel_<n>L) - a tri-axle
trailer's origin is its middle axle, which is the group's centre.

BODY ENDS off the imported mesh's bounds - the same figure the test reads - so the two cannot
disagree by construction. BODY WIDTH off the .glb's parts with the WING MIRRORS LEFT OUT, the
exclusion FVehicle::BodyWidth defines (mirrors sit above a kerb and overhang it legally).

PERFORMANCE IS NOT MEASURED, and is the rigid-truck figures UAirsideSettings::ResolveDefaultVehicle
decides and argues for. The catering truck is the same rigidCab1 chassis and takes them whole.
The two TOWED types carry axles and body only: nothing dispatches them on their own (bTowed),
so a speed or a steering lock on them would be a number nothing reads.

THE FUEL TRUCK IS STATED TWICE UNTIL M3, deliberately and under a test. Its row here must equal
ResolveDefaultVehicle field for field - AirportMgr.Content.VehicleTypes.FuelTruckAgreesWithDispatch
- and UVehicleType's header says why dispatch is not moved onto the type in this change.
"""
import json
import os
import struct
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_import import part_bounds_uu, read_gltf  # noqa: E402

MODELS = r"C:\repos\AirportMgr2Models"
FOLDER = "/Game/Entities"

# ResolveDefaultVehicle's figures, which this file copies rather than decides. See the header.
RIGID_TRUCK = dict(accel=100.0, decel=200.0, speed_cap=1000.0, min_steering_speed=0.0,
                   max_lateral_accel_uu=294.0, max_turn_rate_deg_per_sec=90.0,
                   max_steer_degrees=45.0)

TYPES = [
    dict(asset="DA_Vehicle_FuelTruck1", code="FUEL", name="Fuel bowser",
         mesh="/Game/Vehicles/FuelTruck1/SK_FuelTruck1", abp="/Game/Vehicles/FuelTruck1/ABP_FuelTruck1",
         glb=r"rigidCab1\export\fueltruck1.glb", performance=RIGID_TRUCK, towed=False),
    dict(asset="DA_Vehicle_Catering1", code="CATR", name="Catering high-loader",
         mesh="/Game/Vehicles/Catering1/SK_Catering1", abp="/Game/Vehicles/Catering1/ABP_Catering1",
         glb=r"rigidCab1\export\catering1.glb", performance=RIGID_TRUCK, towed=False),
    dict(asset="DA_Vehicle_BaggageCart1", code="CART", name="Baggage cart",
         mesh="/Game/Vehicles/BaggageCart1/SK_BaggageCart1",
         abp="/Game/Vehicles/BaggageCart1/ABP_BaggageCart1",
         glb=r"baggageCart1\export\baggageCart1.glb", performance=None, towed=True),
    dict(asset="DA_Vehicle_CurtainTrailer1", code="TRLR", name="Curtain-side trailer",
         mesh="/Game/Vehicles/CurtainTrailer1/SK_CurtainTrailer1",
         abp="/Game/Vehicles/CurtainTrailer1/ABP_CurtainTrailer1",
         glb=r"truckCab1\export\curtainTrailer1.glb", performance=None, towed=True),
]


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def joints_x_uu(doc):
    """{joint: X uu} - the joint's position along the vehicle, parents composed.

    TRANSLATIONS ONLY, which is exact for these rigs and would not be for a rotated parent:
    every bone above a wheel here is unrotated in the export (rigidCab1/README.md's rig
    contract), and AxlesMatchTheMesh re-measures through the full transforms regardless.
    """
    nodes = doc["nodes"]
    parent = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parent[child] = index
    out = {}
    for skin in doc.get("skins", []):
        for index in skin["joints"]:
            x = 0.0
            at = index
            while at is not None:
                x += nodes[at].get("translation", [0.0, 0.0, 0.0])[0]
                at = parent.get(at)
            out[nodes[index]["name"]] = x * 100.0
    return out


def measure(t):
    doc = read_gltf(os.path.join(MODELS, t["glb"]))
    if doc is None:
        fail("%s: could not read %s" % (t["asset"], t["glb"]))
        return None
    joints = joints_x_uu(doc)
    if "wheel_RL" in joints:
        fixed = joints["wheel_RL"]
    else:
        group = [x for name, x in joints.items() if len(name) == 8 and name.startswith("wheel_")
                 and name[6].isdigit() and name[7] == "L"]
        if not group:
            fail("%s: no fixed axle among %s" % (t["asset"], ", ".join(sorted(joints))))
            return None
        fixed = sum(group) / len(group)
    steer = joints.get("steer_FL", fixed)

    mesh = unreal.EditorAssetLibrary.load_asset(t["mesh"])
    if mesh is None:
        fail("%s: no mesh %s" % (t["asset"], t["mesh"]))
        return None
    bounds = mesh.get_bounds()
    front = bounds.origin.x + bounds.box_extent.x
    rear = bounds.origin.x - bounds.box_extent.x

    parts = part_bounds_uu(doc)
    low, high, mirrors = None, None, []
    for stem, (lo, hi) in parts.items():
        if "mirror" in stem.lower():
            mirrors.append(stem)
            continue
        low = lo[1] if low is None else min(low, lo[1])
        high = hi[1] if high is None else max(high, hi[1])
    width = high - low
    return dict(fixed=fixed, steer=steer, front=front, rear=rear, width=width,
                mesh_width=bounds.box_extent.y * 2.0, mirrors=mirrors, mesh=mesh)


def ensure(asset):
    path = "%s/%s" % (FOLDER, asset)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return unreal.EditorAssetLibrary.load_asset(path)
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.VehicleType)
    made = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        asset, FOLDER, unreal.VehicleType, factory)
    if made is not None:
        say("created %s" % path)
    return made


def author(t, m):
    asset = ensure(t["asset"])
    if asset is None:
        fail("could not create %s" % t["asset"])
        return False
    abp = unreal.EditorAssetLibrary.load_asset(t["abp"])
    anim = abp.generated_class() if abp is not None else None
    if anim is None:
        fail("%s: no compiled %s - run build_vehicle_anims.py first" % (t["asset"], t["abp"]))
        return False

    # STRUCTS COME BACK AS COPIES from get_editor_property, and a copy edited and never set
    # back is the most common silent no-op in editor Python. Every level is set back.
    geometry = asset.get_editor_property("geometry")
    chassis = geometry.get_editor_property("chassis")
    chassis.set_editor_property("fixed_axle_x", round(m["fixed"], 1))
    chassis.set_editor_property("steer_axle_x", round(m["steer"], 1))
    if t["performance"] is not None:
        p = t["performance"]
        chassis.set_editor_property("steer_law", unreal.SteerLaw.ROLLING_STEER)
        ground = chassis.get_editor_property("ground")
        taxi = ground.get_editor_property("taxi")
        taxi.set_editor_property("accel", p["accel"])
        taxi.set_editor_property("decel", p["decel"])
        taxi.set_editor_property("speed_cap", p["speed_cap"])
        ground.set_editor_property("taxi", taxi)
        ground.set_editor_property("min_steering_speed", p["min_steering_speed"])
        ground.set_editor_property("max_lateral_accel_uu", p["max_lateral_accel_uu"])
        ground.set_editor_property("max_turn_rate_deg_per_sec", p["max_turn_rate_deg_per_sec"])
        ground.set_editor_property("max_steer_degrees", p["max_steer_degrees"])
        chassis.set_editor_property("ground", ground)
    geometry.set_editor_property("chassis", chassis)
    geometry.set_editor_property("body_width", round(m["width"], 1))
    geometry.set_editor_property("body_front_x", round(m["front"], 1))
    geometry.set_editor_property("body_rear_x", round(m["rear"], 1))
    asset.set_editor_property("geometry", geometry)

    asset.set_editor_property("code", t["code"])
    asset.set_editor_property("display_name", unreal.Text(t["name"]))
    asset.set_editor_property("mesh", m["mesh"])
    asset.set_editor_property("anim_class", anim)
    asset.set_editor_property("towed", t["towed"])

    path = "%s/%s" % (FOLDER, t["asset"])
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("%s: %s \"%s\"%s" % (t["asset"], t["code"], t["name"], "  TOWED" if t["towed"] else ""))
    say("    axles   fixed %.1f  steer %.1f  (wheelbase %.1f)"
        % (m["fixed"], m["steer"], m["steer"] - m["fixed"]))
    say("    body    front %.1f  rear %.1f  width %.1f  (mesh %.1f; left out: %s)"
        % (m["front"], m["rear"], m["width"], m["mesh_width"],
           ", ".join(m["mirrors"]) if m["mirrors"] else "nothing"))
    return True


def run():
    bad = []
    for t in TYPES:
        m = measure(t)
        if m is None or not author(t, m):
            bad.append(t["asset"])
    say("vehicle types: %s" % ("ALL CHECKS PASSED" if not bad else "FAILED: " + ", ".join(bad)))
    say("DONE")


run()
