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
			"GeometryFramework",  // UDynamicMeshComponent
			"DeveloperSettings"   // UAirsideSettings, so the content set is configured not coded
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
