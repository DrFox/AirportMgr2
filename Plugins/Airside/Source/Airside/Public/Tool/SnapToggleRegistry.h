#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

struct FSnapGuideSettings;

/**
 * Which row of the guide grid a toggle belongs to - the bar's two sections, and the editor
 * palette's order. See the 2026-09-20 guide-grid design section 7: what a guide MEANS (a
 * relation, plus the world grid, which is a way of aligning) against what it is measured
 * AGAINST (a reference).
 */
enum class ESnapToggleGroup : uint8
{
	AlignBy,
	SnapTo
};

/**
 * One switch over an airport's FSnapGuideSettings: a guide relation, a guide reference, the grid
 * step or the grid's orientation. BuildVerbRegistry()'s shape (Key/Id/Name/Tooltip plus what it
 * does), applied to the settings rather than to the session.
 *
 * ISSUE #440: these were fourteen hand-written BuildActions rows in the GAME module, run through
 * eight ARoadBuildController proxies - so the editor mode, which cannot depend on the game module,
 * reached FSnapGuideSettings only through the Details panel, and H (grid orientation) existed in
 * PIE alone. This table lives in Airside, where both drivers can read it: BuildActions.cpp builds
 * one bar row per entry, and FRoadBuildEdModeCommands::RegisterCommands one palette command per
 * entry, so a fifteenth toggle added here reaches both or neither.
 * ENFORCED BY: AirportMgr.Actions.SnapRowsComeFromTheRegistry (the bar),
 * Airside.Editor.SnapCommandsReachTheAirport (the palette, through the toolkit's own command list)
 *
 * OVER THE SETTINGS, NOT OVER A DRIVER: the settings live on the airport (ARoadNetworkActor::
 * GuideSources - see FSnapGuideSettings for why a per-driver copy is the failure it records), so
 * Apply takes the settings each driver resolved from its own target. Logging and undo are the
 * DRIVER's - a PIE toggle is not an editor transaction - so neither is in here.
 */
struct FSnapToggleRegistration
{
	/**
	 * THE ONE ID, verbatim in both drivers: the bar row's FBuildAction::Id and the editor
	 * command's name. The dotted form ("snap.extending", "snapto.runway") is deliberate, not a
	 * BuildActions-only spelling: a bare "Runway" would collide with the runway TOOL's command in
	 * the one editor binding context (FUICommandInfo names are unique per context and a second one
	 * is an ensure), and the prefix is also what keeps a reference apart from a tool of the same
	 * word. Stable and unlocalised, FToolRegistration::Id's reason: it is persisted to the
	 * editor's per-user keybindings ini.
	 */
	FName Id;

	ESnapToggleGroup Group = ESnapToggleGroup::AlignBy;

	FText Name;
	FText Tooltip;

	/** EKeys::Invalid for a button-only toggle - every one but the grid's orientation (H). */
	FKey Key;

	/** Flips (or, for the grid step, cycles) this toggle on an airport's settings. */
	TFunction<void(FSnapGuideSettings&)> Apply;

	/** Lit on the bar, checked on the palette. */
	TFunction<bool(const FSnapGuideSettings&)> IsActive;

	/** A caption that follows state - "Grid: 5 m" - or unset for the fixed Name. */
	TFunction<FText(const FSnapGuideSettings&)> DynamicLabel;
};

/** Built once, on first use, never mutated after - ToolRegistry()'s own reason. */
AIRSIDE_API TConstArrayView<FSnapToggleRegistration> SnapToggleRegistry();

/**
 * What a driver logs after a toggle: its DynamicLabel when it has one, "<Name>: on/off" when
 * not. One wording for both drivers' log lines, so a diagnosis grep finds either.
 */
AIRSIDE_API FString DescribeSnapToggle(const FSnapToggleRegistration& Toggle, const FSnapGuideSettings& Settings);
