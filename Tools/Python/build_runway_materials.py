"""Authors M_RunwayGrass, M_RunwayTarmac and M_RunwayConcrete and points DA_AirsideContent
at them. Run headless, EDITOR CLOSED:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

Run AFTER build_road_material.py, which declares the SurfaceTint parameter these tint.

THREE INSTANCES OF M_RoadSurface, not three materials (spec 2026-09-07 §4.3). A runway's
pavement is the road surface with two differences: no yellow centreline, and a tint per
surface. Both are parameters of the parent, so an instance says all there is to say and a
designer can retune either in the details panel without a script. Reinforced has no
material: it LOOKS like concrete, the difference is a rating (spec §8).

CentrelineWidth = 0 is what removes the yellow line (§4.2): M_RoadSurface's centreline mask
is 1 - saturate((|lateral| - CentrelineWidth) * MarkingSharpness), and with the width at
zero only lateral == 0 exactly is painted - a line of zero width. The white markings are
a separate mesh (FRunwayMarkingBuilder) and do not go through this mask.
"""
import unreal

MAT_DIR = "/Game/Materials"
PARENT = "%s/M_RoadSurface" % MAT_DIR
CONTENT = "/Game/DA_AirsideContent"

# name -> (tint, content property). Tints multiply the asphalt albedo, so > 1 lightens.
# Tarmac is the road as it is. Concrete is lifted toward a pale, slightly cool grey.
# Grass is a green-brown: a placeholder over an asphalt texture until a grass set exists,
# chosen to read as "not pavement" from the build camera rather than to look like turf.
INSTANCES = [
    ("M_RunwayTarmac",   unreal.LinearColor(1.00, 1.00, 1.00, 1.0), "runway_tarmac_material"),
    ("M_RunwayConcrete", unreal.LinearColor(1.90, 1.90, 2.05, 1.0), "runway_concrete_material"),
    ("M_RunwayGrass",    unreal.LinearColor(0.55, 0.80, 0.35, 1.0), "runway_grass_material"),
]


def build_instance(name, tint, parent):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    lib = unreal.MaterialEditingLibrary
    path = "%s/%s" % (MAT_DIR, name)

    # Replaced in place, for the reason build_road_material_set.py gives: an asset already
    # in memory reports a successful delete while staying loaded, and create_asset then
    # refuses unattended.
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        existing = unreal.EditorAssetLibrary.load_asset(path)
        if existing is not None:
            unreal.EditorAssetLibrary.delete_loaded_asset(existing)
        else:
            unreal.EditorAssetLibrary.delete_asset(path)
        unreal.log("MARKER: replaced existing %s" % path)

    instance = tools.create_asset(
        name, MAT_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    if instance is None:
        unreal.log_error("MARKER: create_asset returned None for %s" % path)
        return None

    lib.set_material_instance_parent(instance, parent)
    lib.set_material_instance_scalar_parameter_value(instance, "CentrelineWidth", 0.0)
    lib.set_material_instance_vector_parameter_value(instance, "SurfaceTint", tint)
    lib.update_material_instance(instance)
    unreal.EditorAssetLibrary.save_asset(path)

    # Read back, not trusted: a parameter name the parent does not declare is accepted
    # silently and does nothing, which is how a tint ends up "set" on a material that
    # never changes colour.
    width = lib.get_material_instance_scalar_parameter_value(instance, "CentrelineWidth")
    colour = lib.get_material_instance_vector_parameter_value(instance, "SurfaceTint")
    unreal.log("MARKER: %s parent=%s CentrelineWidth=%.1f SurfaceTint=(%.2f, %.2f, %.2f)"
               % (path, parent.get_name(), width, colour.r, colour.g, colour.b))
    return instance


def point_content_at(instances):
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        unreal.log_error("MARKER: %s not found - content set not updated" % CONTENT)
        return
    for (name, _tint, prop), instance in zip(INSTANCES, instances):
        if instance is None:
            continue
        content.set_editor_property(prop, instance)
        unreal.log("MARKER: %s.%s = %s" % (CONTENT, prop, name))
    unreal.EditorAssetLibrary.save_asset(CONTENT)


parent = unreal.EditorAssetLibrary.load_asset(PARENT)
if parent is None:
    unreal.log_error("MARKER: %s not found - run build_road_material.py first" % PARENT)
else:
    names = [str(n) for n in unreal.MaterialEditingLibrary.get_vector_parameter_names(parent)]
    if "SurfaceTint" not in names:
        unreal.log_error("MARKER: %s declares no SurfaceTint - rerun build_road_material.py" % PARENT)
    else:
        built = [build_instance(name, tint, parent) for name, tint, _prop in INSTANCES]
        point_content_at(built)
        unreal.log("MARKER: done")
