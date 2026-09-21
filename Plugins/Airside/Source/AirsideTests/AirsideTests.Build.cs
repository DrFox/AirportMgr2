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
			"GeometryCore",       // FDynamicMesh3, to test what the sink actually accepts
			"GeometryFramework",  // UDynamicMeshComponent - Airside's own dependency on it is
			                      // now Private (issue #191); tests that construct one need it named here
			"InputCore",          // EKeys, to assert WHICH key a registry entry claims
			"Airside"
		});
	}
}
