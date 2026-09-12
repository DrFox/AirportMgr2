"""Points each aircraft type at its OWN mesh and anim Blueprint. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log.

WHY THIS EXISTS. UAircraftType had no mesh at all until a Twin Otter was offered and a Piper
Meridian landed: UAirsideTraffic handed every aircraft agent UAirsideContent::AgentMesh, one
skeletal mesh for the whole game, and nothing noticed while the project had exactly one
aircraft model to wear. The mesh now travels on FAirframe with the performance figures, so
each type brings its own.

IN PYTHON RATHER THAN IN BuildPiperMeridian, which is where the Piper's other figures come
from. That function lives in Airside, and Airside may not name /Game assets - Check-
Architecture enforces the direction. A mesh reference is content, so it is set from the
content layer, the same way build_road_profiles.py wires a profile into the content set.

IN PLACE, never delete-and-recreate. DA_Aircraft_Piper is referenced by DA_Airline_Cumbria's
fleet, and deleting an asset something points at breaks the reference rather than updating
it - build_airlines.py's own load-or-create carries the same warning.

THE CONTENT DEFAULT REMAINS, and is now a real fallback rather than the only answer: a type
with no mesh of its own still draws as UAirsideContent::AgentMesh. What changed is that
nothing RELIES on it.
"""
import unreal

# type asset -> (skeletal mesh, anim blueprint, short code)
#
# The short code is NOT the ICAO aerodrome letter that UAircraftType::Code holds - that is
# "A" for a Meridian and "C" for both an A320 and a 737, so it cannot name a type.
# FAirframe::TypeCode takes this instead.
LOOKS = {
    "/Game/Entities/DA_Aircraft_Piper": (
        "/Game/Aircraft/PiperMeridian/SK_PiperMeridian",
        "/Game/Aircraft/PiperMeridian/ABP_PiperMeridian",
        "PA46",
    ),
    "/Game/Entities/DA_Aircraft_Plane2": (
        "/Game/Aircraft/Plane2/SK_Plane2",
        "/Game/Aircraft/Plane2/ABP_Plane2",
        "DHC6",
    ),
}


def say(msg):
    unreal.log("MARKER: " + str(msg))


def fail(msg):
    unreal.log_error("MARKER: FAIL " + str(msg))


def wire(type_path, mesh_path, abp_path, short_code):
    aircraft = unreal.EditorAssetLibrary.load_asset(type_path)
    if aircraft is None:
        say("%s absent; skipped" % type_path)
        return None

    mesh = unreal.EditorAssetLibrary.load_asset(mesh_path)
    if mesh is None:
        fail("%s has no mesh at %s" % (type_path, mesh_path))
        return None
    aircraft.set_editor_property("mesh", mesh)

    abp = unreal.EditorAssetLibrary.load_asset(abp_path)
    if abp is None or abp.generated_class() is None:
        # Not fatal: a type with a mesh and no anim Blueprint falls back to the content
        # default one, which drives bones by name and is better than no animation.
        say("%s: no compiled anim Blueprint at %s; leaving the content default"
            % (type_path, abp_path))
    else:
        aircraft.set_editor_property("anim_class", abp.generated_class())

    aircraft.set_editor_property("short_code", unreal.Name(short_code))
    unreal.EditorAssetLibrary.save_asset(type_path, only_if_is_dirty=False)

    flagged, already = flag_materials(mesh)
    say("%s: %d material(s) newly flagged for skeletal use, %d already were"
        % (type_path.split("/")[-1], flagged, already))
    return type_path


def flag_materials(mesh):
    """Every material on this mesh marked as usable on a SKELETAL mesh.

    THE FLAG THAT TURNS A CORRECT MATERIAL GREY. A UMaterial without
    bUsedWithSkeletalMesh cannot compile for the skeletal vertex factory, and the renderer
    substitutes the DEFAULT material rather than failing - so a component holds the right
    eight materials, logs the right eight names, and draws a grey aeroplane. Reported as
    "when the twotter spawned none of its materials were set".

    It bites glTF materials in particular: Interchange generates them during an import that
    produced a STATIC mesh, so the skeletal flag is false. The editor sets it itself on first
    use and recompiles - which is why such a mesh looks right in the skeletal mesh editor and
    wrong in play - but that only marks the material DIRTY, so the fix dies with the session
    unless somebody saves it.
    """
    flagged, already = 0, 0
    for slot in mesh.get_editor_property("materials"):
        interface = slot.material_interface
        if interface is None:
            continue
        # A material instance inherits the flag from its parent, so the parent is what to set.
        material = interface.get_base_material() if hasattr(interface, "get_base_material") else interface
        if not isinstance(material, unreal.Material):
            continue
        if material.get_editor_property("used_with_skeletal_mesh"):
            already += 1
            continue
        material.set_editor_property("used_with_skeletal_mesh", True)
        unreal.EditorAssetLibrary.save_asset(
            material.get_path_name().split(".")[0], only_if_is_dirty=False)
        flagged += 1
    return flagged, already


def verify():
    """Read back from disk, and compare the types AGAINST EACH OTHER.

    Asserting that each points at the right asset would pass while both pointed at the same
    one; two types resolving to the SAME mesh is the defect, so that is what is checked.
    """
    seen = {}
    for type_path in LOOKS:
        aircraft = unreal.EditorAssetLibrary.load_asset(type_path)
        if aircraft is None:
            continue
        mesh = aircraft.get_editor_property("mesh")
        code = aircraft.get_editor_property("short_code")
        name = type_path.split("/")[-1]
        if mesh is None:
            fail("%s still has no mesh; it would wear the game-wide default" % name)
            continue
        unflagged = []
        for slot in mesh.get_editor_property("materials"):
            interface = slot.material_interface
            if interface is None:
                continue
            base = interface.get_base_material() if hasattr(interface, "get_base_material") else interface
            if isinstance(base, unreal.Material) and not base.get_editor_property(
                    "used_with_skeletal_mesh"):
                unflagged.append(base.get_name())
        if unflagged:
            fail("%s would draw as grey clay: %s lack the skeletal usage flag"
                 % (name, ", ".join(unflagged)))
        else:
            say("PASS %-22s %-6s wears %s, every material skeletal-ready"
                % (name, code, mesh.get_name()))
        seen.setdefault(mesh.get_path_name(), []).append(name)

    for mesh_path, users in seen.items():
        if len(users) > 1:
            fail("%s is worn by %s - two types sharing one mesh is the whole defect"
                 % (mesh_path.split("/")[-1], ", ".join(users)))
    if all(len(users) == 1 for users in seen.values()) and seen:
        say("PASS %d type(s), each wearing its own mesh" % len(seen))


def run():
    for type_path, (mesh_path, abp_path, short_code) in sorted(LOOKS.items()):
        if wire(type_path, mesh_path, abp_path, short_code) is not None:
            say("wired %s" % type_path.split("/")[-1])
    verify()
    say("DONE")


run()
