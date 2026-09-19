"""Authors M_RunwayGrass, M_RunwayTarmac and M_RunwayConcrete and points DA_AirsideContent
at them. Run headless, EDITOR CLOSED:

  UnrealEditor-Cmd.exe <project> -run=pythonscript -script=<this file> -unattended -nosplash -nopause

Every result line is prefixed MARKER: so it can be grepped out of the log, because print()
goes to the log rather than stdout under the commandlet.

Run AFTER build_road_material.py, which declares the BaseColour parameter these set.

THREE INSTANCES OF M_RoadSurface, not three materials (spec 2026-09-07 §4.3). A runway's
pavement is the road surface with two differences: no yellow centreline, and a colour per
surface. Both are parameters of the parent, so an instance says all there is to say and a
designer can retune either in the details panel without a script. Reinforced has no
material: it LOOKS like concrete, the difference is a rating (spec §8).

CentrelineWidth = 0 is what removes the yellow line (§4.2). THIS COMMENT USED TO BE WRONG
AND THE BUG IT DESCRIBED SHIPPED. It read: "with the width at zero only lateral == 0 exactly
is painted - a line of zero width". Painted at lateral == 0 is not a line of zero width, it
is a line one ramp wide: the old mask was 1 - saturate((|lateral| - CentrelineWidth) *
MarkingSharpness), which is 1 at lateral == 0 whatever the width, so every runway carried a
faint 4 cm yellow hairline down its centre. build_road_material.py now computes
saturate((CentrelineWidth - |lateral|) * MarkingSharpness) instead, where a zero width
really does paint nothing. Re-run that script before this one or the hairline comes back.

The white markings are a separate mesh (FRunwayMarkingBuilder) and do not go through this
mask - they are painted through an instance of the PARENT, whose CentrelineWidth is still
the default, which is why they are unaffected by any of the above.
"""
import os
import sys
import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airside_palette as palette

MAT_DIR = "/Game/Materials"
PARENT = "%s/M_RoadSurface" % MAT_DIR
CONTENT = "/Game/DA_AirsideContent"

# name -> (palette colour, content property).
#
# COLOURS, NOT TINTS, since 2026-09-19. These used to be multipliers on a photoreal asphalt
# albedo - 1.0 left it alone, 1.9 lifted it to concrete, 0.55/0.80/0.35 pushed it toward
# grass. build_road_material.py dropped the photo maps, so there is nothing left to
# multiply and each surface states its colour outright. That also makes them diffable
# against the palette table in spec 1.1, which a multiplier never was.
#
# Runway asphalt is DARKER than the taxiway asphalt M_RoadSurface defaults to. That is real
# - the two are laid and maintained differently - and it is functional from the build
# camera, saying which surface an aircraft is on before any marking is legible.
#
# The grass strip is the mown-grass colour, deliberately close to the field around it. A
# grass runway IS just mown field; what makes it read as a runway is the white edge markers
# and corner squares FRunwayMarkingBuilder paints along it (GrassMarker, GrassCorner), not
# a colour that says "pavement". The old green-brown was a placeholder chosen to read as
# "not pavement" over an asphalt texture that no longer exists.
# EACH INSTANCE SETS ITS WEAR TONE TOO, and not doing so was a real defect rather than an
# omission. M_RoadSurface's WearColour default is #5D6569, a cool grey chosen for asphalt,
# and an instance that overrode only BaseColour inherited it. So a GRASS runway lerped
# between olive green and asphalt grey: on screen it read as a muddy, camouflaged strip,
# nothing like mown turf. A parameter default can only ever suit one instance; every
# instance that has a different base needs its own.
#
# Each wear tone is its base at about 1.09x luminance in the SAME hue family - the ratio
# settled after the pavement's first wear tones read as wet patches. See build_road_material.
INSTANCES = [
    ("M_RunwayTarmac",   palette.RUNWAY_ASPHALT, "#51585D", "runway_tarmac_material"),
    ("M_RunwayConcrete", palette.APRON_CONCRETE, "#949389", "runway_concrete_material"),
    # NOT the sampled olive #7D8E47 the ground layers use. That is a terrain colour at hue
    # 74 degrees, and the FIELD around this strip is currently drawn in the 2026-09-19
    # greens at hue 97 - so an olive strip on a green field reads as a different material
    # rather than as mown grass. A grass runway is mown field, so its colour has to be a
    # near relative of whatever the field is wearing: this is ToneA lifted slightly, the way
    # cut grass is lighter than the rough around it.
    #
    # It therefore MOVES IF THE FIELD TONES MOVE. They are on trial (spec 1.1), and if the
    # sampled olives win, this becomes an olive too.
    ("M_RunwayGrass",    "#7BA363",              "#80A967", "runway_grass_material"),
]


def build_instance(name, hexcode, wear_hex, parent):
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
    lib.set_material_instance_vector_parameter_value(
        instance, "BaseColour", palette.linear_color(hexcode))
    lib.set_material_instance_vector_parameter_value(
        instance, "WearColour", palette.linear_color(wear_hex))
    lib.update_material_instance(instance)
    unreal.EditorAssetLibrary.save_asset(path)

    # Read back, not trusted: a parameter name the parent does not declare is accepted
    # silently and does nothing, which is how a tint ends up "set" on a material that
    # never changes colour.
    width = lib.get_material_instance_scalar_parameter_value(instance, "CentrelineWidth")
    colour = lib.get_material_instance_vector_parameter_value(instance, "BaseColour")
    wear = lib.get_material_instance_vector_parameter_value(instance, "WearColour")
    unreal.log("MARKER: %s parent=%s CentrelineWidth=%.1f BaseColour=%s (%.3f, %.3f, %.3f) WearColour=%s (%.3f, %.3f, %.3f)"
               % (path, parent.get_name(), width, hexcode, colour.r, colour.g, colour.b,
                  wear_hex, wear.r, wear.g, wear.b))
    return instance


def point_content_at(instances):
    content = unreal.EditorAssetLibrary.load_asset(CONTENT)
    if content is None:
        unreal.log_error("MARKER: %s not found - content set not updated" % CONTENT)
        return
    for (name, _hexcode, _wear, prop), instance in zip(INSTANCES, instances):
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
    missing = [n for n in ("BaseColour", "WearColour") if n not in names]
    if missing:
        unreal.log_error("MARKER: %s declares no %s - rerun build_road_material.py"
                         % (PARENT, ", ".join(missing)))
    else:
        built = [build_instance(name, hexcode, wear, parent)
                 for name, hexcode, wear, _prop in INSTANCES]
        point_content_at(built)
        unreal.log("MARKER: done")
