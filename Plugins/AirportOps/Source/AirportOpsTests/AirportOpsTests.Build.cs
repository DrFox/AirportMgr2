using UnrealBuildTool;

public class AirportOpsTests : ModuleRules
{
	public AirportOpsTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Airside",
			"AirportOps"
		});
	}
}
