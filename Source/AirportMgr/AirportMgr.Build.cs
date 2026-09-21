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
		// DeveloperSettings: UAirportMgrUISettings, so the UI style asset is CONFIGURED and
		// not coded - the same reason UAirsideSettings and UAirportOpsSettings take it.
		//
		// NO EnhancedInput (issue #191 dropped it): input here is legacy InputComponent->
		// BindKey plus IsInputKeyDown polls (ARoadBuildController::SetupInputComponent and
		// UpdateView) - nothing in this module ever called an EnhancedInput C++ type, so the
		// dependency bought nothing. DefaultInput.ini's EnhancedPlayerInput/
		// EnhancedInputComponent defaults are a project-wide, plugin-level setting, unaffected
		// by this module's own dependency list.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "DeveloperSettings", "Airside", "AirportOps" });

		// UMG: the build bar. Slate/SlateCore: FInputChord (the registry's Ctrl bindings) and
		// the UMG types' bases.
		//
		// NO ModelViewViewModel (issue #191 dropped it): this used to say "the offer inbox
		// binds viewmodels rather than polling the board", but no Content/UI Blueprint ever
		// bound a field, so every UE_MVVM_SET_PROPERTY_VALUE broadcast a change to zero
		// subscribers. OfferInboxWidget and LedgerPanelWidget poll on NativeTick and read
		// plain getters (see their own headers) - the actual, working design, and the one
		// PR #196 already made cheap by gating on a revision instead of every tick. The
		// viewmodels are now plain UObject DTOs; see OfferViewModels.h and LedgerViewModels.h.
		//
		// AssetRegistry: AnimYardCatalogue scans for every UAircraftType in the project, so the
		// model yard's bench needs no list of its own to keep in step with the fleet.
		PrivateDependencyModuleNames.AddRange(new string[] { "UMG", "Slate", "SlateCore", "AssetRegistry" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
