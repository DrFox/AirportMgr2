using UnrealBuildTool;

public class AirsideTests : ModuleRules
{
	public AirsideTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GeometryCore",   // FDynamicMesh3, to test what the sink actually accepts
			"Airside"
		});
	}
}
