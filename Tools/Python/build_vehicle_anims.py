"""Creates the ground vehicles' Anim Blueprints, sets each one's measured defaults, and resolves
every driven bone's axis and sign for Tools/wire_vehicle_anim.py. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

THE EDITOR MUST BE CLOSED; the wiring half needs it OPEN. Same split as every
build_<model>_anim.py - see docs/2026-09-20-animgraph-authoring.md.

ONE SCRIPT FOR FOUR VEHICLES, not four copies, because the aircraft scripts' own history says
where copies go: airside_anim.py exists because four copies of the bone plan had drifted. The
table below is what is true of each vehicle; everything else is mechanism.

WHAT IS DIFFERENT FROM THE AIRCRAFT RESOLVER, AND WHY IT IS NOT airside_anim.resolve_axis.
That function anchors on a left main wheel called wheel_L and takes its SIGN from plane2's
nosewheel_steer - both aircraft facts; no vehicle has either bone. And it assumes every driven
bone turns about ONE local axis, which a vehicle breaks on purpose: the catering platform
SLIDES, and a towbar pitches about the axle direction while a beacon spins about the vertical.
So each row here names the WORLD axis it moves about (lateral, vertical, forward), the local
axis is found by measuring which of the bone's own axes lies along it, and the sign is taken
from the shipped plane2 rig bone for bone - its wheel_L for anything about the lateral axis,
its nosewheel_steer for anything about the vertical. The plane graphs drive those with x-1 and
x+1, so a bone whose axis points the same way as plane2's takes the same multiplier.

A TOWBAR RAISES THE OTHER WAY TO A WHEEL ROLLING FORWARD. Both turn about the lateral axis, but
a forward-rolling wheel carries its FRONT down, and a raised towbar carries its front end UP.
So "raise" takes the opposite sign to "roll" about the same axis. Stated here because it is
the one sign that is reasoned rather than copied; AirportMgr.Content.VehicleTypes.
WorkingPartsMove checks that the tow eye goes up.

THE CATERING LIFT IS A CLIP, NOT BONES. Its nine tracks are a scissor solved in Blender
(catering_pose.py), which no chain of Transform (Modify) Bone nodes should reproduce. The
Lift sequence moves up out of SkeletalMeshes/ to AS_Catering1_Lift, and its length becomes
ABP_Catering1's LiftClipLengthSeconds. The PLATFORM CLIP IS DELETED: its first key sits the
platform 2.70 m below its rest height (read off catering1.glb on 2026-09-25), and a one-axis
slide is a bone node's job - see UAirsideAgentAnim::PlatformOffsetUu.

THE CART'S TURNTABLE IS NOT WIRED. steer_FL/FR and towbar_yaw follow the TOWBAR's angle, not
the cart's own steering - feature/articulated-rig's build_rig_anim.py makes the same ruling
for fuelTrailer1 and adds UAirsideAgentAnim::TowbarAngleDegrees for it. Once that lands those
three bones wire to it; driving them from SteerAngleDegrees now would be a wrong answer rather
than a missing one.

Every result line is prefixed MARKER: - it lands in Saved/Logs/AirportMgr.log, not stdout.
"""
import json
import os
import struct
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from airside_anim import bone_frames, fail, joint_names, say  # noqa: E402

MODELS = r"C:\repos\AirportMgr2Models"

# THE SHIPPED RIG THE SIGNS ARE READ AGAINST. See the header.
REFERENCE_MESH = "/Game/Aircraft/Plane2/SK_Plane2"
REFERENCE = {
    # world axis -> (plane2 bone whose axis defines "positive", the multiplier plane2's graph uses)
    "lateral": ("wheel_L", -1.0),
    "vertical": ("nosewheel_steer", 1.0),
}

WORLD = {"lateral": (0.0, 1.0, 0.0), "vertical": (0.0, 0.0, 1.0), "forward": (1.0, 0.0, 0.0)}

