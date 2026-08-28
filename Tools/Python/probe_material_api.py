import unreal
lib = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
print("PROBE: MaterialEditingLibrary =", lib is not None)
print("PROBE: create_material_expression =", hasattr(lib, "create_material_expression"))
print("PROBE: connect_material_property =", hasattr(lib, "connect_material_property"))
print("PROBE: connect_material_expressions =", hasattr(lib, "connect_material_expressions"))
print("PROBE: AssetImportTask =", hasattr(unreal, "AssetImportTask"))
print("PROBE: MaterialFactoryNew =", hasattr(unreal, "MaterialFactoryNew"))
print("PROBE: asset_tools =", tools is not None)
