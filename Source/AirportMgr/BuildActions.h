#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

class ARoadBuildController;

/** Where an action sits on the bar. Bar order is enum order. */
enum class EActionSection : uint8
{
	Time,
	Tools,
	Edit,
	Aircraft,
	Selection,
	Game
};

const TCHAR* ActionSectionName(EActionSection Section);

/**
 * One thing the player can do from the bar or a key.
 *
 * Execute, IsActive and IsEnabled take the controller rather than capturing it: the table
 * is a function-local static built once per process, and a captured controller would be
 * the first PIE session's, dangling in the second.
 */
struct FBuildAction
{
	FName Id;
	EActionSection Section = EActionSection::Tools;
	FText Label;
	/** EKeys::Invalid for a bar-only action. */
	FKey Key;
	bool bRequiresCtrl = false;
	TFunction<void(ARoadBuildController&)> Execute;
	/** Lit on the bar: the active tool, paused, overlay on, a sticky modifier. */
	TFunction<bool(const ARoadBuildController&)> IsActive;
	/** Greyed when false: undo with nothing to undo, land with no runway. */
	TFunction<bool(const ARoadBuildController&)> IsEnabled;
};

/**
 * THE list. Key bindings (ARoadBuildController::SetupInputComponent), the startup banner and
 * the bar's buttons (UBuildBarWidget) are all generated from it, so the three cannot
 * disagree - see CLAUDE.md "Lists that must agree are ONE list", and the three times this
 * project shipped a key that went nowhere.
 *
 * The Tools section is generated from Airside's ToolRegistry() so that table stays the one
 * source for tools. A function-local static for the same reason ToolRegistry() is.
 */
TConstArrayView<FBuildAction> BuildActions();
