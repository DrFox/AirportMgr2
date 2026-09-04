"""Imports the Piper Meridian airframe and authors its two materials. Run headless:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

The export at SOURCE is already game-ready and its README states the conventions this script
depends on. Three of them are the kind that fail SILENTLY, so they are restated where the
code acts on them rather than left to a document nobody opens at the moment it matters:

  * The normal map is ALREADY DirectX-style. Flipping green again inverts every lit surface
    in a way that looks like bad lighting rather than a bad import.
  * MetallicRoughness is PACKED - G is roughness, B is metallic - and must be sampled with
    sRGB off, or the whole airframe reads as wet plastic.
  * The FBX declares Z-up, centimetres, UnitScaleFactor 1.0, and bakes -Y-forward into the
    vertices so the aircraft arrives nose-along +X. Overriding axis or scale on import is
    what turns a correct export into a sideways or 100x aircraft.

Origin is the MAIN-GEAR AXLE CENTRE projected to the ground, which is the point an aircraft
pivots about while taxiing - so a taxiing agent needs no offset of its own, and the mesh
sits ON the road at Z = SurfaceZ rather than lifted.
"""
import os
import unreal

SOURCE = r"C:\repos\AirportMgr2Models\piperMeridian\export"

MESH_DIR = "/Game/Aircraft"
TEX_DIR = "/Game/Aircraft/Textures"
MAT_DIR = "/Game/Aircraft/Materials"

MESH_NAME = "SM_PiperMeridian"
BODY_MATERIAL = "M_PiperMeridian"
GLASS_MATERIAL = "M_PiperGlass"

# Published wingspan, in Unreal units. The export was scaled to match this exactly, and
# every clearance decision in the sim - stand fit, the router's TooWide refusal - is
# measured against it, so it is worth checking rather than trusting.
EXPECTED_WINGSPAN_UU = 1311.0

# name -> (file, is_srgb, compression)
MAPS = {
    "T_Piper_BaseColor": (
        "PiperMeridian_BaseColor.jpg", True, unreal.TextureCompressionSettings.TC_DEFAULT),
    "T_Piper_Normal": (
        "PiperMeridian_Normal.png", False, unreal.TextureCompressionSettings.TC_NORMALMAP),
    # NOT TC_MASKS. GetSamplerTypeForTexture maps TC_Masks to SAMPLERTYPE_Masks, and
    # VerifySamplerType rejects a mismatch outright - the material then fails to compile and
    # every surface using it silently falls back to the engine default.
    "T_Piper_MR": (
        "PiperMeridian_MetallicRoughness.png", False,
        unreal.TextureCompressionSettings.TC_DEFAULT),
}


def import_textures():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    imported = {}

    for name, (filename, srgb, compression) in MAPS.items():
        path = os.path.join(SOURCE, filename)
        if not os.path.isfile(path):
            unreal.log_error("MARKER: missing source texture %s" % path)
            continue

        task = unreal.AssetImportTask()
        task.filename = path
        task.destination_path = TEX_DIR
        task.destination_name = name
        task.automated = True
        task.replace_existing = True
        task.save = True
        tools.import_asset_tasks([task])

        asset = unreal.load_asset("%s/%s" % (TEX_DIR, name))
        if asset is None:
            unreal.log_error("MARKER: import failed for %s" % name)
            continue

        asset.set_editor_property("srgb", srgb)
        asset.set_editor_property("compression_settings", compression)

        if name == "T_Piper_Normal":
            # LEFT OFF DELIBERATELY. The export is already written DirectX-style, which is
            # what Unreal samples; flipping here would double-flip and light every panel
            # from the wrong side - wrong in a way that reads as a lighting problem.
            asset.set_editor_property("flip_green_channel", False)

        unreal.EditorAssetLibrary.save_asset("%s/%s" % (TEX_DIR, name))
        imported[name] = asset
        unreal.log("MARKER: imported %s srgb=%s" % (name, srgb))

    return imported


