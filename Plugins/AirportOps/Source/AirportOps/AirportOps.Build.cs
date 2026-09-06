using UnrealBuildTool;

public class AirportOps : ModuleRules
{
	public AirportOps(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Airside is a PUBLIC dependency: this module's headers name Airside types
		// (EAgentPhase, EArrivalRefusal, URoadNetwork) in their own signatures, so anything
		// that includes AirportOps must be able to see Airside too. The direction is one-way
		// by rule - Check-Architecture.ps1 fails the build if Airside ever includes us.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",  // UAirportOpsSettings
			"Airside"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
