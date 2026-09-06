# Build Bar HUD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A horizontal, sectioned bottom bar (UMG) whose buttons are generated from one action registry that also drives the key bindings and the startup banner, so every key is a click and the bar can never show a control that goes nowhere.

**Architecture:** `BuildActions()` in the game module is the single list: tools generated from Airside's `ToolRegistry()`, plus the clock, edit, aircraft and game actions. `ARoadBuildController` binds keys from it and exposes the state queries it needs. `UBuildBarWidget` (C++ `UUserWidget`) fills five optionally-bound section panels with one button per action and polls state each frame; a Python-authored Blueprint widget supplies the chrome.

**Tech Stack:** UE 5.8.2 C++, UMG (`UMG`, `Slate`, `SlateCore` modules), Python editor scripting for the asset, automation tests via `Run-AirsideTests.ps1`.

**Spec:** `docs/superpowers/specs/2026-09-06-build-bar-hud-design.md`

## Global Constraints

- The bar's buttons, the key bindings and the banner all derive from `BuildActions()`. No hand-listed action anywhere else.
- Sections in order: Time, Tools, Edit, Aircraft, Game. Horizontal bar along the bottom; never a vertical toolbar.
- Click modifiers are one enum `EClickModifier { None, Remove, Insert }`, never two bools.
- A missing Blueprint asset or missing slot degrades to a code-built panel; nothing may fail because content is absent.
- Camera keys (WASD, Q/E, mouse wheel, mouse buttons) are not actions and stay hand-bound.
- `UE_LOG` count in the game module does not fall. Comments explain WHY.
- **Editor closed for every build.** Build line:
  ```
  D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\AirportMgr2\AirportMgr.uproject" -WaitMutex
  ```
- Tests: `./Tools/Run-AirsideTests.ps1` (default filter widens to `Airside+AirportOps+AirportMgr` in Task 1). Read the `N test(s) run, N failed, N crashed` line.
- Branch: `feature/build-bar` (exists, holds the spec).

---

## File map

| File | Responsibility |
|---|---|
| `Source/AirportMgr/BuildActions.h/.cpp` | `EActionSection`, `FBuildAction`, `BuildActions()` |
| `Source/AirportMgr/BuildActionsTest.cpp` | Registry completeness test |
| `Source/AirportMgr/RoadBuildController.h/.cpp` | `EClickModifier`, `SelectTool(int32)`, queries, registry-driven bindings and banner, `LandAircraftNearViewFocus`, input mode, bar creation |
| `Source/AirportMgr/ClickModifierTest.cpp` | Modifier state test |
| `Source/AirportMgr/BuildBarWidget.h/.cpp` | `UBuildBarEntry`, `UBuildBarWidget` |
| `Source/AirportMgr/BuildBarWidgetTest.cpp` | Bar-builds-from-registry test |
| `Source/AirportMgr/RoadBuildHUD.cpp` | Tool-name and clock lines removed |
| `Source/AirportMgr/AirportMgr.Build.cs` | `UMG`, `Slate`, `SlateCore` |
| `Tools/Run-AirsideTests.ps1` | Filter `Airside+AirportOps+AirportMgr` |
| `Tools/Python/build_bar_widget.py` | Creates `/Game/UI/WBP_BuildBar` |
| `Config/DefaultGame.ini` | `BuildBarClass` default |

---

### Task 1: The action registry

**Files:**
- Create: `Source/AirportMgr/BuildActions.h`, `Source/AirportMgr/BuildActions.cpp`
- Create: `Source/AirportMgr/BuildActionsTest.cpp`
- Modify: `Tools/Run-AirsideTests.ps1:20`

**Interfaces:**
- Produces:
```cpp
enum class EActionSection : uint8 { Time, Tools, Edit, Aircraft, Game };
struct FBuildAction {
	FName Id; EActionSection Section; FText Label; FKey Key; bool bRequiresCtrl = false;
	TFunction<void(ARoadBuildController&)> Execute;
	TFunction<bool(const ARoadBuildController&)> IsActive;
	TFunction<bool(const ARoadBuildController&)> IsEnabled;
};
TConstArrayView<FBuildAction> BuildActions();
const TCHAR* ActionSectionName(EActionSection Section);
```
- Consumes from Task 2 (declared here, defined there): `ARoadBuildController::SelectTool(int32)`, `GetActiveToolIndex()`, `ToggleClickModifier(EClickModifier)`, `GetClickModifier()`, `IsWatchingAgent()`, `IsGuidelineOverlayOn()`, `CanUndo()`, `CanRedo()`, `HasNetworkContent()`, `HasRunway()`, `HasAgent()`, `HasOpsRuntime()`, `StepSpeed(int32)`, `TogglePause()`, `QuickSave()`, `QuickLoad()`, `LandAircraftNearViewFocus()`, `ToggleWatchAgent()`, `OnToggleGuidelines()`, `OnUndo()`, `OnRedo()`, `OnClearNetwork()`. Task 1 compiles only after Task 2's header lands, so **Tasks 1 and 2 build together**; Task 1's test is what fails first.

- [ ] **Step 1: Write the failing test**

`Source/AirportMgr/BuildActionsTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "BuildActions.h"
#include "Misc/AutomationTest.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildActionsRegistryTest,
	"AirportMgr.Actions.RegistryIsComplete",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildActionsRegistryTest::RunTest(const FString& Parameters)
{
	// THE ONE-LIST CHECK. Keys, banner and bar all read this table, so a defect here is a
	// key that goes nowhere or a button with no handler - the bug this project has shipped
	// three times through lists that were supposed to agree.
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	TestTrue(TEXT("the registry is not empty"), Actions.Num() > 0);

	TSet<FName> Ids;
	TSet<FString> Chords;
	for (const FBuildAction& A : Actions)
	{
		TestFalse(*FString::Printf(TEXT("%s has an id"), *A.Id.ToString()), A.Id.IsNone());
		TestFalse(*FString::Printf(TEXT("%s has a label"), *A.Id.ToString()), A.Label.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s has an executor"), *A.Id.ToString()), static_cast<bool>(A.Execute));
		TestTrue(*FString::Printf(TEXT("%s has an IsActive query"), *A.Id.ToString()), static_cast<bool>(A.IsActive));
		TestTrue(*FString::Printf(TEXT("%s has an IsEnabled query"), *A.Id.ToString()), static_cast<bool>(A.IsEnabled));
		TestFalse(*FString::Printf(TEXT("%s id is unique"), *A.Id.ToString()), Ids.Contains(A.Id));
		Ids.Add(A.Id);
		if (A.Key.IsValid())
		{
			const FString Chord = FString::Printf(TEXT("%s%s"), A.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""), *A.Key.ToString());
			TestFalse(*FString::Printf(TEXT("%s key %s is unique"), *A.Id.ToString(), *Chord), Chords.Contains(Chord));
			Chords.Add(Chord);
		}
	}

	// Every registered tool is an action exactly once, in the Tools section, with its key.
	for (const FToolRegistration& Tool : ToolRegistry())
	{
		int32 Found = 0;
		for (const FBuildAction& A : Actions)
		{
			if (A.Section == EActionSection::Tools && A.Key == Tool.Key) { ++Found; }
		}
		TestEqual(*FString::Printf(TEXT("tool %s appears once as an action"), *Tool.Name.ToString()), Found, 1);
	}

	// Every section has at least one action - an empty section on the bar is a layout with
	// nothing in it, which reads as a bug.
	for (uint8 S = 0; S <= static_cast<uint8>(EActionSection::Game); ++S)
	{
		const EActionSection Section = static_cast<EActionSection>(S);
		TestTrue(*FString::Printf(TEXT("section %s has actions"), ActionSectionName(Section)),
			Actions.ContainsByPredicate([Section](const FBuildAction& A) { return A.Section == Section; }));
	}
	return true;
}

#endif
```

