using UnrealBuildTool;

public class RoadNetEditor : ModuleRules
{
	public RoadNetEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// RoadNet is the only thing this needs from the project: the tools, the facade and
		// the model. The dependency runs this way ONLY - the runtime plugin must never
		// depend on an editor module, or it cannot ship.
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RoadNet"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"EditorFramework",
			"UnrealEd",
			"LevelEditor",

			// UEdMode has NO raw input hooks - no InputKey, no MouseMove, no Render. It
			// routes viewport input through the InteractiveTools input router, so these are
			// not optional for an editor mode that wants clicks.
			"InteractiveToolsFramework",
			"EditorInteractiveToolsFramework"
		});
	}
}
