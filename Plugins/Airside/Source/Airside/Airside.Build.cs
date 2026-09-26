using UnrealBuildTool;

public class Airside : ModuleRules
{
	public Airside(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",       // FDynamicMesh3
			"DeveloperSettings",  // UAirsideSettings, so the content set is configured not coded
			"InputCore"           // FKey, for FToolRegistration - the one thing the shared tool table needs
		});

		// GeometryFramework (UDynamicMeshComponent) is PRIVATE: every Public header that
		// names it holds a TObjectPtr behind a forward declaration (RoadNetworkActor.h,
		// DynamicMeshSink.h, RoadJunctionGallery.h, RoadRebuildCensus.h) - no Public header
		// needs the full type, so no consumer of Airside needs this dependency (issue #191).
		//
		// AssetRegistry is PRIVATE for the same shape of reason, needed by two independent
		// callers now: EveryAircraftType()'s scan (issue #293, Testing/AirsideTestWorld.cpp)
		// and AirsideSettings.cpp's ResolveLetterEnvelopeTable (#292) - neither is named by a
		// Public header (EveryAircraftType's own TArray<UAircraftType*> return type hides it,
		// and no Public header names FAssetData or IAssetRegistry), so no consumer of Airside
		// needs the module, only these two functions.
		PrivateDependencyModuleNames.AddRange(new string[] { "GeometryFramework", "AssetRegistry" });
	}
}
