# UI Library Step 3 - Player Settings and Remembered Window Layout: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A window the player moved or resized comes back where they left it on the next launch; a layout that no longer fits the screen falls back to the default; the layout can be reset.

**Architecture:** `UAirportMgrUserSettings : UGameUserSettings` (named as the engine's `GameUserSettingsClassName`) is the per-player settings object and holds `WindowLayout`. The window host never touches it directly: it talks to an `IUiLayoutStore`, the shape `IToolPreferences` already established (#376) so a test never reads or writes the player's own `GameUserSettings.ini`. Tests hand the host an `FMemoryUiLayoutStore`; `UBuildHudLayer` alone hands it the real `FUserSettingsLayoutStore`, and a lint rule holds that wiring in place.

**Tech Stack:** UE 5.8 C++ (UMG, Engine `UGameUserSettings`), config ini, PowerShell lint.

**Spec:** `docs/superpowers/specs/2026-09-28-ui-widget-library-design.md` sections 2 (persistence) and 3 (player settings). Delivery step 3.

## Global Constraints

- Worktree `C:\repos\airportmgr2-ui-widget-library`, branch `feature/ui-widget-library`. No push, no PR (user, 2026-09-28).
- Build with `-NoHotReloadFromIDE`; close THIS worktree's editor first (it locks the DLL), by PID. Tests: `Run-AirsideTests.ps1 -Project ... -Filter AirportMgr`, read the `N test(s) run` line. New test .cpp: two builds.
- Edits via Write/Edit or Python scripts written with Write; no `\n` through bash heredocs.
- `UAirportMgrUserSettings::Get()` is the one accessor (spec section 3).
- Persist ONLY on gesture end, never per mouse move (a flush per frame of a drag).
- Window ids are `inspector`, `ledger`, `land`, `offers` (step 2) - they are the persistence keys.
- Log category `LogRoadBuild`. Comments explain WHY; `// ENFORCED BY:` on claims about other code.

## Deviation from the spec, ruled here

Spec section 3 lists six fields on `UAirportMgrUserSettings`. This step adds only `WindowLayout`, the one with a consumer now. `UIScale`, `PanSpeedScale`, `ZoomSpeedScale`, `bDriveOnLeft` and `bGridSnapDefault` land in step 4 beside the Settings control that edits each and the code that reads it - a config field nothing reads is the "declared, never consumed" bug CLAUDE.md names three times. Cost if wrong: step 4 adds five fields instead of zero.

## Review Focus

1. **A layout saved on a bigger screen** (or a window dragged to a second monitor's coordinates). Expect the default placement, not a window restored off-screen or clamped into a corner the player never chose. Task 2 `SavedLayoutOffScreenFallsBackToDefault`.
2. **A saved size below the current minimum** (edited ini, or the minimum raised later). Expect the minimum, not a sliver. Task 2 `RestoredSizeRespectsTheMinimum`.
3. **A saved size for a window that is no longer resizable** (Offers became non-resizable in step 2). Expect the position restored and the size ignored - it keeps growing with its offers. Task 2 `UnresizableWindowIgnoresASavedSize`.
4. **A drag writes to disk every frame.** Expect exactly one write per gesture, at its end. Task 2 `WritesOncePerGesture`.
5. **A saved entry for an id no window claims** (a renamed panel). Expect it ignored, and cleared by a reset. Task 2 `ResetClearsTheStoreAndRestoresDefaults`.

---

### Task 1: The placement record and the store interface

**Files:**
- Create: `Source/AirportMgr/UI/UiLayoutStore.h`, `UI/UiLayoutStore.cpp`

**Interfaces:**
- Produces:

```cpp
USTRUCT() struct FUiWindowPlacement { FVector2D TopLeft; FVector2D Size; bool bSized = false; };
class IUiLayoutStore {
	virtual TOptional<FUiWindowPlacement> Read(FName Id) const = 0;
	virtual void Write(FName Id, const FUiWindowPlacement& Placement) = 0;
	virtual void Clear() = 0;
};
class FMemoryUiLayoutStore : public IUiLayoutStore { ...; int32 GetWriteCount() const; };
```

- [ ] **Step 1: Write `UI/UiLayoutStore.h`** (no test of its own - it is exercised through the host in Task 2, where its behaviour matters):

```cpp
#pragma once

#include "CoreMinimal.h"
#include "UiLayoutStore.generated.h"

/**
 * Where a window was left: host-local top-left, and - only once the player resized it - its size.
 * bSized false means "auto-sized to its panel", which is what an untouched window is; restoring a
 * size for it would freeze a window that should grow with its content. Plain UPROPERTYs: the
 * struct is saved through UAirportMgrUserSettings::WindowLayout, whose config flag covers it.
 */
USTRUCT()
struct FUiWindowPlacement
{
	GENERATED_BODY()

	UPROPERTY() FVector2D TopLeft = FVector2D::ZeroVector;
	UPROPERTY() FVector2D Size = FVector2D::ZeroVector;
	UPROPERTY() bool bSized = false;
};

/**
 * Where the window layout is kept between launches (UI library step 3).
 *
 * AN INTERFACE, not UAirportMgrUserSettings called from the host - IToolPreferences' shape (#376)
 * and its reason: a test's host must never read or write the player's own GameUserSettings.ini,
 * or a player's dragged ledger would move where every test's ledger starts. A host starts with no
 * store (nothing remembered, nothing written); UBuildHudLayer alone hands it the real one.
 * ENFORCED BY: Check-Architecture rule 29 (layout-store-wired).
 */
class AIRPORTMGR_API IUiLayoutStore
{
public:
	virtual ~IUiLayoutStore() = default;
	/** What was stored for Id, or unset when nothing ever was. */
	virtual TOptional<FUiWindowPlacement> Read(FName Id) const = 0;
	virtual void Write(FName Id, const FUiWindowPlacement& Placement) = 0;
	/** Forgets every window - "Reset window layout". */
	virtual void Clear() = 0;
};

/** Held in memory and lost with the object - what a test hands a host. */
class AIRPORTMGR_API FMemoryUiLayoutStore : public IUiLayoutStore
{
public:
	virtual TOptional<FUiWindowPlacement> Read(FName Id) const override;
	virtual void Write(FName Id, const FUiWindowPlacement& Placement) override;
	virtual void Clear() override;
	/** How many writes reached the store - a drag must cost one, not one per frame. */
	int32 GetWriteCount() const { return WriteCount; }

private:
	TMap<FName, FUiWindowPlacement> Values;
	int32 WriteCount = 0;
};
```

- [ ] **Step 2: Write `UI/UiLayoutStore.cpp`:**

```cpp
#include "UI/UiLayoutStore.h"

TOptional<FUiWindowPlacement> FMemoryUiLayoutStore::Read(FName Id) const
{
	const FUiWindowPlacement* Found = Values.Find(Id);
	return Found != nullptr ? TOptional<FUiWindowPlacement>(*Found) : TOptional<FUiWindowPlacement>();
}

void FMemoryUiLayoutStore::Write(FName Id, const FUiWindowPlacement& Placement)
{
	Values.Add(Id, Placement);
	++WriteCount;
}

void FMemoryUiLayoutStore::Clear()
{
	Values.Reset();
}
```

- [ ] **Step 3: Build.** Expected `Result: Succeeded` (a USTRUCT header needs the build to generate it).

- [ ] **Step 4: Commit** `git add Source/AirportMgr/UI && git commit -m "feat(ui): IUiLayoutStore and the window placement record"`

---

### Task 2: The host remembers

**Files:**
- Modify: `Source/AirportMgr/UI/UiWindowHost.h/.cpp`, `UI/UiWindow.cpp` (gesture end reports to the host)
- Test: `Source/AirportMgr/UI/UiWindowHostTest.cpp`

**Interfaces:**
- Consumes: `IUiLayoutStore`, `FUiWindowPlacement`, `FMemoryUiLayoutStore` (Task 1).
- Produces on `UUiWindowHost`:

```cpp
void SetLayoutStore(TSharedPtr<IUiLayoutStore> InStore);
void CommitPlacement(FName Id);   // UUiWindow::EndGesture calls it; writes once
void ResetLayout();               // store cleared, every window back to its default placement
```

- [ ] **Step 1: Write the failing tests** - append to `UI/UiWindowHostTest.cpp` (add `#include "UI/UiLayoutStore.h"`):

```cpp
namespace UiWindowHostTest
{
	/** A host whose ledger is shown and has a known size, with a memory store attached. */
	struct FStoredFixture : FFixture
	{
		TSharedRef<FMemoryUiLayoutStore> Store = MakeShared<FMemoryUiLayoutStore>();
		FStoredFixture()
		{
			if (Host != nullptr)
			{
				Host->SetLayoutStore(Store);
			}
		}
		/** A second host over the same store - the next launch. */
		UUiWindowHost* Relaunch(ULedgerPanelWidget*& OutLedger, FVector2D ViewSize)
		{
			UUiWindowHost* Next = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
			OutLedger = CreateWidget<ULedgerPanelWidget>(TestWorld.World, ULedgerPanelWidget::StaticClass());
			Next->SetViewSizeForTest(ViewSize);
			Next->SetLayoutStore(Store);
			Next->AddWindow(*OutLedger);
			OutLedger->Toggle();
			Next->TickForTest(0.016f);   // the first tick is when a saved layout is judged
			return Next;
		}
	};
}

/**
 * A WINDOW COMES BACK WHERE THE PLAYER LEFT IT - position and size - on the next launch. Written
 * once, when the gesture ends; a drag must not flush to disk every frame (Review Focus 4).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutRestoreTest, "AirportMgr.UI.WindowHost.LayoutSurvivesARelaunch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutRestoreTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const FName Id(TEXT("ledger"));
	F.Ledger->Toggle();
	F.Host->MoveWindow(Id, FVector2D(100.0, 100.0));
	F.Host->ResizeWindow(Id, FVector2D(300.0, 200.0));
	F.Host->MoveWindow(Id, FVector2D(640.0, 380.0));
	F.Host->CommitPlacement(Id);

	ULedgerPanelWidget* Ledger = nullptr;
	UUiWindowHost* Next = F.Relaunch(Ledger, FVector2D(1920.0, 1080.0));
	TestEqual(TEXT("its position comes back"), Next->WindowRect(Id).Min, FVector2D(640.0, 380.0));
	TestEqual(TEXT("and its size"), Next->WindowRect(Id).GetSize(), FVector2D(300.0, 200.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutWritesOnceTest, "AirportMgr.UI.WindowHost.WritesOncePerGesture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutWritesOnceTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	F.Ledger->Toggle();
	F.Window->BeginGestureForTest(EUiWindowGesture::Move, FVector2D(100.0, 100.0));
	for (int32 Frame = 0; Frame < 10; ++Frame)
	{
		F.Window->MoveGestureForTest(FVector2D(100.0 + 5.0 * Frame, 100.0));
	}
	TestEqual(TEXT("ten frames of drag write nothing"), F.Store->GetWriteCount(), 0);
	F.Window->TakeWidget()->OnMouseCaptureLost(FCaptureLostEvent(0, 0));   // any gesture end
	TestEqual(TEXT("the gesture's end writes once"), F.Store->GetWriteCount(), 1);
	return true;
}

/** A LAYOUT FROM A BIGGER SCREEN FALLS BACK TO THE DEFAULT, not to a corner - Review Focus 1. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutOffScreenTest, "AirportMgr.UI.WindowHost.SavedLayoutOffScreenFallsBackToDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutOffScreenTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	FUiWindowPlacement Far;
	Far.TopLeft = FVector2D(3000.0, 1500.0);   // a 4K screen's bottom-right
	Far.Size = FVector2D(300.0, 200.0);
	Far.bSized = true;
	F.Store->Write(TEXT("ledger"), Far);

	ULedgerPanelWidget* Ledger = nullptr;
	UUiWindowHost* Next = F.Relaunch(Ledger, FVector2D(1920.0, 1080.0));
	// The ledger's default is top-right, 12 in; headless its auto size is 0x0, so its top-left IS
	// the anchor point minus the inset.
	TestEqual(TEXT("off-screen: back to the default placement"), Next->WindowRect(TEXT("ledger")).Min, FVector2D(1920.0 - 12.0, 12.0));
	return true;
}

/** A SAVED SIZE BELOW THE MINIMUM IS RAISED TO IT - Review Focus 2. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutMinSizeTest, "AirportMgr.UI.WindowHost.RestoredSizeRespectsTheMinimum",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutMinSizeTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	FUiWindowPlacement Tiny;
	Tiny.TopLeft = FVector2D(200.0, 200.0);
	Tiny.Size = FVector2D(20.0, 10.0);
	Tiny.bSized = true;
	F.Store->Write(TEXT("ledger"), Tiny);
	ULedgerPanelWidget* Ledger = nullptr;
	UUiWindowHost* Next = F.Relaunch(Ledger, FVector2D(1920.0, 1080.0));
	TestEqual(TEXT("raised to the minimum"), Next->WindowRect(TEXT("ledger")).GetSize(), FVector2D(180.0, 90.0));
	return true;
}

/** A SAVED SIZE FOR A WINDOW THAT CANNOT BE RESIZED IS IGNORED; its position is not - Review Focus 3. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutUnresizableTest, "AirportMgr.UI.WindowHost.UnresizableWindowIgnoresASavedSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutUnresizableTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	FUiWindowPlacement Saved;
	Saved.TopLeft = FVector2D(500.0, 300.0);
	Saved.Size = FVector2D(400.0, 300.0);
	Saved.bSized = true;
	F.Store->Write(TEXT("offers"), Saved);
	UOfferInboxWidget* Offers = CreateWidget<UOfferInboxWidget>(F.TestWorld.World, UOfferInboxWidget::StaticClass());
	F.Host->AddWindow(*Offers);
	F.Host->TickForTest(0.016f);
	TestEqual(TEXT("its position is restored"), F.Host->WindowRect(TEXT("offers")).Min, FVector2D(500.0, 300.0));
	TestEqual(TEXT("its size is not - it grows with its offers (headless: 0x0 desired)"),
		F.Host->WindowRect(TEXT("offers")).GetSize(), FVector2D::ZeroVector);
	return true;
}

/** RESET PUTS EVERY WINDOW BACK AND FORGETS THE STORE - including an id no window claims (Review Focus 5). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutResetTest, "AirportMgr.UI.WindowHost.ResetClearsTheStoreAndRestoresDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutResetTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	FUiWindowPlacement Orphan;
	Orphan.TopLeft = FVector2D(10.0, 10.0);
	F.Store->Write(TEXT("a.renamed.panel"), Orphan);   // no window claims it: ignored, no crash
	const FName Id(TEXT("ledger"));
	F.Ledger->Toggle();
	F.Host->TickForTest(0.016f);
	F.Host->MoveWindow(Id, FVector2D(640.0, 380.0));
	F.Host->CommitPlacement(Id);
	F.Host->ResetLayout();
	TestEqual(TEXT("the ledger is back at its default top-right inset"), F.Host->WindowRect(Id).Min, FVector2D(1920.0 - 12.0, 12.0));
	TestFalse(TEXT("its saved placement is gone"), F.Store->Read(Id).IsSet());
	TestFalse(TEXT("and so is the orphan"), F.Store->Read(TEXT("a.renamed.panel")).IsSet());
	return true;
}

/** NO STORE, NOTHING REMEMBERED - every test's host, deliberately (IUiLayoutStore's comment). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutNoStoreTest, "AirportMgr.UI.WindowHost.NoStoreRemembersNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutNoStoreTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	F.Ledger->Toggle();
	F.Host->MoveWindow(TEXT("ledger"), FVector2D(640.0, 380.0));
	F.Host->CommitPlacement(TEXT("ledger"));   // must not crash, must not reach any file
	F.Host->ResetLayout();
	TestEqual(TEXT("reset still restores the default"), F.Host->WindowRect(TEXT("ledger")).Min, FVector2D(1920.0 - 12.0, 12.0));
	return true;
}
```

- [ ] **Step 2: Build twice; expect compile failure** (`SetLayoutStore`, `CommitPlacement`, `ResetLayout`).

- [ ] **Step 3: Implement in `UiWindowHost.h`:** `#include "UI/UiLayoutStore.h"`; public:

```cpp
	/** Where the layout is remembered; null (the default, and every test's) remembers nothing. */
	void SetLayoutStore(TSharedPtr<IUiLayoutStore> InStore);
	/** The player finished moving or resizing Id: remember where it is. Once per gesture. */
	void CommitPlacement(FName Id);
	/** "Reset window layout": forget everything and put every window back where it starts. */
	void ResetLayout();
```

private: `void ApplyDefaultPlacement(FUiWindowEntry& E);`, `void RestoreSavedLayout();`, `TSharedPtr<IUiLayoutStore> LayoutStore;`, `bool bLayoutRestored = false;` (the saved layout is judged once, on the first tick - when the view's real size is known).

- [ ] **Step 4: Implement in `UiWindowHost.cpp`.** Move the anchor `switch` out of `AddWindow` into `ApplyDefaultPlacement` (AddWindow calls it); it also resets `E.bPlaced = false` and `Slot->SetAutoSize(true)`:

```cpp
void UUiWindowHost::ApplyDefaultPlacement(FUiWindowEntry& E)
{
	UCanvasPanelSlot* CanvasSlot = E.Slot;
	if (CanvasSlot == nullptr)
	{
		return;
	}
	CanvasSlot->SetAutoSize(true);
	switch (E.Spec.Anchor)
	{
	case EUiWindowAnchor::TopRight:
		CanvasSlot->SetAnchors(FAnchors(1.0f, 0.0f));
		CanvasSlot->SetAlignment(FVector2D(1.0, 0.0));
		CanvasSlot->SetPosition(FVector2D(-E.Spec.Offset.X, E.Spec.Offset.Y));
		break;
	case EUiWindowAnchor::AboveBarLeft:
		CanvasSlot->SetAnchors(FAnchors(0.0f, 1.0f));
		CanvasSlot->SetAlignment(FVector2D(0.0, 1.0));
		CanvasSlot->SetPosition(FVector2D(E.Spec.Offset.X, -(BarHeight() + E.Spec.Offset.Y)));
		break;
	default:
		CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f));
		CanvasSlot->SetAlignment(FVector2D::ZeroVector);
		CanvasSlot->SetPosition(E.Spec.Offset);
		break;
	}
	E.bPlaced = false;
}

void UUiWindowHost::SetLayoutStore(TSharedPtr<IUiLayoutStore> InStore)
{
	LayoutStore = MoveTemp(InStore);
}

void UUiWindowHost::CommitPlacement(FName Id)
{
	FUiWindowEntry* E = Find(Id);
	if (E == nullptr || !LayoutStore.IsValid() || !E->bPlaced || E->Slot == nullptr)
	{
		return;
	}
	FUiWindowPlacement P;
	P.TopLeft = E->Slot->GetPosition();   // placed: top-left anchored, so position IS the top-left
	P.bSized = !E->Slot->GetAutoSize();
	P.Size = P.bSized ? E->Slot->GetSize() : FVector2D::ZeroVector;
	LayoutStore->Write(Id, P);
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: layout saved at (%.0f, %.0f)%s"), *Id.ToString(), P.TopLeft.X, P.TopLeft.Y,
		P.bSized ? *FString::Printf(TEXT(" size (%.0f, %.0f)"), P.Size.X, P.Size.Y) : TEXT(""));
}

void UUiWindowHost::RestoreSavedLayout()
{
	if (!LayoutStore.IsValid())
	{
		return;
	}
	for (FUiWindowEntry& E : Windows)
	{
		const TOptional<FUiWindowPlacement> Saved = LayoutStore->Read(E.Spec.Id);
		if (!Saved.IsSet() || E.Slot == nullptr)
		{
			continue;
		}
		// A SIZE ONLY FOR A WINDOW THAT CAN BE RESIZED, and never below the minimum: a saved size
		// for Offers (unresizable since step 2) would freeze a window meant to grow with its offers.
		const bool bUseSize = Saved->bSized && E.Spec.bResizable;
		const FVector2D Size = bUseSize
			? FVector2D(FMath::Max(Saved->Size.X, Style->WindowMinSize.X), FMath::Max(Saved->Size.Y, Style->WindowMinSize.Y))
			: SizeOf(E);
		// OFF SCREEN -> THE DEFAULT, not a clamp: a layout from a bigger monitor clamped into this
		// one's corner is a place the player never chose (Review Focus 1).
		const FBox2D B = Bounds();
		const bool bFits = Saved->TopLeft.X >= B.Min.X && Saved->TopLeft.Y >= B.Min.Y
			&& Saved->TopLeft.X + Size.X <= B.Max.X && Saved->TopLeft.Y + Size.Y <= B.Max.Y;
		if (!bFits)
		{
			UE_LOG(LogRoadBuild, Log, TEXT("Window %s: saved layout (%.0f, %.0f) is off this screen - default placement"),
				*E.Spec.Id.ToString(), Saved->TopLeft.X, Saved->TopLeft.Y);
			continue;
		}
		E.Slot->SetAnchors(FAnchors(0.0f, 0.0f));
		E.Slot->SetAlignment(FVector2D::ZeroVector);
		E.Slot->SetPosition(Saved->TopLeft);
		if (bUseSize)
		{
			E.Slot->SetAutoSize(false);
			E.Slot->SetSize(Size);
		}
		E.bPlaced = true;
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: layout restored from settings at (%.0f, %.0f)"),
			*E.Spec.Id.ToString(), Saved->TopLeft.X, Saved->TopLeft.Y);
	}
}

void UUiWindowHost::ResetLayout()
{
	if (LayoutStore.IsValid())
	{
		LayoutStore->Clear();
	}
	for (FUiWindowEntry& E : Windows)
	{
		ApplyDefaultPlacement(E);
	}
	UE_LOG(LogRoadBuild, Log, TEXT("Window host: layout reset to defaults"));
}
```

At the top of `TickWindows`, before the loop:

```cpp
	// THE SAVED LAYOUT IS JUDGED ON THE FIRST TICK, not in AddWindow: "does it still fit" needs the
	// view's real size, which NativeTick has only just read.
	if (!bLayoutRestored)
	{
		bLayoutRestored = true;
		RestoreSavedLayout();
	}
```

(`TickForTest` reaches this too, after `SetViewSizeForTest`.)

- [ ] **Step 5: `UUiWindow::EndGesture`** - after the log line, `Host->CommitPlacement(Id);`, with the comment "REMEMBERED ON THE GESTURE'S END, never per move: a drag must cost one write, not one per frame (WritesOncePerGesture)."

- [ ] **Step 6: Build; run `-Filter AirportMgr.UI.WindowHost`.** Expected: all pass. `ResetClearsTheStoreAndRestoresDefaults` needs the orphan write to count - it does not matter to `GetWriteCount` here.

- [ ] **Step 7: Commit** `git commit -am "feat(ui): the window host remembers the layout through an IUiLayoutStore"`

---

### Task 3: UAirportMgrUserSettings and the real store

**Files:**
- Create: `Source/AirportMgr/AirportMgrUserSettings.h/.cpp`, `AirportMgrUserSettingsTest.cpp`
- Modify: `Config/DefaultEngine.ini`, `Source/AirportMgr/UI/UiLayoutStore.h/.cpp` (the real store)

**Interfaces:**
- Produces: `UAirportMgrUserSettings::Get()` (null-safe, logs once if the engine made a different class); `UPROPERTY(config) TMap<FName, FUiWindowPlacement> WindowLayout;`; `class FUserSettingsLayoutStore : public IUiLayoutStore` (+ `void Remove(FName Id)` for a test's own key).

- [ ] **Step 1: Write the failing test** `AirportMgrUserSettingsTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "AirportMgrUserSettings.h"
#include "Misc/AutomationTest.h"
#include "UI/UiLayoutStore.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ENGINE MAKES OUR SETTINGS CLASS. GameUserSettingsClassName is a config line nothing else
 * checks: left out, the engine quietly makes a plain UGameUserSettings, Get() returns null, and
 * every remembered layout is written nowhere - with every host test still green, because they all
 * use a memory store on purpose.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportMgrUserSettingsClassTest, "AirportMgr.Settings.EngineMakesOurUserSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportMgrUserSettingsClassTest::RunTest(const FString& Parameters)
{
	TestNotNull(TEXT("GEngine's GameUserSettings is a UAirportMgrUserSettings"), UAirportMgrUserSettings::Get());
	return true;
}

/**
 * THE REAL STORE ROUND-TRIPS through the settings object - on a key of the test's own, removed
 * afterwards, so the player's windows are never touched (FConfigToolPreferences' precedent).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUserSettingsLayoutStoreTest, "AirportMgr.Settings.LayoutStoreRoundTrips",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUserSettingsLayoutStoreTest::RunTest(const FString& Parameters)
{
	if (UAirportMgrUserSettings::Get() == nullptr)
	{
		AddError(TEXT("no UAirportMgrUserSettings - see EngineMakesOurUserSettings"));
		return false;
	}
	const FName Key(TEXT("test.layout.roundtrip"));
	FUserSettingsLayoutStore Store;
	FUiWindowPlacement P;
	P.TopLeft = FVector2D(321.0, 123.0);
	P.Size = FVector2D(250.0, 150.0);
	P.bSized = true;
	Store.Write(Key, P);
	const TOptional<FUiWindowPlacement> Back = Store.Read(Key);
	TestTrue(TEXT("read back"), Back.IsSet());
	if (Back.IsSet())
	{
		TestEqual(TEXT("position"), Back->TopLeft, P.TopLeft);
		TestEqual(TEXT("size"), Back->Size, P.Size);
		TestTrue(TEXT("sized"), Back->bSized);
	}
	Store.Remove(Key);
	TestFalse(TEXT("removed again - the player's file keeps nothing of this test"), Store.Read(Key).IsSet());
	return true;
}

#endif
```

- [ ] **Step 2: Build twice; expect compile failure** (`AirportMgrUserSettings.h`).

- [ ] **Step 3: Write `AirportMgrUserSettings.h/.cpp`:**

```cpp
// AirportMgrUserSettings.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "UI/UiLayoutStore.h"
#include "AirportMgrUserSettings.generated.h"

/**
 * The player's own settings (UI library step 3, spec section 3): the engine's per-player object,
 * named as GameUserSettingsClassName in DefaultEngine.ini, so it saves to the player's
 * GameUserSettings.ini beside the scalability it already owns - where a shipped game keeps them.
 *
 * ONE FIELD FOR NOW - WindowLayout. The spec's other five (UI scale, camera speeds, drive side,
 * grid snap default) arrive in step 4 with the Settings control that edits each and the code that
 * reads it: a config field nothing reads is the declared-never-consumed bug this codebase keeps
 * shipping (CLAUDE.md, "Check where a list is CONSUMED").
 */
UCLASS(config = GameUserSettings)
class AIRPORTMGR_API UAirportMgrUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	/** The one accessor. Null (logged once) if the engine was not told to make this class. */
	static UAirportMgrUserSettings* Get();

	/** Where each window was left, by window id - see FUserSettingsLayoutStore. */
	UPROPERTY(config) TMap<FName, FUiWindowPlacement> WindowLayout;
};
```

```cpp
// AirportMgrUserSettings.cpp
#include "AirportMgrUserSettings.h"

#include "Engine/Engine.h"
#include "RoadBuildLog.h"

UAirportMgrUserSettings* UAirportMgrUserSettings::Get()
{
	UAirportMgrUserSettings* Settings = GEngine != nullptr ? Cast<UAirportMgrUserSettings>(GEngine->GetGameUserSettings()) : nullptr;
	if (Settings == nullptr)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogRoadBuild, Error, TEXT("GameUserSettings is not a UAirportMgrUserSettings - is GameUserSettingsClassName set in DefaultEngine.ini? Nothing will be remembered."));
		}
	}
	return Settings;
}
```

- [ ] **Step 4: The real store** - append to `UI/UiLayoutStore.h`:

```cpp
/**
 * The player's per-user config: UAirportMgrUserSettings::WindowLayout in GameUserSettings.ini.
 * PER USER, NOT PER AIRPORT, IToolPreferences' ruling: where a player keeps their ledger is a habit,
 * like a keybinding. SAVED ON EVERY WRITE - writes happen once per gesture (CommitPlacement), and a
 * save left for shutdown is lost on a crash or a Stop-Process, the way this project's editor ends.
 */
class AIRPORTMGR_API FUserSettingsLayoutStore : public IUiLayoutStore
{
public:
	virtual TOptional<FUiWindowPlacement> Read(FName Id) const override;
	virtual void Write(FName Id, const FUiWindowPlacement& Placement) override;
	virtual void Clear() override;
	/** Takes Id out again - for a test that wrote a key of its own, never a window. */
	void Remove(FName Id);
};
```

and to `UI/UiLayoutStore.cpp` (include `AirportMgrUserSettings.h`):

```cpp
TOptional<FUiWindowPlacement> FUserSettingsLayoutStore::Read(FName Id) const
{
	const UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get();
	const FUiWindowPlacement* Found = Settings != nullptr ? Settings->WindowLayout.Find(Id) : nullptr;
	return Found != nullptr ? TOptional<FUiWindowPlacement>(*Found) : TOptional<FUiWindowPlacement>();
}

void FUserSettingsLayoutStore::Write(FName Id, const FUiWindowPlacement& Placement)
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->WindowLayout.Add(Id, Placement);
		Settings->SaveSettings();
	}
}

void FUserSettingsLayoutStore::Clear()
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->WindowLayout.Reset();
		Settings->SaveSettings();
	}
}

void FUserSettingsLayoutStore::Remove(FName Id)
{
	if (UAirportMgrUserSettings* Settings = UAirportMgrUserSettings::Get())
	{
		Settings->WindowLayout.Remove(Id);
		Settings->SaveSettings();
	}
}
```

(`UiLayoutStore.h` must not include `AirportMgrUserSettings.h` - the settings header includes it for `FUiWindowPlacement`; the .cpp includes both.)

- [ ] **Step 5: `Config/DefaultEngine.ini`** - in the existing `[/Script/Engine.Engine]` section add:

```ini
; The player's settings object (UI library step 3): UAirportMgrUserSettings keeps the window layout
; beside the engine's own scalability. ENFORCED BY: AirportMgr.Settings.EngineMakesOurUserSettings.
GameUserSettingsClassName=/Script/AirportMgr.AirportMgrUserSettings
```

- [ ] **Step 6: Build (twice for the new test file); run `-Filter AirportMgr.Settings`.** Expected: 2 pass. If `EngineMakesOurUserSettings` fails, the ini line did not take - check the section and the class path; do not weaken the test.

- [ ] **Step 7: Control-check** - comment out the ini line, rebuild not needed (config), run the filter, confirm `EngineMakesOurUserSettings` FAILS; restore.

- [ ] **Step 8: Commit** `git add ... && git commit -m "feat(settings): UAirportMgrUserSettings holds the window layout"`

---

### Task 4: The HUD hands the host the real store, and a rule keeps it there

**Files:**
- Modify: `Source/AirportMgr/BuildHudLayer.cpp`, `Tools/Check-Architecture.ps1` (rule 29)

- [ ] **Step 1: Wire it** - in `UBuildHudLayer::CreateAll`, right after the host is created and added to the viewport:

```cpp
		// THE PLAYER'S FILE, here and only here - every test's host has none (IUiLayoutStore's
		// comment). ENFORCED BY: Check-Architecture rule 29 (layout-store-wired).
		WindowHost->SetLayoutStore(MakeShared<FUserSettingsLayoutStore>());
```

(include `UI/UiLayoutStore.h`.)

- [ ] **Step 2: Rule 29** before `# --- Verdict`:

```powershell
# --- 29. THE GAME'S WINDOW HOST REMEMBERS THE LAYOUT ------------------------------------------
# UUiWindowHost::SetLayoutStore is the seam a window's placement is remembered through, and a host
# never handed a store remembers nothing - which is every test's host, deliberately, so the
# player's ini cannot steer the suite. So nothing but this rule sees the HUD stop wiring it:
# CreateAll needs a local player the headless suite does not have (UI library step 3, 2026-09-28).
$hudLayer = Join-Path $Root 'Source\AirportMgr\BuildHudLayer.cpp'
if (-not (Test-Path $hudLayer)) {
    $failures.Add("layout-store-wired: $hudLayer is named by rule 29 but does not exist - update the rule")
} elseif (-not (Select-String -Path $hudLayer -Pattern 'SetLayoutStore\(MakeShared<FUserSettingsLayoutStore>' -Quiet)) {
    $failures.Add("layout-store-wired: $hudLayer no longer hands the window host an FUserSettingsLayoutStore - the player's window layout would be forgotten every launch")
}
$ranRules.Add('layout-store-wired')
```

- [ ] **Step 3: Run `./Tools/Check-Architecture.ps1`.** Expected PASS listing `layout-store-wired`. Control: comment the `SetLayoutStore` line, rerun, expect FAIL naming rule 29; restore.

- [ ] **Step 4: Build; run `-Filter AirportMgr`.** Expected all pass.

- [ ] **Step 5: Commit** `git commit -am "feat(ui): the HUD's host remembers the player's layout; rule 29 keeps it wired"`

---

### Task 5: Verify and hand over

- [ ] **Step 1: Full suite** (no filter); quote the `N test(s) run` line.
- [ ] **Step 2: PIE on port 8002.** Log: no `GameUserSettings is not a UAirportMgrUserSettings` error. Hand to the user: drag and resize the ledger (B), stop PIE, start PIE, press B - it opens where they left it; log `Window ledger: layout saved at` then, next session, `layout restored from settings`. Confirm the file: `Saved/Config/WindowsEditor/GameUserSettings.ini` has a `WindowLayout` line under `[/Script/AirportMgr.AirportMgrUserSettings]`.
- [ ] **Step 3: No push.**