# kind -> (world axis, how it drives, extra sign on top of the reference's)
KINDS = {
    "roll": ("lateral", "rotate", 1.0),
    "raise": ("lateral", "rotate", -1.0),   # see the header: a towbar lifts the other way
    "steer": ("vertical", "rotate", 1.0),
    "spin": ("vertical", "rotate", 1.0),
    "slide": ("forward", "translate", 1.0),
}

ROTATOR_PIN = {0: "Roll", 1: "Pitch", 2: "Yaw"}
VECTOR_PIN = {0: "X", 1: "Y", 2: "Z"}

# 60 RPM - a flash a second. See UAirsideAgentAnim::BeaconRPM.
BEACON_RPM = 60.0

WHEELS_4 = [("wheel_RL", "WheelAngleDegrees", "roll"), ("wheel_RR", "WheelAngleDegrees", "roll"),
            ("wheel_FL", "WheelAngleDegrees", "roll"), ("wheel_FR", "WheelAngleDegrees", "roll")]
STEER_2 = [("steer_FL", "SteerAngleDegrees", "steer"), ("steer_FR", "SteerAngleDegrees", "steer")]

# CHAIN ORDER, source first: a bone after every bone it is the parent of (wire_anim_lib's rule).
VEHICLES = [
    dict(key="fueltruck1", folder="/Game/Vehicles/FuelTruck1", name="FuelTruck1",
         glb=r"rigidCab1\export\fueltruck1.glb",
         plan=WHEELS_4 + STEER_2 + [("beacon", "BeaconAngleDegrees", "spin")],
         defaults=dict(beacon_rpm=BEACON_RPM)),
    dict(key="catering1", folder="/Game/Vehicles/Catering1", name="Catering1",
         glb=r"rigidCab1\export\catering1.glb",
         plan=WHEELS_4 + STEER_2 + [("platform", "PlatformOffsetUu", "slide"),
                                    ("beacon", "BeaconAngleDegrees", "spin")],
         # 1.80 m, rigidCab1/README.md - and measured below off the clip's last key too.
         defaults=dict(beacon_rpm=BEACON_RPM, platform_travel_uu=180.0),
         lift_clip=True),
    dict(key="baggageCart1", folder="/Game/Vehicles/BaggageCart1", name="BaggageCart1",
         glb=r"baggageCart1\export\baggageCart1.glb",
         plan=WHEELS_4 + [("towbar", "TowbarPitchDegrees", "raise")],
         # baggageCart1/README.md: "pitch towbar up to about 70 degrees".
         defaults=dict(towbar_raised_angle_degrees=70.0)),
    dict(key="curtainTrailer1", folder="/Game/Vehicles/CurtainTrailer1", name="CurtainTrailer1",
         glb=r"truckCab1\export\curtainTrailer1.glb",
         plan=[("wheel_%d%s" % (axle, side), "WheelAngleDegrees", "roll")
               for axle in (1, 2, 3) for side in ("L", "R")],
         defaults=dict()),
]


def mesh_path(v):
    return "%s/SK_%s" % (v["folder"], v["name"])


def abp_path(v):
    return "%s/ABP_%s" % (v["folder"], v["name"])


def wheel_radius_uu(v):
    """The rolling radius: the wheel bone's height above the ground, off the .glb.

    Every vehicle here stands its tyres on z = 0 with the wheel bone on the axle, so the bone's
    height IS the radius - the same reading import_models.py's 'wheels are on the ground' check
    relies on. glTF is Y-up.
    """
    source = os.path.join(MODELS, v["glb"])
    with open(source, "rb") as handle:
        handle.read(12)
        length = struct.unpack("<I4s", handle.read(8))[0]
        doc = json.loads(handle.read(length).decode("utf-8"))
    nodes = doc["nodes"]
    parent = {}
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parent[child] = index
    wheel = v["plan"][0][0]
    for index, node in enumerate(nodes):
        if node.get("name") == wheel:
            height = 0.0
            at = index
            while at is not None:
                height += nodes[at].get("translation", [0.0, 0.0, 0.0])[1]
                at = parent.get(at)
            return height * 100.0
    return None


