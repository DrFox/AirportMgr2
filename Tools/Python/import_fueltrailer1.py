"""Imports fuelTrailer1 - the drawbar bowser utility1 tows - as ONE skeletal mesh. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

A SIBLING OF import_rig.py, NOT AN EXTENSION OF IT. import_rig.py's own header argues "this is
a NEW model, so it is the moment to use the mechanism [airside_import.Spec/import_one] rather
than grow a fourth copy" - that argument is about the MECHANISM, which this script also reuses.
It is a separate FILE because it is a separate ASSET FAMILY: import_rig.py's two Specs are two
halves of ONE towed rig (truckCab1 + tankTrailer1, both under Content/Vehicles/Rig/, one
import run). fuelTrailer1 belongs to utility1's family instead - utility1 is already imported,
by a run this script does not repeat - and its own asset lands at Content/Vehicles/FuelTrailer1,
a sibling of Content/Vehicles/Utility1, not a third folder under Rig/. One file per family
keeps "what does importing utility1 + fuelTrailer1 involve" answerable by reading one script,
the same reason import_rig.py itself is not a third copy of import_fueltruck.py.

RIG (build_export_fueltrailer.py's own header, utility1/scripts/build_export_fueltrailer.py):
  root
  +- wheel_RL, wheel_RR            roll (lateral axis), children of root - the rear axle is rigid
  +- steer_FL, steer_FR            vertical at the wheel centre
  |  +- wheel_FL, wheel_FR         roll
  +- towbar_yaw                    vertical, front axle centre - the turntable pivots here
  |  +- towbar                     lateral: pitch. Rest = level = coupled
  |     +- tow_eye                 SOCKET: snapped onto utility1's hitch (see report_tow_chain)
  +- hitch                         SOCKET: the next cart's tow_eye would go here (unused - this
                                    trailer sits at the end of the train)

tow_eye AND hitch ARE SOCKETS, NOT DRIVEN BONES - see build_rig_anim.py's own header for the
'wheel' substring trap a name-based bone plan would fall into with 'fifth_wheel'/'kingpin'; the
same reasoning applies here, and is repeated at build_rig_anim.py's own RIGS entry for this
model rather than taught to airside_anim.BONE_RULES for one rig's naming.

ORIGIN: the trailer's OWN rear axle ground point (build_export_fueltrailer.py's own comment:
"Export space: the trailer's own rear-axle ground point (ft_root) at the origin"), which is
what lets its Content/Vehicles/FuelTrailer1 sit beside Utility1 rather than nested under it -
the two are coupled at runtime by UAirsideSettings::ResolveUtilityTowVehicle's FTowLink, not by
sharing an origin.
"""
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import airside_import  # noqa: E402
from airside_import import Spec, fail, say  # noqa: E402

# fuelTrailer1.glb ships from UTILITY1'S export/ folder (utility1/scripts/build_export_
# fueltrailer.py: "the towed asset is built by scripts inside the tower's file"), a sibling of
# utility1.glb rather than a folder of its own - the same shape tankTrailer1.glb ships from
# truckCab1's export/, which is why airside_import.EXPORT_FOLDER maps both to their tower.
MODELS_ROOT = r"C:\repos\AirportMgr2Models\utility1\export"
UTILITY1_MESH = "/Game/Vehicles/Utility1/SK_Utility1"

SPEC = Spec(
    key="fuelTrailer1",
    source=os.path.join(MODELS_ROOT, "fuelTrailer1.glb"),
    mesh_dir="/Game/Vehicles/FuelTrailer1",
    skel_name="SK_FuelTrailer1",
    front_nodes=("fuelTrailer1_wheel_FL", "fuelTrailer1_wheel_FR"),
    rear_nodes=("fuelTrailer1_wheel_RL", "fuelTrailer1_wheel_RR"),
    front_label="front (steered) axle",
    rear_label="rear axle",
    # THE REAR AXLE IS THE ORIGIN (build_export_fueltrailer.py's own assertion: "rear axle
    # centre not on the origin" fails the export otherwise) - the SAME convention utility1
    # itself uses, so the two meshes agree about which end of each is "home".
    origin_on="rear",
    note="the 2,000 L bowser drawbar trailer utility1 tows (utility1/README.md's own section, "
         "2026-09-24). tow_eye and hitch are coupling SOCKETS, not driven bones - see "
         "build_rig_anim.py's RIGS entry for this model.",
)

