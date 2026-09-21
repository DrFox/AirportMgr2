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
		PrivateDependencyModuleNames.AddRange(new string[] { "GeometryFramework" });
	}
}
