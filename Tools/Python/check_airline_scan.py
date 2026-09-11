"""One-shot check: does the asset manager actually SCAN AirlineDefinition?

UOpsDefinition::GetPrimaryAssetId derives the type from the class name minus its prefix, so
a missing or misspelt PrimaryAssetTypesToScan line in DefaultGame.ini means the catalog
loads nothing and reports no airlines - with no error anywhere. This is the only cheap way
to tell the two cases apart.
"""
import unreal

manager = unreal.AssetManager.get()
ids = manager.get_primary_asset_id_list(unreal.Name("AirlineDefinition"))
unreal.log("MARKER: asset manager scans {} AirlineDefinition(s)".format(len(ids)))
for i in ids:
    unreal.log("MARKER:   {}".format(i))