# Mesh node stems (asset-prefixed export names, from fueltrailer_materials.py's PARTS after its
# own RENAME, with the 'ft_' prefix build_export_fueltrailer.py's emit() drops) grouped by which
# TOW LINK they belong to - see report_tow_chain below. NOT joint names: the distinction Spec's
# own docstring makes for front_nodes/rear_nodes applies here too.
TOWBAR_PARTS = ("fuelTrailer1_axle_front", "fuelTrailer1_yoke", "fuelTrailer1_towbar",
                "fuelTrailer1_knuckle_FL", "fuelTrailer1_knuckle_FR",
                "fuelTrailer1_wheel_FL", "fuelTrailer1_wheel_FR")
BODY_PARTS = ("fuelTrailer1_frame", "fuelTrailer1_tank", "fuelTrailer1_tank_fittings",
              "fuelTrailer1_bay", "fuelTrailer1_hose_reel", "fuelTrailer1_nozzle",
              "fuelTrailer1_rear", "fuelTrailer1_mudguards", "fuelTrailer1_axle_rear",
              "fuelTrailer1_wheel_RL", "fuelTrailer1_wheel_RR")


def group_bounds(parts_uu, names):
    """Combined (min, max) over X/Y/Z for a group of part_bounds_uu's own keys."""
    low = [None, None, None]
    high = [None, None, None]
    missing = []
    for name in names:
        found = parts_uu.get(name)
        if found is None:
            missing.append(name)
            continue
        lo, hi = found
        for axis in range(3):
            low[axis] = lo[axis] if low[axis] is None else min(low[axis], lo[axis])
            high[axis] = hi[axis] if high[axis] is None else max(high[axis], hi[axis])
    if missing:
        fail("group_bounds: %d part(s) not found in the export: %s"
             % (len(missing), ", ".join(missing)))
        return None
    return low, high


def bone_world_position(mesh, bone_name):
    """A driven or socket bone's WORLD (component at identity, so component space IS world)
    position off the IMPORTED skeleton's reference pose. unreal.AnimPose.get_bone_pose(...,
    WORLD) - the mechanism airside_anim.bone_frames already uses for ROTATION - is tried
    FIRST, because it does the parent-chain composition correctly and for every bone this
    file asks about except one.

    THE ONE EXCEPTION, MEASURED 2026-09-24: AnimPose.get_bone_names(skeleton.
    get_reference_pose()) drops 'hitch' from SK_Utility1's 9-bone skeleton entirely (8 names
    back, no error), while component.get_bone_name(i) reports all 9 and fuelTrailer1's OWN
    'hitch'/'tow_eye' - equally un-skinned, equally leaf, equally added in the same kind of
    'no mesh, just a socket' bone_table() entry - are NOT dropped from ITS AnimPose. So this
    is a property of THAT asset's compact pose (most likely stale required-bones data left
    over from reimport_utility1.py's reimport, which does not touch LOD build settings), not
    a rule about unskinned bones in general - checked, not assumed, per CLAUDE.md.

    THE FALLBACK COMPOSES BY HAND: a transient SkeletalMeshComponent's get_ref_pose_transform
    gives a bone's LOCAL (parent-relative) reference transform even for a bone AnimPose drops
    - proven separately, since it is where this function's FIRST version wrongly stopped,
    reading tow_eye's PARENT-relative 89.0 uu as if it were tow_eye's WORLD 335.0 uu. Composed
    with the PARENT's own WORLD transform (recursively, in case the parent is ALSO missing -
    none are, in practice, since only leaf sockets are ever dropped) this reaches the same
    answer AnimPose gives everywhere it does not drop the bone - checked against towbar_yaw,
    which both paths agree on to 0.01 uu.
    """
    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is not None:
        pose = skeleton.get_reference_pose()
        if bone_name in (str(n) for n in unreal.AnimPose.get_bone_names(pose)):
            return unreal.AnimPose.get_bone_pose(pose, bone_name, unreal.AnimPoseSpaces.WORLD).translation

    component = unreal.new_object(unreal.SkeletalMeshComponent)
    component.set_skeletal_mesh_asset(mesh)
    index = component.get_bone_index(bone_name)
    if index < 0:
        fail("%s: no bone named %s in the imported skeleton" % (mesh.get_path_name(), bone_name))
        return None
    local = component.get_ref_pose_transform(index)
    parent_name = str(component.get_parent_bone(bone_name))
    if not parent_name:
        # No parent at all: this bone hangs directly off the component, so its own reference
        # transform already IS the world one (true of 'root', never true of a socket).
        return local.translation
    say("NOTE %s: AnimPose dropped this bone; composing its world position by hand off its "
        "parent '%s'" % (bone_name, parent_name))
    parent_world = bone_world_position(mesh, parent_name)
    parent_rotation = bone_world_rotation(mesh, parent_name)
    if parent_world is None or parent_rotation is None:
        return None
    return parent_rotation.rotate_vector(local.translation) + parent_world


