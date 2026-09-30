#pragma once

#include "CoreMinimal.h"
#include "Solve/GuideArbiter.h"
#include "InputCoreTypes.h"
#include "UI/UiMenuButton.h"

class ARoadBuildController;
class ARoadNetworkActor;
class UOpsRuntime;

/**
 * What an FBuildAction's Execute/IsActive/IsEnabled actually needs, rather than the bare
 * ARoadBuildController& every one of them used to take.
 *
 * ISSUE #191: a lambda typed TFunction<void(ARoadBuildController&)> forces every verb to
 * become a controller method, whether or not the controller is who actually does the work -
 * StepLandingFee owning a pricing rule was one symptom of exactly this. Runtime and Target
 * are resolved ONCE, here, rather than by every verb that wants them (most already did their
 * own UOpsRuntimeSubsystem::Get(GetWorld()) call) - so a verb whose real owner is UOpsRuntime,
 * or a future one whose owner is neither, can bind straight to it instead of going through a
 * controller forwarder that exists only to be that owner's proxy.
 *
 * A CONVERTING CONSTRUCTOR FROM ARoadBuildController&, not an explicit factory function: every
 * external caller of TryRun/IsActive/IsEnabled (BuildBarWidget, InspectorWidget, the controller
 * itself, and the tests) already holds a controller reference, and building the context is the
 * SAME lookup TryRun used to do inline - moving it here costs those callers nothing to keep
 * calling the way they always have.
 */
struct FBuildActionContext
{
	ARoadBuildController& Controller;
	/** The attached OpsRuntime, or null in a world with no game instance (a headless test). */
	UOpsRuntime* Runtime = nullptr;
	/** The road actor being built into, or null when the level has none. */
	ARoadNetworkActor* Target = nullptr;

	/**
	 * EXPLICIT (issue #309, reversing #191's own ruling): the implicit conversion that ruling
	 * asked for is exactly what let UBuildBarWidget::RefreshState build one FRESH context per
	 * IsEnabled/IsActive call - `Action.IsEnabled(*C)` converts silently, invisibly, at every
	 * one of ~35 actions' two calls, ~70 GetSubsystem lookups a tick (BuildActions.cpp:11-19)
	 * for a context BuildActions.h itself said should be "resolved ONCE". Explicit forces every
	 * call site that used to convert silently to say so, which is what turned up RefreshState
	 * and PlotReadoutBarTest.cpp's Build->IsEnabled(*C) as the only two - see their own fixes.
	 * TryRun already constructed one by name (FBuildActionContext Context(C);) so it needed no
	 * change.
	 */
	explicit FBuildActionContext(ARoadBuildController& InController);

	/**
	 * How many contexts have been constructed since process start - the same idiom
	 * UAirportMgrUISettings::ResolveCallCountForTest and FBuildSession::MakeContextCallCountForTest
	 * use, and read the same way: a DELTA across N ticks, never an absolute count, since other
	 * tests in the same run construct contexts too.
	 */
	static int32 ConstructCountForTest() { return ConstructCalls; }

private:
	static int32 ConstructCalls;
};

/** Where an action sits on the bar. Bar order is enum order. Count is a sentinel, never a
 *  section: it lets a table or a loop size itself off the enum instead of retyping "Game"
 *  as the last one, the way BuildBarWidgetTest and BuildActionsTest both used to. */
enum class EActionSection : uint8
{
	Time,
	Tools,
	Edit,
	Aircraft,
	Selection,
	Game,

	/** The guide RELATIONS - what a guide means. See the 2026-09-20 guide-grid design §7. */
	Snap,

	/** The guide REFERENCES - what it is measured against. The second axis, same design §7. */
	SnapTo,

	Count
};

const TCHAR* ActionSectionName(EActionSection Section);

/**
 * One thing the player can do from the bar or a key.
 *
 * Execute, IsActive and IsEnabled take an FBuildActionContext rather than capturing anything:
 * the table is a function-local static built once per process, and a captured controller (or
 * runtime) would be the first PIE session's, dangling in the second. See FBuildActionContext's
 * own comment (issue #191) for why that context is a struct now rather than a bare
 * ARoadBuildController& - it is what lets a verb bind to UOpsRuntime directly instead of
 * forcing a controller method to exist purely to reach it.
 */
