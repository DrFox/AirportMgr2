using UnrealBuildTool;

public class RoadNetTests : ModuleRules
{
	public RoadNetTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RoadNet"
		});
	}
}