def bone_world_rotation(mesh, bone_name):
    """The WORLD rotation this file's fallback composition needs alongside the position
    above - AnimPose when the bone is in it (every bone this file calls this for), the
    transient component's local rotation otherwise (root only, where it is the identity)."""
    skeleton = mesh.get_editor_property("skeleton")
    if skeleton is not None:
        pose = skeleton.get_reference_pose()
        if bone_name in (str(n) for n in unreal.AnimPose.get_bone_names(pose)):
            return unreal.AnimPose.get_bone_pose(pose, bone_name, unreal.AnimPoseSpaces.WORLD).rotation
    component = unreal.new_object(unreal.SkeletalMeshComponent)
    component.set_skeletal_mesh_asset(mesh)
    index = component.get_bone_index(bone_name)
    return component.get_ref_pose_transform(index).rotation if index >= 0 else None


def report_tow_chain(doc, trailer_mesh):
    """MEASURED figures for UAirsideSettings::ResolveUtilityTowVehicle's two-link FTowLink
    chain (spec 2026-09-24 revision, section 4) - printed so they can be read off the log and
    carried into that function WITH their source and date, as ResolveRigVehicle's own figures
    are. Nothing here writes the C++; this only measures and reports.
    """
    say("-" * 70)
    say("MEASURING the utility1 + fuelTrailer1 tow chain")

    utility1 = unreal.EditorAssetLibrary.load_asset(UTILITY1_MESH)
    if utility1 is None:
        fail("%s not found - utility1 must already be imported (task 3b's brief: "
             "'utility1 is already imported')" % UTILITY1_MESH)
        return

    # LINK 0 (the towbar, a BAR - FTowLink.BodyFront == BodyRear == 0): HitchX is utility1's
    # OWN hitch bone, along utility1's body from ITS fixed (rear) axle - which IS utility1's
    # origin (SPEC.md: "origin at the rear axle centre projected to the ground"), so the
    # bone's X *is* HitchX with no further offset to carry.
    hitch = bone_world_position(utility1, "hitch")
    towbar_yaw = bone_world_position(trailer_mesh, "towbar_yaw")
    tow_eye = bone_world_position(trailer_mesh, "tow_eye")
    root_check = bone_world_position(trailer_mesh, "root")
    if hitch is None or towbar_yaw is None or tow_eye is None or root_check is None:
        return

    say("utility1 'hitch' bone (world, mesh origin = rear axle): X=%.2f Y=%.2f Z=%.3f uu"
        % (hitch.x, hitch.y, hitch.z))
    say("fuelTrailer1 'root' bone (world; should be ~0,0,0 - the origin IS the rear axle): "
        "X=%.2f Y=%.2f Z=%.3f uu" % (root_check.x, root_check.y, root_check.z))
    say("fuelTrailer1 'towbar_yaw' bone (the front/steered axle): X=%.2f Y=%.2f Z=%.3f uu"
        % (towbar_yaw.x, towbar_yaw.y, towbar_yaw.z))
    say("fuelTrailer1 'tow_eye' bone (the towbar's own hitch): X=%.2f Y=%.2f Z=%.3f uu"
        % (tow_eye.x, tow_eye.y, tow_eye.z))

    # HORIZONTAL ONLY (X, Y), NOT THE FULL 3D DISTANCE: FTowLink and VehicleSweep both work in
    # the ROAD PLANE (FVector2D) - see VehicleSweep.h's own FLink and FLinkPose. towbar_yaw and
    # tow_eye sit at the coupling height (Z 30.8, the SAME z 0.308 utility1's hitch and this
    # model's own rear hitch share - the train height the whole chain couples at) while the
    # rear axle is on the ground (Z 0); a 3D .length() would fold that 30.8 uu of vertical
    # separation into Link 1's Length, which is not a fact the 2D routing model represents.
    def horizontal(a, b):
        return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2) ** 0.5

    towbar_length = horizontal(towbar_yaw, tow_eye)
    body_length = horizontal(towbar_yaw, root_check)   # towbar_yaw to the rear (fixed) axle
    say("PASS Link 0 (towbar) Length = horizontal(towbar_yaw, tow_eye) = %.2f uu" % towbar_length)
    say("PASS Link 1 (body)   Length = horizontal(towbar_yaw, root)    = %.2f uu (utility1/"
        "README.md's own 'wheelbase 2.21' x 100 = 221.0)" % body_length)
    say("PASS Link 0 (towbar) HitchX = utility1 'hitch'.X = %.2f uu (README.md's own 'hitch "
        "socket... 0.907 m behind utility1's rear axle' x -100 = -90.7)" % hitch.x)

    parts_uu = airside_import.part_bounds_uu(doc)
    towbar_bounds = group_bounds(parts_uu, TOWBAR_PARTS)
    body_bounds = group_bounds(parts_uu, BODY_PARTS)
    if towbar_bounds is None or body_bounds is None:
        return

    tb_low, tb_high = towbar_bounds
    bd_low, bd_high = body_bounds
    towbar_width = tb_high[1] - tb_low[1]
    body_width = bd_high[1] - bd_low[1]
    say("Towbar-region parts (%s) bounds: X[%.1f, %.1f] Y[%.1f, %.1f] -> Width %.1f uu"
        % (", ".join(TOWBAR_PARTS), tb_low[0], tb_high[0], tb_low[1], tb_high[1], towbar_width))
    say("Body-region parts (%s) bounds: X[%.1f, %.1f] Y[%.1f, %.1f] -> Width %.1f uu"
        % (", ".join(BODY_PARTS), bd_low[0], bd_high[0], bd_low[1], bd_high[1], body_width))

    # BodyFront/BodyRear per FTowLink: how far the link's OWN body reaches ahead of ITS hitch
    # and behind ITS axle - see FTowLink's own comment. Link 1's hitch is towbar_yaw (X =
    # body_length, forward); its axle is the origin (X = 0).
    body_front = bd_high[0] - towbar_yaw.x
    body_rear = -bd_low[0]
    say("PASS Link 1 (body) BodyFront = body-region max X (%.1f) - towbar_yaw.X (%.1f) = %.1f uu"
        % (bd_high[0], towbar_yaw.x, body_front))
    say("PASS Link 1 (body) BodyRear  = -(body-region min X) = -(%.1f) = %.1f uu"
        % (bd_low[0], body_rear))
    say("Link 0 (towbar) is a BAR: BodyFront = BodyRear = 0.0 by definition (FTowLink's own "
        "comment) - its width alone (%.1f uu) is what VehicleSweep::Trace sweeps between its "
        "hitch and its axle." % towbar_width)


def main():
    say("=" * 78)
    say("importing fuelTrailer1 - the drawbar bowser utility1 tows")
    doc = airside_import.read_gltf(SPEC.source)
    ok = airside_import.import_one(SPEC)

    if ok and doc is not None:
        trailer_mesh = unreal.EditorAssetLibrary.load_asset(
            "%s/%s" % (SPEC.mesh_dir, SPEC.skel_name))
        if trailer_mesh is not None:
            report_tow_chain(doc, trailer_mesh)

    say("-" * 70)
    say("fuelTrailer1 import: %s" % ("ALL CHECKS PASSED" if ok else "SOME CHECKS FAILED - see above"))
    say("DONE")


main()