struct FBuildAction
{
	FName Id;
	EActionSection Section = EActionSection::Tools;
	FText Label;
	/** EKeys::Invalid for a bar-only action. */
	FKey Key;
	bool bRequiresCtrl = false;
	TFunction<void(FBuildActionContext&)> Execute;
	/** Lit on the bar: the active tool, paused, overlay on, a sticky modifier. */
	TFunction<bool(const FBuildActionContext&)> IsActive;
	/** Greyed when false: undo with nothing to undo, land with no runway. */
	TFunction<bool(const FBuildActionContext&)> IsEnabled;

	/**
	 * A caption that follows state - "Grid: 5 m" - or unset for the fixed Label. Read by the bar
	 * every tick beside IsActive. Label stays the fixed name (tooltips, the log, the tests' id
	 * lookups); this only changes what the button reads.
	 */
	TFunction<FText(const FBuildActionContext&)> DynamicLabel;

	/**
	 * Run from the inspector's card only - no bar button, so no icon (facility-upgrades spec §4: "nothing on
	 * the bottom bar: acting on a selection belongs to the inspector"). Still a row, so the card's clicks go
	 * through TryRun's one door and its log line.
	 * ENFORCED BY: AirportMgr.Actions.FacilityVerbsRegistered, AirportMgr.Actions.BarBuildsFromRegistry
	 */
	bool bInspectorOnly = false;

	/**
	 * A MENU VERB: set, and the bar builds this action as a UUiMenuButton whose popup lists these lines, asked for as it
	 * opens - the inspector Unstick's shape, and its CONFIRM (FUiMenuItem::bConfirm): a destructive line is armed by the
	 * first click and chosen by the second, at the button the player just clicked, and closing the popup disarms it.
	 * Choose runs the chosen line, through TryChoose. Execute stays the keyless, non-destructive door (TryRun).
	 *
	 * THE OTHER MENU-VERB SHAPE is selection.unstick's: its popup lives in the INSPECTOR beside the card it acts on, so
	 * the bar row's Execute only asks for it (ARoadBuildController::RequestUnstickMenu) and sets no MenuItems. Use
	 * MenuItems when the popup belongs at the bar button; the request shape when it belongs to another panel.
	 * ENFORCED BY: AirportMgr.Actions.BarBuildsMenuActionsAsMenus, AirportMgr.Actions.AirportCloseConfirms
	 */
	TFunction<TArray<FUiMenuItem>(const FBuildActionContext&)> MenuItems;
	TFunction<void(FBuildActionContext&, int32)> Choose;

	/**
	 * The ONE door: checks IsEnabled, logs "<Via>: <Id>" on LogRoadBuild, then Execute - so
	 * the bar, the inspector and every key press leave the same one line when they actually
	 * fire, and none of them can fire a disabled action by forgetting the check. Returns
	 * whether it ran.
	 *
	 * STILL TAKES ARoadBuildController&, not FBuildActionContext&: every external caller
	 * (BuildBarWidget, InspectorWidget, the controller's own key handlers) holds a controller
	 * reference and nothing else, and building the context from it is exactly the one lookup
	 * this method already did inline before issue #191 gave that lookup a name.
	 */
	bool TryRun(ARoadBuildController& C, const TCHAR* Via) const;

	/** TryRun's door for a menu verb's chosen Line: the same IsEnabled gate and the same one log line
	 *  ("<Via>: <Id> line <n>"), then Choose. False when disabled or not a menu verb. */
	bool TryChoose(ARoadBuildController& C, int32 Line, const TCHAR* Via) const;
	/** The same, given a context already built - the bar's (and a test's, with a runtime handed in). */
	bool TryChoose(FBuildActionContext& Context, int32 Line, const TCHAR* Via) const;
};

/** Linear scan: BuildActions() is a few dozen entries, not a hot loop. */
const FBuildAction* FindAction(FName Id);

/** game.settings - named once, for the registry entry and for the controller's modal key gate,
 *  which lets this one action through (ARoadBuildController::RunActionForKey). */
inline FName SettingsActionId() { return FName(TEXT("game.settings")); }
/** Ctrl state disambiguates two actions sharing a key; none do today, but the table allows it. */
const FBuildAction* FindAction(FKey Key, bool bRequiresCtrl);

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