def import_mesh():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = os.path.join(SOURCE, "PiperMeridian.fbx")
    if not os.path.isfile(path):
        unreal.log_error("MARKER: missing %s" % path)
        return None

    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_as_skeletal", False)
    options.set_editor_property("automated_import_should_detect_type", False)
    options.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_STATIC_MESH)

    static_mesh_data = options.static_mesh_import_data
    # NO axis or scale overrides. The FBX declares Z-up and UnitScaleFactor 1.0, and
    # -Y-forward is baked into the vertices, so the defaults are already correct. Setting
    # any of these is what turns a correct export into a sideways or 100x aircraft.
    # COMBINED into one asset. The FBX holds the airframe and the glazing as two nodes;
    # left separate the importer names each asset after its node, ignores destination_name,
    # and an agent would have to carry two components that must be kept in step. Combined
    # they are one mesh with two MATERIAL SLOTS, which is the distinction that matters -
    # the glass still takes its own translucent material.
    static_mesh_data.set_editor_property("combine_meshes", True)
    # YAW -90, and this is a CORRECTION rather than a preference.
    #
    # The export's README says -Y-forward "becomes +X in Unreal on FBX export". It does not,
    # with these settings: measured on import, the span sat on X at [-655.5, 655.5] and the
    # fuselage on Y at [-531.5, 385.1].
    #
    # WHICH END IS THE NOSE was then got wrong by reasoning and right by measuring. The
    # origin is the main-gear axle, so the two Y reaches are nose and tail - and
    # renders/aligned_side.png shows the prop about 3.7 m ahead of the main gear and the
    # tail about 5.5 m behind it. The NOSE IS THE SHORTER SIDE, +385.1, so forward is +Y:
    # the exporter negated Blender's -Y. A yaw of +90 would have put the TAIL on +X and the
    # aircraft would have taxied backwards, at the right size, along the right line.
    #
    # -90 maps +Y to +X and X to -Y, which puts nose and span where UAircraftType's local
    # space says they are (+X forward, +Y starboard).
    #
    # Corrected HERE rather than by rotating the component, so the asset matches the stated
    # convention and nothing downstream carries a fudge that would have to be remembered.
    # Keyword args deliberately: unreal.Rotator() positional order is (roll, pitch, yaw).
    static_mesh_data.set_editor_property(
        "import_rotation", unreal.Rotator(roll=0.0, pitch=0.0, yaw=-90.0))
    static_mesh_data.set_editor_property("generate_lightmap_u_vs", True)
    static_mesh_data.set_editor_property("auto_generate_collision", False)

    task = unreal.AssetImportTask()
    task.filename = path
    task.destination_path = MESH_DIR
    task.destination_name = MESH_NAME
    task.automated = True
    task.replace_existing = True
    task.save = True
    task.options = options
    tools.import_asset_tasks([task])

    mesh = unreal.load_asset("%s/%s" % (MESH_DIR, MESH_NAME))
    if mesh is None:
        # combine_meshes False means the importer may name the asset after the FBX node
        # rather than destination_name. Report what actually landed instead of guessing.
        found = unreal.EditorAssetLibrary.list_assets(MESH_DIR, recursive=False)
        unreal.log_error("MARKER: mesh not at %s/%s - directory holds: %s"
                         % (MESH_DIR, MESH_NAME, found))
        return None

    unreal.log("MARKER: imported mesh %s/%s" % (MESH_DIR, MESH_NAME))
    return mesh


def check_scale(mesh):
    """The import against the PUBLISHED wingspan, not against itself.

    Wingspan is on X in Blender and the export turns -Y-forward into +X, so in Unreal the
    span lies on Y. A silent scale error here would not look wrong on screen - it would
    look like a slightly different aircraft - while making every clearance number in the
    sim quietly false.
    """
    bounds = mesh.get_bounding_box()
    span = bounds.max.y - bounds.min.y
    length = bounds.max.x - bounds.min.x
    height = bounds.max.z - bounds.min.z

    unreal.log("MARKER: bounds X[%.1f, %.1f] Y[%.1f, %.1f] Z[%.1f, %.1f]"
               % (bounds.min.x, bounds.max.x, bounds.min.y, bounds.max.y,
                  bounds.min.z, bounds.max.z))
    unreal.log("MARKER: extents X=%.1f Y=%.1f Z=%.1f uu" % (length, span, height))

    if abs(span - EXPECTED_WINGSPAN_UU) > 5.0:
        unreal.log_error(
            "MARKER: wingspan is %.1f uu, expected %.1f - the import scaled wrong, or the "
            "export changed. Every clearance check in the sim uses this number."
            % (span, EXPECTED_WINGSPAN_UU))
        return False

    # Wheels on the ground is what lets a taxiing agent sit at Z = SurfaceZ with no lift.
    if abs(bounds.min.z) > 5.0:
        unreal.log_warning(
            "MARKER: mesh bottom is at Z=%.1f, not 0 - a taxiing agent will float or sink"
            % bounds.min.z)

    # Orientation, not just size. Span and length are 1311 and 916 uu, close enough that a
    # 90-degree error passes any check that only looks at ONE of them - and an aircraft
    # taxiing sideways down its own wingspan is the result.
    if length > span:
        unreal.log_error(
            "MARKER: length(X)=%.1f exceeds span(Y)=%.1f - the airframe is rotated 90 "
            "degrees and would taxi sideways" % (length, span))
        return False

    # WHICH WAY it faces, not merely which axis it lies on. A 180-degree error keeps the
    # span on Y, keeps the length on X, and passes every check above - it just taxis
    # backwards. Measured against the side render: nose 3.85 m ahead of the main gear,
    # tail 5.32 m behind it, so the nose reach is the SHORTER of the two.
    nose = bounds.max.x
    tail = -bounds.min.x
    if nose >= tail:
        unreal.log_error(
            "MARKER: +X reach is %.1f and -X reach is %.1f - the longer side is forward, so "
            "the airframe is backwards and would taxi tail-first" % (nose, tail))
        return False

    unreal.log("MARKER: wingspan %.1f uu on Y, length %.1f uu on X, nose +%.1f tail -%.1f "
               "- correct, to scale, and facing forward" % (span, length, nose, tail))
    return True