def resolve(v):
    """[(bone, variable, mode, pin, multiplier)] or None - measured, per bone."""
    frames = bone_frames(mesh_path(v))
    reference = bone_frames(REFERENCE_MESH)
    if not frames or not reference:
        fail("%s: no frames to measure (mesh or %s missing)" % (v["key"], REFERENCE_MESH))
        return None
    rows = []
    for bone, variable, kind in v["plan"]:
        if bone not in frames:
            fail("%s has no bone %s - the rig was renamed. Bones: %s"
                 % (v["key"], bone, ", ".join(sorted(frames))))
            return None
        world_axis, mode, extra = KINDS[kind]
        want = WORLD[world_axis]
        best, dot = 0, 0.0
        for index, axis in enumerate(frames[bone]):
            d = sum(a * b for a, b in zip(axis, want))
            if abs(d) > abs(dot):
                best, dot = index, d
        if abs(dot) < 0.99:
            fail("%s.%s: no local axis lies along UE %s (best %s at %.2f) - a driven bone "
                 "should be square to the vehicle" % (v["key"], bone, world_axis, "XYZ"[best], dot))
            return None
        if mode == "translate":
            # POSITIVE OFFSET GOES FORWARD: the sign is simply which way the chosen local axis
            # points along UE +X. No reference rig slides anything.
            mul = 1.0 if dot > 0 else -1.0
            pin = VECTOR_PIN[best]
        else:
            ref_bone, ref_mul = REFERENCE[world_axis]
            ref_axes = reference[ref_bone]
            ref_best, ref_dot = 0, 0.0
            for index, axis in enumerate(ref_axes):
                d = sum(a * b for a, b in zip(axis, want))
                if abs(d) > abs(ref_dot):
                    ref_best, ref_dot = index, d
            # SAME DIRECTION AS PLANE2'S BONE IN UE SPACE -> plane2's multiplier.
            agree = 1.0 if (dot > 0) == (ref_dot > 0) else -1.0
            mul = ref_mul * agree * extra
            pin = ROTATOR_PIN[best]
        rows.append((bone, variable, mode, pin, mul))
        say("    %-10s %-20s %-9s %-5s x %+.0f  (%s about UE %s, local %s at %.3f)"
            % (bone, variable, mode, pin, mul, kind, world_axis, "XYZ"[best], dot))
    return rows


def write_plan(v, rows):
    path = os.path.join(unreal.Paths.project_saved_dir(), "%s_vehicle_plan.json" % v["key"])
    payload = {"mesh": mesh_path(v), "abp": abp_path(v),
               "lift_clip": ("%s/AS_%s_Lift" % (v["folder"], v["name"])) if v.get("lift_clip") else None,
               "rows": [dict(bone=b, variable=var, mode=m, pin=p, mul=mul)
                        for b, var, m, p, mul in rows]}
    with open(path, "w") as handle:
        json.dump(payload, handle, indent=2)
    say("wrote %s - Tools/wire_vehicle_anim.py wires from it" % path)