- [ ] **Step 2: Widen the test runner**

`Tools/Run-AirsideTests.ps1:20`: `[string] $Filter  = 'Airside+AirportOps+AirportMgr',` and add `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.Actions` to the examples. Also update the comment above it: "All three modules by default."

- [ ] **Step 3: Write the header**

`Source/AirportMgr/BuildActions.h`:
```cpp
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
```

- [ ] **Step 4: Write the registry**

`Source/AirportMgr/BuildActions.cpp`:
```cpp
#include "BuildActions.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"

#define LOCTEXT_NAMESPACE "AirportMgr"

const TCHAR* ActionSectionName(EActionSection Section)
{
	switch (Section)
	{
	case EActionSection::Time:     return TEXT("Time");
	case EActionSection::Tools:    return TEXT("Tools");
	case EActionSection::Edit:     return TEXT("Edit");
	case EActionSection::Aircraft: return TEXT("Aircraft");
	case EActionSection::Game:     return TEXT("Game");
	}
	return TEXT("?");
}

namespace
{
	auto Always = [](const ARoadBuildController&) { return true; };
	auto Never = [](const ARoadBuildController&) { return false; };
	auto HasRuntime = [](const ARoadBuildController& C) { return C.HasOpsRuntime(); };

	TArray<FBuildAction> MakeActions()
	{
		TArray<FBuildAction> Out;

		// --- Time ---
		Out.Add({ TEXT("time.slower"), EActionSection::Time, LOCTEXT("Slower", "Slower"), EKeys::Comma, false,
			[](ARoadBuildController& C) { C.StepSpeed(-1); }, Never, HasRuntime });
		Out.Add({ TEXT("time.pause"), EActionSection::Time, LOCTEXT("Pause", "Pause"), EKeys::P, false,
			[](ARoadBuildController& C) { C.TogglePause(); },
			[](const ARoadBuildController& C) { return C.IsPaused(); }, HasRuntime });
		Out.Add({ TEXT("time.faster"), EActionSection::Time, LOCTEXT("Faster", "Faster"), EKeys::Period, false,
			[](ARoadBuildController& C) { C.StepSpeed(+1); }, Never, HasRuntime });

		// --- Tools: generated, never listed ---
		const TConstArrayView<FToolRegistration> Registry = ToolRegistry();
		for (int32 Index = 0; Index < Registry.Num(); ++Index)
		{
			const FToolRegistration& Tool = Registry[Index];
			Out.Add({ FName(*FString::Printf(TEXT("tool.%s"), *Tool.Name.ToString().ToLower())),
				EActionSection::Tools, Tool.Name, Tool.Key, false,
				[Index](ARoadBuildController& C) { C.SelectTool(Index); },
				[Index](const ARoadBuildController& C) { return C.GetActiveToolIndex() == Index; },
				Always });
		}

		// --- Edit ---
		Out.Add({ TEXT("edit.remove"), EActionSection::Edit, LOCTEXT("Remove", "Remove"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleClickModifier(EClickModifier::Remove); },
			[](const ARoadBuildController& C) { return C.GetClickModifier() == EClickModifier::Remove; }, Always });
		Out.Add({ TEXT("edit.insert"), EActionSection::Edit, LOCTEXT("Insert", "Insert"), EKeys::Invalid, false,
			[](ARoadBuildController& C) { C.ToggleClickModifier(EClickModifier::Insert); },
			[](const ARoadBuildController& C) { return C.GetClickModifier() == EClickModifier::Insert; }, Always });
		Out.Add({ TEXT("edit.undo"), EActionSection::Edit, LOCTEXT("Undo", "Undo"), EKeys::Z, true,
			[](ARoadBuildController& C) { C.OnUndo(); }, Never,
			[](const ARoadBuildController& C) { return C.CanUndo(); } });
		Out.Add({ TEXT("edit.redo"), EActionSection::Edit, LOCTEXT("Redo", "Redo"), EKeys::Y, true,
			[](ARoadBuildController& C) { C.OnRedo(); }, Never,
			[](const ARoadBuildController& C) { return C.CanRedo(); } });
		Out.Add({ TEXT("edit.clear"), EActionSection::Edit, LOCTEXT("Clear", "Clear"), EKeys::BackSpace, false,
			[](ARoadBuildController& C) { C.OnClearNetwork(); }, Never,
			[](const ARoadBuildController& C) { return C.HasNetworkContent(); } });

		// --- Aircraft ---
		Out.Add({ TEXT("aircraft.land"), EActionSection::Aircraft, LOCTEXT("Land", "Land"), EKeys::Seven, false,
			[](ARoadBuildController& C) { C.LandAircraftNearViewFocus(); }, Never,
			[](const ARoadBuildController& C) { return C.HasRunway(); } });
		Out.Add({ TEXT("aircraft.watch"), EActionSection::Aircraft, LOCTEXT("Watch", "Watch"), EKeys::C, false,
			[](ARoadBuildController& C) { C.ToggleWatchAgent(); },
			[](const ARoadBuildController& C) { return C.IsWatchingAgent(); },
			[](const ARoadBuildController& C) { return C.HasAgent() || C.IsWatchingAgent(); } });
		Out.Add({ TEXT("aircraft.guidelines"), EActionSection::Aircraft, LOCTEXT("Guidelines", "Guidelines"), EKeys::G, false,
			[](ARoadBuildController& C) { C.OnToggleGuidelines(); },
			[](const ARoadBuildController& C) { return C.IsGuidelineOverlayOn(); }, Always });

		// --- Game ---
		Out.Add({ TEXT("game.save"), EActionSection::Game, LOCTEXT("Save", "Save"), EKeys::K, false,
			[](ARoadBuildController& C) { C.QuickSave(); }, Never, HasRuntime });
		Out.Add({ TEXT("game.load"), EActionSection::Game, LOCTEXT("Load", "Load"), EKeys::L, false,
			[](ARoadBuildController& C) { C.QuickLoad(); }, Never, HasRuntime });
		return Out;
	}
}

TConstArrayView<FBuildAction> BuildActions()
{
	static const TArray<FBuildAction> Actions = MakeActions();
	return Actions;
}

#undef LOCTEXT_NAMESPACE
```
Note: the `OnLandAircraft` KEY keeps landing at the cursor (its existing behaviour) while the bar's Land button lands near the view focus, because a click on the bar leaves the cursor on the bar. Task 2 makes `OnLandAircraft` call `LandAircraftAt(Cursor)` and `LandAircraftNearViewFocus` call `LandAircraftAt(TargetView.Focus)`; the registry's `Execute` for `aircraft.land` is the focus variant, and the key bound from the registry therefore ALSO lands near the focus. That is a deliberate change: one action, one behaviour, and the focus is where the player is looking either way.

