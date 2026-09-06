// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class AirportMgr : ModuleRules
{
	public AirportMgr(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		// Airside: the game module drives the road facade. AirportOps: the game module drives
		// the sim clock and save/load. Both dependencies run this way only - neither plugin
		// ever depends on the game, and AirportOps depends on Airside, never the reverse.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Airside", "AirportOps" });

		// UMG: the build bar. Slate/SlateCore: FInputChord (the registry's Ctrl bindings) and
		// the UMG types' bases.
		PrivateDependencyModuleNames.AddRange(new string[] { "UMG", "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