def settle_lift_clip(v):
    """Move Lift up beside the mesh as AS_<Name>_Lift, drop Platform. Returns its length or None."""
    folder = v["folder"]
    target = "%s/AS_%s_Lift" % (folder, v["name"])
    loose = "%s/SkeletalMeshes/Lift" % folder
    if not unreal.EditorAssetLibrary.does_asset_exist(target):
        if not unreal.EditorAssetLibrary.does_asset_exist(loose):
            fail("no Lift clip at %s or %s - re-import catering1" % (target, loose))
            return None
        if not unreal.EditorAssetLibrary.rename_asset(loose, target):
            fail("could not move %s -> %s" % (loose, target))
            return None
        say("moved %s -> %s" % (loose, target))
    platform = "%s/SkeletalMeshes/Platform" % folder
    if unreal.EditorAssetLibrary.does_asset_exist(platform):
        unreal.EditorAssetLibrary.delete_asset(platform)
        say("deleted %s - its first key is wrong; a bone node drives the platform" % platform)
    clip = unreal.EditorAssetLibrary.load_asset(target)
    length = clip.get_play_length() if hasattr(clip, "get_play_length") else \
        clip.get_editor_property("sequence_length")
    skeleton = clip.get_editor_property("skeleton")
    say("%s: %.3f s on %s" % (target, length, skeleton.get_name() if skeleton else None))
    unreal.EditorAssetLibrary.save_asset(target, only_if_is_dirty=False)
    return length


def ensure_abp(v):
    path = abp_path(v)
    skeleton = unreal.EditorAssetLibrary.load_asset("%s/SK_%s_Skeleton" % (v["folder"], v["name"]))
    if skeleton is None:
        fail("no skeleton for %s - import it first" % v["key"])
        return None
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        say("%s exists; its graph is Tools/wire_vehicle_anim.py's to rebuild" % path)
        return unreal.EditorAssetLibrary.load_asset(path)
    factory = unreal.AnimBlueprintFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    factory.set_editor_property("parent_class", unreal.AirsideAgentAnim)
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "ABP_%s" % v["name"], v["folder"], unreal.AnimBlueprint, factory)
    if asset is None:
        fail("could not create %s" % path)
        return None
    unreal.EditorAssetLibrary.save_asset(path, only_if_is_dirty=False)
    say("created %s" % path)
    return asset


def set_defaults(v, abp, defaults):
    generated = abp.generated_class()
    if generated is None:
        fail("%s has no generated class" % abp_path(v))
        return False
    cdo = unreal.get_default_object(generated)
    for prop, value in sorted(defaults.items()):
        cdo.set_editor_property(prop, value)
    unreal.EditorAssetLibrary.save_asset(abp_path(v), only_if_is_dirty=False)
    got = unreal.get_default_object(
        unreal.EditorAssetLibrary.load_asset(abp_path(v)).generated_class())
    ok = True
    for prop, value in sorted(defaults.items()):
        back = got.get_editor_property(prop)
        if abs(back - value) > 0.01:
            fail("%s.%s read back %.3f, wanted %.3f" % (abp_path(v), prop, back, value))
            ok = False
        else:
            say("    %-30s %.3f" % (prop, back))
    return ok


def run():
    bad = []
    for v in VEHICLES:
        say("=" * 70)
        say("%s" % v["key"])
        abp = ensure_abp(v)
        if abp is None:
            bad.append(v["key"])
            continue
        defaults = dict(v["defaults"])
        radius = wheel_radius_uu(v)
        if radius is None:
            fail("%s: could not measure the wheel radius" % v["key"])
            bad.append(v["key"])
            continue
        defaults["main_wheel_radius"] = radius
        if v.get("lift_clip"):
            length = settle_lift_clip(v)
            if length is None:
                bad.append(v["key"])
                continue
            defaults["lift_clip_length_seconds"] = length
        if not set_defaults(v, abp, defaults):
            bad.append(v["key"])
        rows = resolve(v)
        if rows is None or len(rows) != len(v["plan"]):
            bad.append(v["key"])
            continue
        write_plan(v, rows)
        names = set(joint_names(os.path.join(MODELS, v["glb"])))
        undriven = sorted(names - set(b for b, _, _ in v["plan"]) - {"root"})
        say("    NOT DRIVEN: %s" % (", ".join(undriven) if undriven else "nothing"))
    say("=" * 70)
    say("vehicle anims: %s" % ("ALL CHECKS PASSED" if not bad else "FAILED: " + ", ".join(bad)))
    say("DONE")


run()