- [ ] **Step 5: Do not build yet**

Task 2 supplies the controller members this file calls. Continue to Task 2 and build there.

---

### Task 2: Controller — modifier enum, queries, registry-driven bindings

**Files:**
- Modify: `Source/AirportMgr/RoadBuildController.h`
- Modify: `Source/AirportMgr/RoadBuildController.cpp` (`BeginPlay`, `SetupInputComponent`, `OnLandAircraft`, `MakeToolContext`, `SelectToolByKey`, `OnUndo`, `OnRedo`, the clock handlers)
- Create: `Source/AirportMgr/ClickModifierTest.cpp`

**Interfaces:**
- Produces on `ARoadBuildController` (all public):
```cpp
enum class EClickModifier : uint8 { None, Remove, Insert };   // in the header, above the class
void SelectTool(int32 Index);
int32 GetActiveToolIndex() const;
void ToggleClickModifier(EClickModifier Mode);
EClickModifier GetClickModifier() const;
bool IsWatchingAgent() const;
bool IsGuidelineOverlayOn() const;
bool CanUndo() const; bool CanRedo() const;
bool HasNetworkContent() const; bool HasRunway() const; bool HasAgent() const;
bool HasOpsRuntime() const; bool IsPaused() const;
void StepSpeed(int32 Delta); void TogglePause(); void QuickSave(); void QuickLoad();
void LandAircraftNearViewFocus();
void OnUndo(); void OnRedo(); void OnClearNetwork(); void OnToggleGuidelines(); void ToggleWatchAgent();  // become public
```
- `MakeToolContext` reports `bRemove = Mode == Remove || CtrlHeld`, `bInsert = Mode == Insert || ShiftHeld`.

- [ ] **Step 1: Write the failing test**

`Source/AirportMgr/ClickModifierTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "RoadBuildController.h"
#include "Tool/BuildSession.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClickModifierTest,
	"AirportMgr.Actions.ClickModifier",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FClickModifierTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadBuildController* C = World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C)) { return false; }

	// ONE ENUM, NOT TWO BOOLS: the illegal "remove and insert at once" is unrepresentable.
	TestEqual(TEXT("starts with no modifier"), C->GetClickModifier(), EClickModifier::None);
	C->ToggleClickModifier(EClickModifier::Remove);
	TestEqual(TEXT("Remove sticks"), C->GetClickModifier(), EClickModifier::Remove);
	C->ToggleClickModifier(EClickModifier::Insert);
	TestEqual(TEXT("Insert replaces Remove rather than joining it"), C->GetClickModifier(), EClickModifier::Insert);
	C->ToggleClickModifier(EClickModifier::Insert);
	TestEqual(TEXT("toggling the lit one clears it"), C->GetClickModifier(), EClickModifier::None);

	// A mode is chosen for a tool. Picking another tool drops it, so a Remove lit for the
	// road tool cannot silently delete the first stand the player clicks.
	C->ToggleClickModifier(EClickModifier::Remove);
	C->SelectTool(1);
	TestEqual(TEXT("selecting a tool clears the modifier"), C->GetClickModifier(), EClickModifier::None);
	TestEqual(TEXT("and the tool changed"), C->GetActiveToolIndex(), 1);
	TestEqual(TEXT("selecting out of range is refused"), (C->SelectTool(99), C->GetActiveToolIndex()), 1);
	return true;
}

#endif
```
Note: `SpawnActor<ARoadBuildController>` in a bare world has no `Target` actor and no input component; the modifier and tool-selection paths must not need either. `SelectTool` calls `Session.SelectTool(Index, MakeToolContext())` and `MakeToolContext` must tolerate `Target == nullptr` (check it does: it reads `Target->SurfaceZ` in `CursorOnRoadPlane`; that function returns false early when `Target == nullptr` — verify with `grep -n "CursorOnRoadPlane" -A 6 Source/AirportMgr/RoadBuildController.cpp`; if it dereferences Target first, add the null check).

- [ ] **Step 2: Build to verify failure**