def build_material(name, textures, translucent):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, name)

    # Replaced through delete_loaded_asset: a material referenced from C++ is already in
    # memory when this runs, and delete_asset reports success while the package stays
    # loaded, after which create_asset refuses unattended.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    material = tools.create_asset(name, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        unreal.log_error("MARKER: create_asset returned None for %s" % path)
        return None

    if translucent:
        # Glass was kept as its own mesh and slot precisely so it could take a real
        # translucent material - baking glazing into the opaque set is what makes windows
        # read as painted on.
        material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        material.set_editor_property("two_sided", True)

        tint = lib.create_material_expression(
            material, unreal.MaterialExpressionVectorParameter, -600, -300)
        tint.set_editor_property("parameter_name", "GlassTint")
        tint.set_editor_property("default_value", unreal.LinearColor(0.05, 0.07, 0.09, 1.0))
        lib.connect_material_property(tint, "", unreal.MaterialProperty.MP_BASE_COLOR)

        opacity = lib.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -600, 0)
        opacity.set_editor_property("parameter_name", "Opacity")
        opacity.set_editor_property("default_value", 0.35)
        lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

        rough = lib.create_material_expression(
            material, unreal.MaterialExpressionScalarParameter, -600, 200)
        rough.set_editor_property("parameter_name", "Roughness")
        rough.set_editor_property("default_value", 0.05)
        lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)

        lib.recompile_material(material)
        unreal.EditorAssetLibrary.save_asset(path)
        unreal.log("MARKER: %s built (translucent)" % path)
        return material

    def sample(key, y, sampler=None):
        texture = textures.get(key)
        if texture is None:
            return None
        node = lib.create_material_expression(
            material, unreal.MaterialExpressionTextureSample, -800, y)
        node.set_editor_property("texture", texture)
        if sampler is not None:
            node.set_editor_property("sampler_type", sampler)
        return node

    base = sample("T_Piper_BaseColor", -400)
    normal = sample("T_Piper_Normal", 0, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    packed = sample("T_Piper_MR", 400, unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    if base is not None:
        lib.connect_material_property(base, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    else:
        unreal.log_warning("MARKER: no base colour, the airframe will be untextured")

    if normal is not None:
        lib.connect_material_property(normal, "", unreal.MaterialProperty.MP_NORMAL)

    if packed is not None:
        # THE PACKING, stated where it is used: G is roughness and B is metallic. Wired to
        # the wrong channels the aircraft still renders, just as chrome or as matte clay -
        # a mistake with no error message attached to it.
        lib.connect_material_property(packed, "G", unreal.MaterialProperty.MP_ROUGHNESS)
        lib.connect_material_property(packed, "B", unreal.MaterialProperty.MP_METALLIC)
    else:
        unreal.log_warning("MARKER: no packed MR map, using material defaults")

    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(path)
    unreal.log("MARKER: %s built" % path)
    return material


def assign_materials(mesh, body, glass):
    """Slot 0 is the airframe, slot 1 the glazing - the order the FBX declares them."""
    if mesh is None:
        return

    slots = mesh.get_editor_property("static_materials")
    unreal.log("MARKER: mesh has %d material slot(s)" % len(slots))

    for index, slot in enumerate(slots):
        name = str(slot.get_editor_property("material_slot_name"))
        chosen = glass if "Glass" in name else body
        if chosen is None:
            continue
        slot.set_editor_property("material_interface", chosen)
        unreal.log("MARKER: slot %d '%s' -> %s" % (index, name, chosen.get_name()))

    mesh.set_editor_property("static_materials", slots)
    unreal.EditorAssetLibrary.save_asset("%s/%s" % (MESH_DIR, MESH_NAME))


textures = import_textures()
mesh = import_mesh()
if mesh is not None:
    check_scale(mesh)
    body = build_material(BODY_MATERIAL, textures, translucent=False)
    glass = build_material(GLASS_MATERIAL, textures, translucent=True)
    assign_materials(mesh, body, glass)
    unreal.log("MARKER: done")
