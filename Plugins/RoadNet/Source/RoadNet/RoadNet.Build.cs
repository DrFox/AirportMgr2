using UnrealBuildTool;

public class RoadNet : ModuleRules
{
	public RoadNet(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",       // FDynamicMesh3
			"GeometryFramework"   // UDynamicMeshComponent
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