Expected: FAIL, `EClickModifier` undeclared (and Task 1's file fails on the missing members).

- [ ] **Step 3: Header changes**

Above `UCLASS()` in `RoadBuildController.h`:
```cpp
/**
 * What a plain click means right now. ONE ENUM: Remove and Insert can never both be lit,
 * so the state that would need a rule to resolve is not representable (CLAUDE.md, "a phase
 * is an enum, never a set of bools"). Ctrl and Shift held on the keyboard OR with this in
 * MakeToolContext, so the keys keep working and light the same button.
 */
UENUM()
enum class EClickModifier : uint8
{
	None,
	Remove,
	Insert
};
```
Add `#include "BuildActions.h"` is NOT needed in the header (the controller does not name the registry types); the .cpp includes it.

In the class, `public:` section after `GetActiveTool()`:
```cpp
	// --- Actions ---------------------------------------------------------------------
	//
	// Everything below is what BuildActions() calls. The registry, not this class, decides
	// which key and which button each maps to; these are the verbs and the state queries.

	/** Selects a tool by registry index and clears the click modifier - see EClickModifier. */
	void SelectTool(int32 Index);
	int32 GetActiveToolIndex() const;

	/** Lights Mode, or clears it if it was already lit. */
	void ToggleClickModifier(EClickModifier Mode);
	EClickModifier GetClickModifier() const { return ClickModifier; }

	bool IsWatchingAgent() const { return bWatchingAgent; }
	bool IsGuidelineOverlayOn() const { return bShowGuidelines; }
	bool CanUndo() const;
	bool CanRedo() const;
	bool HasNetworkContent() const;
	bool HasRunway() const;
	bool HasAgent() const;
	bool HasOpsRuntime() const;
	bool IsPaused() const;

	void StepSpeed(int32 Delta);
	void TogglePause();
	void QuickSave();
	void QuickLoad();

	/**
	 * Lands at the runway nearest the VIEW FOCUS. The bar's Land button is clicked with the
	 * cursor on the bar, where "nearest the cursor" is meaningless; the focus is where the
	 * player is looking. The key does the same for one-action-one-behaviour.
	 */
	void LandAircraftNearViewFocus();
```
Move `OnUndo`, `OnRedo`, `OnClearNetwork`, `OnToggleGuidelines`, `ToggleWatchAgent` declarations to `public:` (they are executors now). Remove the `OnSpeedDown/OnSpeedUp/OnTogglePause/OnQuickSave/OnQuickLoad` declarations (replaced by the public verbs above). Add a private:
```cpp
	/** The registry's key handler: finds the action whose key and Ctrl requirement match. */
	void OnActionKey(FKey Key);

	UPROPERTY(Transient) EClickModifier ClickModifier = EClickModifier::None;
```
Keep `SelectToolByKey` declared; its body becomes a forwarder (below) so nothing that called it breaks.

- [ ] **Step 4: Controller body**

In `RoadBuildController.cpp` add `#include "BuildActions.h"`, `#include "Model/AirsideCapability.h"`, `#include "Model/RoadNetwork.h"` (already), `#include "Model/SimClock.h"` (already).

Replace the whole key-binding block in `SetupInputComponent` (from the comment "AN ACTION, NOT A TOOL" through the K/L binds, AND the tool registry loop above it) with:
```cpp
	// EVERY key comes from BuildActions(), the same table the bar and the banner read - so a
	// key cannot exist without a button, nor a button without a key. Ctrl actions bind the
	// chord, which is what makes "Ctrl+Z" one fact rather than a bare Z plus a check inside
	// the handler that the button could not share.
	for (const FBuildAction& Action : BuildActions())
	{
		if (!Action.Key.IsValid())
		{
			continue;
		}
		const FInputChord Chord(Action.Key, /*shift*/ false, /*ctrl*/ Action.bRequiresCtrl, /*alt*/ false, /*cmd*/ false);
		InputComponent->BindKey(Chord, IE_Pressed, this, &ARoadBuildController::OnActionKey);
	}
```
Keep the mouse and wheel binds. Add:
```cpp
void ARoadBuildController::OnActionKey(FKey Key)
{
	// The chord is already matched by the binding; Ctrl state is re-read only to pick between
	// two actions on the same key that differ by it (none today, but the table allows it).
	const bool bCtrl = IsInputKeyDown(EKeys::LeftControl) || IsInputKeyDown(EKeys::RightControl);
	for (const FBuildAction& Action : BuildActions())
	{
		if (Action.Key == Key && Action.bRequiresCtrl == bCtrl)
		{
			Action.Execute(*this);
			return;
		}
	}
}

void ARoadBuildController::SelectToolByKey(FKey Key)
{
	// Kept for callers by name; the registry route is OnActionKey -> SelectTool.
	OnActionKey(Key);
}

void ARoadBuildController::SelectTool(int32 Index)
{
	if (!ToolRegistry().IsValidIndex(Index))
	{
		return;
	}
	Session.SelectTool(Index, MakeToolContext());
	// A sticky modifier was chosen for the tool it was lit under. Dropping it here is what
	// stops a Remove left on from the road tool deleting the first stand the player clicks.
	ClickModifier = EClickModifier::None;
	if (IBuildTool* Active = Session.GetActiveTool())
	{
		UE_LOG(LogRoadBuild, Log, TEXT("Tool: %s"), *Active->GetDisplayName().ToString());
	}
}

int32 ARoadBuildController::GetActiveToolIndex() const
{
	return Session.GetActiveToolIndex();
}

void ARoadBuildController::ToggleClickModifier(EClickModifier Mode)
{
	ClickModifier = (ClickModifier == Mode) ? EClickModifier::None : Mode;
	UE_LOG(LogRoadBuild, Log, TEXT("Click modifier: %s"), *UEnum::GetValueAsString(ClickModifier));
}

bool ARoadBuildController::CanUndo() const { return Target != nullptr && Target->CanUndo(); }
bool ARoadBuildController::CanRedo() const { return Target != nullptr && Target->CanRedo(); }

bool ARoadBuildController::HasNetworkContent() const
{
	return Target != nullptr && Target->Network != nullptr
		&& (Target->Network->GetNodes().Num() > 0 || Target->Network->GetAprons().Num() > 0);
}

bool ARoadBuildController::HasRunway() const
{
	// One pass over the segments per query. The bar polls this every frame; at this
	// project's segment counts that is nothing, and a cache would need invalidating on
	// every edit - the facade's OnChanged - for a saving nobody would measure.
	return Target != nullptr && Target->Network != nullptr
		&& AirsideCapability::Summarise(*Target->Network).Runways.Num() > 0;
}

bool ARoadBuildController::HasAgent() const { return Target != nullptr && Target->GetAgentCount() > 0; }

bool ARoadBuildController::HasOpsRuntime() const { return UOpsRuntimeSubsystem::Get(GetWorld()) != nullptr; }

bool ARoadBuildController::IsPaused() const
{
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	return Runtime != nullptr && Runtime->GetClock()->GetSpeed() == ESimSpeed::Paused;
}

void ARoadBuildController::StepSpeed(int32 Delta) { if (UOpsRuntime* R = RuntimeFor(*this)) { R->StepSpeed(Delta); } }
void ARoadBuildController::TogglePause()          { if (UOpsRuntime* R = RuntimeFor(*this)) { R->TogglePause(); } }
void ARoadBuildController::QuickSave()            { if (UOpsRuntime* R = RuntimeFor(*this)) { R->SaveToSlot(TEXT("QuickSave")); } }
void ARoadBuildController::QuickLoad()            { if (UOpsRuntime* R = RuntimeFor(*this)) { R->LoadFromSlot(TEXT("QuickSave")); } }
```
Delete the five `OnSpeedDown/OnSpeedUp/OnTogglePause/OnQuickSave/OnQuickLoad` bodies (the `RuntimeFor` helper stays).

`OnLandAircraft` becomes:
```cpp
void ARoadBuildController::OnLandAircraft()
{
	// Kept by name for anything that still calls it. The registry lands near the view focus.
	LandAircraftNearViewFocus();
}

void ARoadBuildController::LandAircraftNearViewFocus()
{
	if (Target == nullptr)
	{
		return;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Land: nearest runway to the view focus (%.0f, %.0f)"),
		TargetView.Focus.X, TargetView.Focus.Y);
	Target->DispatchArrival(TargetView.Focus, UAirsideSettings::ResolveDefaultAirframe());
}
```
(Move any comment lines that were between the old body's `CursorOnRoadPlane` call and `DispatchArrival` into `LandAircraftNearViewFocus` above the dispatch, so no WHY comment is lost. Read the old body first.)

`OnUndo`/`OnRedo`: delete the `!IsRemoveHeld()` clause and its comment; replace the comment with `// Ctrl+Z is bound as a chord from BuildActions(); the handler no longer re-checks it.`

`MakeToolContext`: change the two arguments to
```cpp
	return Session.MakeContext(Target, PlaneHit, Tunables,
		ClickModifier == EClickModifier::Remove || IsRemoveHeld(),
		ClickModifier == EClickModifier::Insert || IsInputKeyDown(EKeys::LeftShift) || IsInputKeyDown(EKeys::RightShift));
```

`BeginPlay`: replace the banner block with one generated from the registry:
```cpp
	FString Keys;
	for (const FBuildAction& Action : BuildActions())
	{
		if (!Action.Key.IsValid()) { continue; }
		Keys += FString::Printf(TEXT("%s%s%s %s"), Keys.IsEmpty() ? TEXT("") : TEXT(", "),
			Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""), *Action.Key.GetDisplayName().ToString(),
			*Action.Label.ToString());
	}
	UE_LOG(LogRoadBuild, Log,
		TEXT("Road building ready on %s. Left click places and connects, right click ends the chain. ")
		TEXT("Keys: %s. WASD pans, Q/E rotate, wheel zooms. Every key is also a button on the bar."),
		*Target->GetName(), *Keys);
```
and set the input mode right after `CreateBuildCamera()`:
```cpp
	// Game AND UI: the bar's buttons must take a click before the road tool sees it, and the
	// camera keys must keep working while the bar has focus.
	FInputModeGameAndUI Mode;
	Mode.SetHideCursorDuringCapture(false);
	SetInputMode(Mode);
```
Delete `ToolKeys` and the loop that built it.

`FBuildSession::GetActiveToolIndex()` may not exist: `grep -n "ActiveTool" Plugins/Airside/Source/Airside/Public/Tool/BuildSession.h`. If only the private `int32 ActiveTool` exists, add beside `GetActiveTool()`: `int32 GetActiveToolIndex() const { return ActiveTool; }`.

- [ ] **Step 5: Build, run**

Build. `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.Actions`. Expected: `2 test(s) run, 0 failed, 0 crashed.` Then the full default run: `0 failed, 0 crashed`. Grep `UE_LOG(` count in `RoadBuildController.cpp` before (git show main) and after; must not fall.

- [ ] **Step 6: Commit**

```bash
git add Source/AirportMgr Tools/Run-AirsideTests.ps1
git commit -m "feat(hud): action registry drives key bindings and banner; click modifier enum"
```

---

### Task 3: The bar widget

**Files:**
- Modify: `Source/AirportMgr/AirportMgr.Build.cs`
- Create: `Source/AirportMgr/BuildBarWidget.h`, `Source/AirportMgr/BuildBarWidget.cpp`
- Create: `Source/AirportMgr/BuildBarWidgetTest.cpp`
- Modify: `Source/AirportMgr/RoadBuildController.h/.cpp` (create the bar at BeginPlay)
- Modify: `Source/AirportMgr/RoadBuildHUD.cpp` (remove tool-name and clock lines)

**Interfaces:**
- Produces: `UBuildBarWidget : UUserWidget` with `int32 ButtonCountForTest(EActionSection) const`; `UBuildBarEntry : UObject`. Controller property `UPROPERTY(Config, EditAnywhere, Category = "Airside|UI") TSubclassOf<UBuildBarWidget> BuildBarClass;` and `UPROPERTY(Transient) TObjectPtr<UBuildBarWidget> BuildBar;`.

- [ ] **Step 1: Build.cs**

Add `"UMG", "Slate", "SlateCore"` to `PublicDependencyModuleNames` in `AirportMgr.Build.cs` with the comment `// UMG: the build bar. Slate/SlateCore: FInputChord and the UMG types' bases.` and delete the two "Uncomment if you are using Slate UI" lines.

- [ ] **Step 2: Write the failing test**

`Source/AirportMgr/BuildBarWidgetTest.cpp`:
```cpp
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildBarWidgetTest,
	"AirportMgr.Actions.BarBuildsFromRegistry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FBuildBarWidgetTest::RunTest(const FString& Parameters)
{
	// THE CONSUMER CHECK. The registry test proves the list is well formed; this proves the
	// bar READS it - one button per action, in the right section - with no asset at all,
	// which is the degraded path the design promises still works.
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("the bar is created with no asset"), Bar)) { return false; }

	for (uint8 S = 0; S <= static_cast<uint8>(EActionSection::Game); ++S)
	{
		const EActionSection Section = static_cast<EActionSection>(S);
		int32 Expected = 0;
		for (const FBuildAction& A : BuildActions()) { if (A.Section == Section) { ++Expected; } }
		TestEqual(*FString::Printf(TEXT("section %s has one button per action"), ActionSectionName(Section)),
			Bar->ButtonCountForTest(Section), Expected);
	}
	TestTrue(TEXT("the bar has a root widget to show"), Bar->HasRootWidgetForTest());
	return true;
}

#endif
```

- [ ] **Step 3: Build to verify failure**

Expected: FAIL, `BuildBarWidget.h` not found.

- [ ] **Step 4: Write the widget header**

`Source/AirportMgr/BuildBarWidget.h`:
```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "BuildActions.h"
#include "BuildBarWidget.generated.h"

class UButton;
class UPanelWidget;
class UTextBlock;
class UBuildBarWidget;
class ARoadBuildController;

/**
 * One button on the bar and the action it runs. A UObject because UButton::OnClicked is a
 * dynamic delegate and binds only to a UFUNCTION on a UObject; a lambda cannot bind.
 * Holds the action's INDEX into BuildActions(), never a name to look up.
 */
UCLASS()
class UBuildBarEntry : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY() int32 ActionIndex = INDEX_NONE;
	UPROPERTY() TObjectPtr<UButton> Button;
	UPROPERTY() TObjectPtr<UTextBlock> Label;
	UPROPERTY() TWeakObjectPtr<UBuildBarWidget> Owner;

	UFUNCTION() void HandleClicked();
};

/**
 * The bottom bar. Sections are Blueprint-authored panels bound by name; their CONTENTS are
 * generated here from BuildActions(), so the asset never lists an action and a new action
 * appears the moment it is registered. Any slot the asset lacks is built in code, so the
 * bar works with no asset at all (AirportMgr.Actions.BarBuildsFromRegistry proves it).
 *
 * State is POLLED each tick rather than subscribed: the fifteen booleans come from four
 * owners (session, controller, actor, runtime), and fifteen reads a frame cost nothing next
 * to four subscriptions and their lifetime rules.
 */
UCLASS()
class AIRPORTMGR_API UBuildBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// Slots the Blueprint may supply. Optional: a missing one is created in code.
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> TimeSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ToolsSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> EditSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> AircraftSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> GameSection;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ClockText;
	UPROPERTY(meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NotificationText;

	// Style knobs a Blueprint subclass overrides without a build.
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor NormalTint = FLinearColor(0.18f, 0.20f, 0.24f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor ActiveTint = FLinearColor(0.95f, 0.75f, 0.20f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FLinearColor DisabledTint = FLinearColor(0.10f, 0.10f, 0.12f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") FMargin ButtonPadding = FMargin(10.0f, 6.0f);
	UPROPERTY(EditAnywhere, Category = "Bar|Style") int32 FontSize = 12;

	/** Runs an action by registry index on the owning controller. Called by entries. */
	void RunAction(int32 ActionIndex);

	int32 ButtonCountForTest(EActionSection Section) const;
	bool HasRootWidgetForTest() const;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	UPROPERTY() TArray<TObjectPtr<UBuildBarEntry>> Entries;

	ARoadBuildController* Controller() const;
	UPanelWidget* SectionPanel(EActionSection Section) const;
	void EnsureSlots();
	void BuildButtons();
	void RefreshState();
	void RefreshClock();

	UFUNCTION() void OnNotification(const FString& Text);
};
```

- [ ] **Step 5: Write the widget body**

`Source/AirportMgr/BuildBarWidget.cpp`:
```cpp
#include "BuildBarWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Model/OpsEvents.h"
#include "Model/SimClock.h"
#include "Present/OpsRuntime.h"
#include "Present/OpsRuntimeSubsystem.h"
#include "RoadBuildController.h"

DEFINE_LOG_CATEGORY_STATIC(LogBuildBar, Log, All);

void UBuildBarEntry::HandleClicked()
{
	if (UBuildBarWidget* Bar = Owner.Get())
	{
		Bar->RunAction(ActionIndex);
	}
}

ARoadBuildController* UBuildBarWidget::Controller() const
{
	// The owning player when the controller created us; the first controller otherwise
	// (tests create the bar from a world). Null is a supported state: buttons still build,
	// and RefreshState simply has nothing to ask.
	if (APlayerController* Owning = GetOwningPlayer())
	{
		return Cast<ARoadBuildController>(Owning);
	}
	return GetWorld() ? Cast<ARoadBuildController>(GetWorld()->GetFirstPlayerController()) : nullptr;
}

void UBuildBarWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	EnsureSlots();
	BuildButtons();

	if (UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld()))
	{
		Runtime->GetEvents()->OnNotification.AddDynamic(this, &UBuildBarWidget::OnNotification);
	}
}

void UBuildBarWidget::EnsureSlots()
{
	// A root only if the asset gave none: BindWidgetOptional has already filled every slot
	// the asset supplies, and a code-built root would replace the designer's bar.
	UPanelWidget* Fallback = nullptr;
	if (WidgetTree->RootWidget == nullptr)
	{
		UVerticalBox* Root = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("FallbackRoot"));
		WidgetTree->RootWidget = Root;
		if (NotificationText == nullptr)
		{
			NotificationText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NotificationText"));
			Root->AddChildToVerticalBox(NotificationText);
		}
		UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("FallbackBar"));
		Root->AddChildToVerticalBox(Bar);
		Fallback = Bar;
		UE_LOG(LogBuildBar, Log, TEXT("No bar asset: building a plain code-only bar"));
	}

	auto Ensure = [&](TObjectPtr<UPanelWidget>& Slot, const TCHAR* Name)
	{
		if (Slot != nullptr) { return; }
		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), Name);
		if (Fallback != nullptr)
		{
			UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBox>(Fallback)->AddChildToHorizontalBox(Box);
			BoxSlot->SetPadding(FMargin(12.0f, 0.0f));
		}
		else if (UPanelWidget* Root = Cast<UPanelWidget>(WidgetTree->RootWidget))
		{
			// The asset has a root but not this section: append to the root so the buttons
			// are at least visible, and say so - the designer forgot a panel.
			Root->AddChild(Box);
			UE_LOG(LogBuildBar, Warning, TEXT("Bar asset has no '%s' panel; appended a plain one to the root"), Name);
		}
		Slot = Box;
	};
	Ensure(TimeSection, TEXT("TimeSection"));
	Ensure(ToolsSection, TEXT("ToolsSection"));
	Ensure(EditSection, TEXT("EditSection"));
	Ensure(AircraftSection, TEXT("AircraftSection"));
	Ensure(GameSection, TEXT("GameSection"));

	if (ClockText == nullptr)
	{
		ClockText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ClockText"));
		TimeSection->AddChild(ClockText);
	}
}

UPanelWidget* UBuildBarWidget::SectionPanel(EActionSection Section) const
{
	switch (Section)
	{
	case EActionSection::Time:     return TimeSection;
	case EActionSection::Tools:    return ToolsSection;
	case EActionSection::Edit:     return EditSection;
	case EActionSection::Aircraft: return AircraftSection;
	case EActionSection::Game:     return GameSection;
	}
	return nullptr;
}

void UBuildBarWidget::BuildButtons()
{
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		const FBuildAction& Action = Actions[Index];
		UPanelWidget* Panel = SectionPanel(Action.Section);
		if (Panel == nullptr) { continue; }

		UBuildBarEntry* Entry = NewObject<UBuildBarEntry>(this);
		Entry->ActionIndex = Index;
		Entry->Owner = this;
		Entry->Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		Entry->Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());

		FString Caption = Action.Label.ToString();
		if (Action.Key.IsValid())
		{
			Caption += FString::Printf(TEXT(" (%s%s)"), Action.bRequiresCtrl ? TEXT("Ctrl+") : TEXT(""),
				*Action.Key.GetDisplayName().ToString());
		}
		Entry->Label->SetText(FText::FromString(Caption));
		FSlateFontInfo Font = Entry->Label->GetFont();
		Font.Size = FontSize;
		Entry->Label->SetFont(Font);

		Entry->Button->SetContent(Entry->Label);
		Entry->Button->SetBackgroundColor(NormalTint);
		Entry->Button->OnClicked.AddDynamic(Entry, &UBuildBarEntry::HandleClicked);

		if (UHorizontalBox* Box = Cast<UHorizontalBox>(Panel))
		{
			Box->AddChildToHorizontalBox(Entry->Button)->SetPadding(ButtonPadding);
		}
		else
		{
			Panel->AddChild(Entry->Button);
		}
		Entries.Add(Entry);
	}
	UE_LOG(LogBuildBar, Log, TEXT("Build bar: %d buttons from %d actions"), Entries.Num(), Actions.Num());
}

void UBuildBarWidget::RunAction(int32 ActionIndex)
{
	ARoadBuildController* C = Controller();
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	if (C == nullptr || !Actions.IsValidIndex(ActionIndex))
	{
		UE_LOG(LogBuildBar, Warning, TEXT("Bar click %d ignored: no controller or no such action"), ActionIndex);
		return;
	}
	if (!Actions[ActionIndex].IsEnabled(*C))
	{
		return;
	}
	UE_LOG(LogBuildBar, Log, TEXT("Bar: %s"), *Actions[ActionIndex].Id.ToString());
	Actions[ActionIndex].Execute(*C);
}

void UBuildBarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshState();
	RefreshClock();
}

void UBuildBarWidget::RefreshState()
{
	const ARoadBuildController* C = Controller();
	if (C == nullptr) { return; }
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	for (UBuildBarEntry* Entry : Entries)
	{
		if (Entry == nullptr || !Actions.IsValidIndex(Entry->ActionIndex)) { continue; }
		const FBuildAction& Action = Actions[Entry->ActionIndex];
		const bool bEnabled = Action.IsEnabled(*C);
		Entry->Button->SetIsEnabled(bEnabled);
		Entry->Button->SetBackgroundColor(!bEnabled ? DisabledTint : Action.IsActive(*C) ? ActiveTint : NormalTint);
	}
}

void UBuildBarWidget::RefreshClock()
{
	if (ClockText == nullptr) { return; }
	const UOpsRuntime* Runtime = UOpsRuntimeSubsystem::Get(GetWorld());
	if (Runtime == nullptr)
	{
		ClockText->SetText(FText::FromString(TEXT("no clock")));
		return;
	}
	const USimClock* Clock = Runtime->GetClock();
	const int32 Hour = static_cast<int32>(Clock->TimeOfDay() / 3600.0);
	const int32 Minute = static_cast<int32>(FMath::Fmod(Clock->TimeOfDay(), 3600.0) / 60.0);
	ClockText->SetText(FText::FromString(FString::Printf(TEXT("Day %d  %02d:%02d  x%.0f%s"),
		Clock->Day() + 1, Hour, Minute, USimClock::Multiplier(Clock->GetSpeed()),
		Clock->GetSpeed() == ESimSpeed::Paused ? TEXT("  PAUSED") : TEXT(""))));
}

void UBuildBarWidget::OnNotification(const FString& Text)
{
	if (NotificationText != nullptr)
	{
		NotificationText->SetText(FText::FromString(Text));
	}
}

int32 UBuildBarWidget::ButtonCountForTest(EActionSection Section) const
{
	const TConstArrayView<FBuildAction> Actions = BuildActions();
	int32 Count = 0;
	for (const UBuildBarEntry* Entry : Entries)
	{
		if (Entry != nullptr && Actions.IsValidIndex(Entry->ActionIndex) && Actions[Entry->ActionIndex].Section == Section)
		{
			++Count;
		}
	}
	return Count;
}

bool UBuildBarWidget::HasRootWidgetForTest() const
{
	return WidgetTree != nullptr && WidgetTree->RootWidget != nullptr;
}
```
`UTextBlock::GetFont()` exists in 5.8 (`grep -n "GetFont" D:/Epic/UE_5.8/Engine/Source/Runtime/UMG/Public/Components/TextBlock.h`); if it is `Font` public instead, read the member directly.

- [ ] **Step 6: Controller creates the bar**

Header, in the properties area:
```cpp
	/**
	 * The bar's Blueprint class. Config so DefaultGame.ini names WBP_BuildBar without a
	 * Blueprint subclass of this controller existing to hold the default. Null means the
	 * plain C++ bar, which works and says so in the log.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Airside|UI")
	TSubclassOf<UBuildBarWidget> BuildBarClass;

	UPROPERTY(Transient) TObjectPtr<UBuildBarWidget> BuildBar;
```
and `UCLASS(Config = Game)` on the class. Forward-declare `class UBuildBarWidget;`. In `BeginPlay` after the input mode:
```cpp
	const TSubclassOf<UBuildBarWidget> BarClass = BuildBarClass != nullptr ? BuildBarClass : TSubclassOf<UBuildBarWidget>(UBuildBarWidget::StaticClass());
	BuildBar = CreateWidget<UBuildBarWidget>(this, BarClass);
	if (BuildBar != nullptr)
	{
		BuildBar->AddToViewport();
		UE_LOG(LogRoadBuild, Log, TEXT("Build bar: %s"), BuildBarClass != nullptr ? *BuildBarClass->GetName() : TEXT("code-only (no BuildBarClass configured)"));
	}
```
Include `BuildBarWidget.h` and `Blueprint/UserWidget.h` in the .cpp.

- [ ] **Step 7: Canvas HUD loses the two text lines**

In `RoadBuildHUD.cpp`, delete the `if (bDrawToolName && GEngine != nullptr) { ... }` block (tool name and the clock block inside it) and the three `#include`s it needed (`Model/SimClock.h`, `Present/OpsRuntime.h`, `Present/OpsRuntimeSubsystem.h`). In `RoadBuildHUD.h`, delete `bDrawToolName` and its comment. Add above the preview call: `// The tool name and the clock moved to UBuildBarWidget; this class draws only in world space now.`

- [ ] **Step 8: Build, run**

Build. `./Tools/Run-AirsideTests.ps1 -Filter AirportMgr.Actions`: `3 test(s) run, 0 failed, 0 crashed.` If `CreateWidget` from a bare world fails (it needs a game instance for `GetOwningPlayer` paths only, not for construction), the log line will say which; `CreateWidget<>(UWorld*, ...)` is the overload for exactly this. Then the full default run.

- [ ] **Step 9: Commit**

```bash
git add Source/AirportMgr
git commit -m "feat(hud): UBuildBarWidget - sectioned bottom bar generated from the action registry"
```

---

### Task 4: The Blueprint asset and its script

**Files:**
- Create: `Tools/Python/build_bar_widget.py`
- Modify: `Config/DefaultGame.ini`

**Interfaces:**
- Produces `/Game/UI/WBP_BuildBar` (parent `UBuildBarWidget`) with widgets named exactly `TimeSection`, `ToolsSection`, `EditSection`, `AircraftSection`, `GameSection`, `ClockText`, `NotificationText`.

- [ ] **Step 1: Write the script**

`Tools/Python/build_bar_widget.py` (run with the editor CLOSED, like the other authoring scripts; read `build_stand_asset.py`'s header for the command line and copy it):
```python
"""
Creates /Game/UI/WBP_BuildBar: the bottom bar's CHROME only - a Border along the bottom
edge, a HorizontalBox with five named section panels and the two text blocks
UBuildBarWidget binds by name. No buttons: those are generated from BuildActions() at
runtime, so this asset never has to agree with a list in code.

Run (editor closed):
  "D:/Epic/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" C:/repos/AirportMgr2/AirportMgr.uproject
      -run=pythonscript -script="C:/repos/AirportMgr2/Tools/Python/build_bar_widget.py" -unattended -nopause
"""
import unreal

ASSET_DIR = "/Game/UI"
ASSET_NAME = "WBP_BuildBar"
SECTIONS = ["TimeSection", "ToolsSection", "EditSection", "AircraftSection", "GameSection"]

def new_widget(tree, cls, name):
    return unreal.new_object(cls, outer=tree, name=name)

def build():
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = "%s/%s" % (ASSET_DIR, ASSET_NAME)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)

    factory = unreal.WidgetBlueprintFactory()
    factory.set_editor_property("parent_class", unreal.load_class(None, "/Script/AirportMgr.BuildBarWidget"))
    bp = tools.create_asset(ASSET_NAME, ASSET_DIR, unreal.WidgetBlueprint, factory)
    if bp is None:
        raise RuntimeError("create_asset returned None for %s" % path)

    tree = bp.get_editor_property("widget_tree")
    canvas = new_widget(tree, unreal.CanvasPanel, "Root")
    tree.set_editor_property("root_widget", canvas)

    notification = new_widget(tree, unreal.TextBlock, "NotificationText")
    notification.set_text(unreal.Text(""))
    n_slot = canvas.add_child_to_canvas(notification)
    n_slot.set_anchors(unreal.Anchors(minimum=unreal.Vector2D(0.5, 1.0), maximum=unreal.Vector2D(0.5, 1.0)))
    n_slot.set_alignment(unreal.Vector2D(0.5, 1.0))
    n_slot.set_offsets(unreal.Margin(0.0, -72.0, 0.0, 0.0))
    n_slot.set_auto_size(True)

    border = new_widget(tree, unreal.Border, "BarBorder")
    border.set_brush_color(unreal.LinearColor(0.06, 0.07, 0.09, 0.92))
    border.set_padding(unreal.Margin(12.0, 8.0, 12.0, 8.0))
    b_slot = canvas.add_child_to_canvas(border)
    b_slot.set_anchors(unreal.Anchors(minimum=unreal.Vector2D(0.0, 1.0), maximum=unreal.Vector2D(1.0, 1.0)))
    b_slot.set_alignment(unreal.Vector2D(0.0, 1.0))
    b_slot.set_offsets(unreal.Margin(0.0, 0.0, 0.0, 56.0))   # left, top, right(=stretch), bottom(=height)

    bar = new_widget(tree, unreal.HorizontalBox, "Bar")
    border.set_content(bar)
    for index, name in enumerate(SECTIONS):
        if index > 0:
            spacer = new_widget(tree, unreal.Spacer, "Spacer%d" % index)
            spacer.set_size(unreal.Vector2D(24.0, 1.0))
            bar.add_child_to_horizontal_box(spacer)
        section = new_widget(tree, unreal.HorizontalBox, name)
        s_slot = bar.add_child_to_horizontal_box(section)
        s_slot.set_vertical_alignment(unreal.VerticalAlignment.V_ALIGN_CENTER)
        if name == "TimeSection":
            clock = new_widget(tree, unreal.TextBlock, "ClockText")
            clock.set_text(unreal.Text("Day 1  00:00  x1"))
            c_slot = section.add_child_to_horizontal_box(clock)
            c_slot.set_padding(unreal.Margin(0.0, 0.0, 12.0, 0.0))
            c_slot.set_vertical_alignment(unreal.VerticalAlignment.V_ALIGN_CENTER)

    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_asset(path)

    # The names are the contract with the C++ base; a typo here is a section that silently
    # falls back to a plain code-built panel, so verify every one is present.
    names = set()
    def walk(w):
        if w is None: return
        names.add(w.get_name())
        if isinstance(w, unreal.PanelWidget):
            for i in range(w.get_children_count()):
                walk(w.get_child_at(i))
        elif isinstance(w, unreal.ContentWidget):
            walk(w.get_content())
    walk(tree.get_editor_property("root_widget"))
    missing = [n for n in SECTIONS + ["ClockText", "NotificationText"] if n not in names]
    if missing:
        raise RuntimeError("WBP_BuildBar is missing named widgets: %s" % missing)
    unreal.log("Built %s. Set in DefaultGame.ini: [/Script/AirportMgr.RoadBuildController] BuildBarClass=%s.%s_C" % (path, path, ASSET_NAME))

build()
```
If `widget_tree` is not readable from Python (`get_editor_property` raises), stop: the C++ code-only bar already works, and the asset is then authored by hand from the seven names above (write that in the PR). Do not spend more than one attempt on the Python route.

- [ ] **Step 2: Run it (editor closed)**

```powershell
& "D:/Epic/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "C:/repos/AirportMgr2/AirportMgr.uproject" -run=pythonscript -script="C:/repos/AirportMgr2/Tools/Python/build_bar_widget.py" -unattended -nopause 2>&1 | Select-String "Built|Error|missing|Traceback"
```
Expected: `Built /Game/UI/WBP_BuildBar. Set in DefaultGame.ini: ...`. Confirm `Content/UI/WBP_BuildBar.uasset` exists.

- [ ] **Step 3: Config default**

Append to `Config/DefaultGame.ini`:
```ini
; The build bar's Blueprint. Authored by Tools/Python/build_bar_widget.py; restyle it in the
; UMG designer. Remove this line to fall back to the plain code-built bar.
[/Script/AirportMgr.RoadBuildController]
BuildBarClass=/Game/UI/WBP_BuildBar.WBP_BuildBar_C
```

- [ ] **Step 4: Commit**

```bash
git add Tools/Python/build_bar_widget.py Config/DefaultGame.ini Content/UI/WBP_BuildBar.uasset
git commit -m "feat(hud): WBP_BuildBar chrome authored headlessly; config default"
```

---

### Task 5: Verification and PR

- [ ] **Step 1: Full build and test run**

Build; `./Tools/Run-AirsideTests.ps1`: `0 failed, 0 crashed`, three `AirportMgr.Actions.*` tests listed. `UE_LOG(` count in `Source/AirportMgr`: `git grep -c "UE_LOG(" main -- Source/AirportMgr | awk -F: '{s+=$NF} END{print s}'` vs HEAD; HEAD ≥ main.

- [ ] **Step 2: PIE check (user, or MCP shot)**

Open the editor, PIE. Expect: a bar along the bottom with five groups; `Build bar: WBP_BuildBar` in the log; the banner lists every key. Click **Road**, click twice on the plane: a road appears and `Bar: tool.road` precedes `Node N placed`. Click **Pause**: the clock in the bar shows PAUSED and the button lights. Click **Remove**, then click a node: it is deleted and the button unlights when a tool is selected. Click **Undo** with nothing to undo: greyed, no log line. `python Tools/Mcp.py shot out.png` shows the bar.

- [ ] **Step 3: PR**

Fill the PR template: build line, test line, `UE_LOG` delta, the seams (registry → bindings, registry → bar, bus → notification text) and their tests, runtime evidence or "builds, unverified at runtime".

---

## Self-review

**Spec coverage.** §2 registry: Task 1. §3 controller (enum, queries, chord bindings, banner, input mode, bar creation, land-near-focus): Tasks 2 and 3. §4 widget (optional slots, generated buttons, polled state, clock, notification binding, style knobs, canvas HUD trimmed): Task 3. §5 asset and config: Task 4. §6 tests: Tasks 1, 2, 3 plus the PIE check in Task 5.

**Placeholders.** None. Two "if X is missing, grep and add Y" instructions give the exact signature to add.

**Type consistency.** `EActionSection` values and `ActionSectionName` used in Tasks 1, 3. `EClickModifier` (Task 2) used by Task 1's lambdas. Controller verbs named in Task 1's registry all appear in Task 2's header list. `ButtonCountForTest(EActionSection)` and `HasRootWidgetForTest()` defined in Task 3 and used by its test. `BuildBarClass` config key in Task 4 matches Task 3's property.
