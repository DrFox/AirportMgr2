import unreal

p = "/Game/RoadNet/Materials/M_RoadSurface"
m = unreal.load_asset(p)
if m is None:
    unreal.log_error("MARKER: %s does not load" % p)
else:
    exprs = unreal.MaterialEditingLibrary.get_material_expressions(m) \
        if hasattr(unreal.MaterialEditingLibrary, "get_material_expressions") else []
    unreal.log("MARKER: expression count = %d" % len(exprs))
    for e in exprs:
        kind = type(e).__name__
        if kind == "MaterialExpressionTextureSample":
            tex = e.get_editor_property("texture")
            unreal.log("MARKER: TextureSample texture=%s samplertype=%s"
                       % (tex.get_name() if tex else "NULL",
                          e.get_editor_property("sampler_type")))
        elif kind == "MaterialExpressionTextureCoordinate":
            unreal.log("MARKER: TexCoord index=%s" % e.get_editor_property("coordinate_index"))
        elif kind in ("MaterialExpressionScalarParameter", "MaterialExpressionVectorParameter"):
            unreal.log("MARKER: %s %s = %s" % (kind, e.get_editor_property("parameter_name"),
                                               e.get_editor_property("default_value")))

    lib = unreal.MaterialEditingLibrary
    for name, prop in (("BaseColor", unreal.MaterialProperty.MP_BASE_COLOR),
                       ("Normal", unreal.MaterialProperty.MP_NORMAL),
                       ("Roughness", unreal.MaterialProperty.MP_ROUGHNESS),
                       ("AO", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)):
        node = lib.get_material_property_input_node(m, prop)
        unreal.log("MARKER: %s <- %s" % (name, type(node).__name__ if node else "NOTHING"))

for t in ("T_Asphalt_Albedo", "T_Asphalt_Normal", "T_Asphalt_Roughness", "T_Asphalt_AO"):
    a = unreal.load_asset("/Game/RoadNet/Textures/%s" % t)
    unreal.log("MARKER: texture %s -> %s" % (t, "OK" if a else "MISSING"))
