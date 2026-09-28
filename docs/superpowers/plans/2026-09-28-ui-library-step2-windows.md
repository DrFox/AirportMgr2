# UI Library Step 2 - Windows: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The inspector, ledger, Land and Offers panels become windows - title bar, close, drag, resize, snap to edges/bar/each other, bring to front - owned by one window host.

**Architecture:** `UUiWindowHost` (one full-screen canvas, Z 1) is the only object that sees every window: placement, z-order, snapping, clamping and the inspector's dock above the bar. `UUiWindow` is the chrome around one panel and translates mouse input into host calls in host-local units. Snapping is pure geometry in `UI/WindowSnap`. Panels keep their logic; they describe their window (`WantsWindow`), ask the host to show/hide them, and are TICKED BY THE HOST, because a collapsed window stops its contents ticking and a panel's tick is what un-hides it.

**Tech Stack:** UE 5.8 C++ (UMG, Slate), PowerShell lint.

**Spec:** `docs/superpowers/specs/2026-09-28-ui-widget-library-design.md` section 2 (Delivery step 2). Persistence of window rectangles is step 3; the modal scrim is step 4 - both out of scope here.

## Global Constraints

- Worktree `C:\repos\airportmgr2-ui-widget-library`, branch `feature/ui-widget-library`. **Do not push or open a PR** (user, 2026-09-28: not until further into the feature).
- Build: `D:\Epic\UE_5.8\Engine\Build\BatchFiles\Build.bat AirportMgrEditor Win64 Development -Project="C:\repos\airportmgr2-ui-widget-library\AirportMgr.uproject" -WaitMutex -NoHotReloadFromIDE` (another checkout's editor is usually open; the flag is safe for a worktree). Close THIS worktree's editor first if one is open (it locks the DLL); kill it by PID only.
- Tests: `./Tools/Run-AirsideTests.ps1 -Project "C:\repos\airportmgr2-ui-widget-library\AirportMgr.uproject" -Filter AirportMgr`; read its `N test(s) run, N failed, N crashed` line. A new test .cpp needs two builds.
- Editor for visual checks: launch with `-ModelContextProtocolPort=8002`, use `AIRSIDE_MCP_PORT=8002` for `Tools/Mcp.py` - another session's editor may own 8000 and would receive your calls.
- Edits via Write/Edit or a Python script written with Write - never `python -c`/heredocs containing `\n` in string literals.
- Spec values (section 2/4): snap distance 12 px (`UUIStyle::SnapDistance`), window min size 180x90 (`UUIStyle::WindowMinSize`), host at Z 1 (bar 0, toasts 2). Windows may not cover the bar: the bar's live height is excluded from the bounds.
- Log category `LogRoadBuild` (game module). Every existing `UE_LOG(` survives - count before and after in touched files.
- Comments explain WHY; `// ENFORCED BY:` beside any claim about other code; a phase is an enum.

## Review Focus

1. **The viewport shrinks after a window was dragged near its far edge** (PIE window resized, resolution change). Expect the window to be pulled back fully on screen on the next tick, not left out of reach. Pinned in Task 5 (`AirportMgr.UI.WindowHost.ReclampsWhenTheViewShrinks`).
2. **A hidden window is still a snap target.** Expect snapping to consider only SHOWN windows - a closed ledger must not leave an invisible edge the inbox sticks to. Pinned in Task 5 (`SnapsOnlyToShownWindows`).
3. **The mouse is released outside the window or capture is lost mid-drag** (alt-tab). Expect the gesture to end; the next mouse move must not keep dragging. Pinned in Task 4 (`AirportMgr.UI.Window.CaptureLostEndsTheGesture`).
4. **A player closes a window its panel keeps asking to show** (the inspector calls SetShown(true) every tick while something is selected). Expect it to stay closed until the panel's own logic hides it or selects something else, then reopen normally. Pinned at the host in Task 4 (`PlayerCloseSticksUntilForgotten`). The inspector's call site (ForgetPlayerClose on a new selection, Task 6 Step 4) is READ, not run: a second selectable agent is a heavy fixture for one line - say so in the commit.
5. **A hosted panel is ticked twice** (the host AND Slate's own NativeTick while visible). Expect exactly one TickPanel per frame. Pinned in Task 3 (`AirportMgr.Panels.HostedPanelTicksOnce`).

---

### Task 1: WindowSnap - the pure geometry

**Files:**
- Create: `Source/AirportMgr/UI/WindowSnap.h`, `UI/WindowSnap.cpp`, `UI/WindowSnapTest.cpp`

**Interfaces:**
- Produces:

```cpp
namespace WindowSnap
{
	/** Top-left for a window of Size dragged to Proposed: clamped inside Bounds, then each axis
	 *  snapped onto the nearest edge within Distance (Bounds' edges, and the edges of every Other
	 *  that lines up with the window on the other axis). */
	AIRPORTMGR_API FVector2D Place(FVector2D Proposed, FVector2D Size, const FBox2D& Bounds,
		TConstArrayView<FBox2D> Others, double Distance);

	/** Size for a window at TopLeft resized to Proposed: at least MinSize, no further than Bounds,
	 *  then its right and bottom edges snapped the same way. */
	AIRPORTMGR_API FVector2D Resize(FVector2D TopLeft, FVector2D Proposed, FVector2D MinSize,
		const FBox2D& Bounds, TConstArrayView<FBox2D> Others, double Distance);
}
```

- [ ] **Step 1: Write the failing tests** `UI/WindowSnapTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "UI/WindowSnap.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace WindowSnapTest
{
	const FBox2D Screen(FVector2D(0.0, 0.0), FVector2D(1920.0, 900.0));   // bar's top at y 900
	const FVector2D Size(200.0, 150.0);
}

/**
 * SNAPPING IS WHAT MAKES DRAGGED WINDOWS LOOK LAID OUT RATHER THAN DROPPED. Each row is one
 * reason: 12 px pulls, 13 does not (the spec's figure, so the edge is exact), the bar's top is an
 * edge like the screen's, a neighbour's edge pulls only when the two actually face each other.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindowSnapPlaceTest, "AirportMgr.UI.WindowSnap.Place",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWindowSnapPlaceTest::RunTest(const FString& Parameters)
{
	using namespace WindowSnapTest;
	const TArray<FBox2D> None;
	struct FRow { FVector2D In; FVector2D Want; const TCHAR* Why; };
	const FRow Rows[] = {
		{ FVector2D(12.0, 300.0),   FVector2D(0.0, 300.0),    TEXT("12 px from the left edge snaps onto it") },
		{ FVector2D(13.0, 300.0),   FVector2D(13.0, 300.0),   TEXT("13 px does not - the distance is exact") },
		{ FVector2D(8.0, 9.0),      FVector2D(0.0, 0.0),      TEXT("a corner snaps on both axes") },
		{ FVector2D(-50.0, 300.0),  FVector2D(0.0, 300.0),    TEXT("dragged off the left is clamped back") },
		{ FVector2D(1800.0, 300.0), FVector2D(1720.0, 300.0), TEXT("dragged off the right is clamped back") },
		{ FVector2D(400.0, 745.0),  FVector2D(400.0, 750.0),  TEXT("the bar's top is an edge: bottom 895 snaps to 900") },
		{ FVector2D(400.0, 800.0),  FVector2D(400.0, 750.0),  TEXT("and a window cannot go under the bar") },
	};
	for (const FRow& R : Rows)
	{
		const FVector2D Got = WindowSnap::Place(R.In, Size, Screen, None, 12.0);
		TestEqual(R.Why, Got, R.Want);
	}

	// NEIGHBOURS: the other window is at x 300..500, y 100..300.
	const TArray<FBox2D> Other = { FBox2D(FVector2D(300.0, 100.0), FVector2D(500.0, 300.0)) };
	TestEqual(TEXT("beside a neighbour it snaps flush to its right edge"),
		WindowSnap::Place(FVector2D(508.0, 120.0), Size, Screen, Other, 12.0), FVector2D(500.0, 120.0));
	TestEqual(TEXT("a neighbour far below on the other axis does not pull sideways"),
		WindowSnap::Place(FVector2D(508.0, 500.0), Size, Screen, Other, 12.0), FVector2D(508.0, 500.0));
	TestEqual(TEXT("under a neighbour it snaps flush to its bottom edge (x is 20 off both its edges: no pull)"),
		WindowSnap::Place(FVector2D(320.0, 309.0), Size, Screen, Other, 12.0), FVector2D(320.0, 300.0));
	return true;
}

/** RESIZE SNAPS THE EDGES THAT MOVE and never below the floor the scroll box relies on. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWindowSnapResizeTest, "AirportMgr.UI.WindowSnap.Resize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWindowSnapResizeTest::RunTest(const FString& Parameters)
{
	using namespace WindowSnapTest;
	const TArray<FBox2D> None;
	const FVector2D Min(180.0, 90.0);
	TestEqual(TEXT("never below the minimum"),
		WindowSnap::Resize(FVector2D(100.0, 100.0), FVector2D(20.0, 10.0), Min, Screen, None, 12.0), FVector2D(180.0, 90.0));
	TestEqual(TEXT("never past the screen or the bar"),
		WindowSnap::Resize(FVector2D(1800.0, 800.0), FVector2D(500.0, 500.0), Min, Screen, None, 12.0), FVector2D(180.0, 100.0));
	TestEqual(TEXT("the right edge snaps onto the screen's right"),
		WindowSnap::Resize(FVector2D(1500.0, 100.0), FVector2D(410.0, 200.0), Min, Screen, None, 12.0), FVector2D(420.0, 200.0));
	return true;
}

#endif
```

Note the "never past the screen" row: at (1800,800) the room left is 120x100, below the 180 minimum on X - the minimum wins (a window narrower than its floor is worse than one that overhangs until moved), so the expected X is 180 and Y is 100.

- [ ] **Step 2: Build twice; expect compile failure** (`UI/WindowSnap.h` not found).

- [ ] **Step 3: Write `UI/WindowSnap.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"

/**
 * Window placement geometry for UUiWindowHost - plain functions over FBox2D, no widgets, so every
 * rule is table-tested (UI library step 2, spec 2026-09-28 section 2). All values are in the
 * host's local units, which is what its canvas slots are positioned in.
 */
namespace WindowSnap
{
	/**
	 * Top-left for a window of Size dragged to Proposed: clamped inside Bounds, then each axis
	 * snapped onto the nearest edge within Distance - Bounds' own edges, and the edges of every
	 * Other that FACES the window (overlaps it, give or take Distance, on the other axis). A
	 * neighbour far above must not pull a window sideways onto a line nobody can see connecting them.
	 */
	AIRPORTMGR_API FVector2D Place(FVector2D Proposed, FVector2D Size, const FBox2D& Bounds,
		TConstArrayView<FBox2D> Others, double Distance);

	/**
	 * Size for a window at TopLeft resized to Proposed: at least MinSize, no further than Bounds
	 * (the minimum wins where they disagree), then its right and bottom edges snapped the same way.
	 */
	AIRPORTMGR_API FVector2D Resize(FVector2D TopLeft, FVector2D Proposed, FVector2D MinSize,
		const FBox2D& Bounds, TConstArrayView<FBox2D> Others, double Distance);
}
```

- [ ] **Step 4: Write `UI/WindowSnap.cpp`:**

```cpp
#include "UI/WindowSnap.h"

namespace
{
	/** The smallest signed shift, at most Distance, that lands one of Edges on one of Targets; 0 if none. */
	double Shift(std::initializer_list<double> Edges, const TArray<double>& Targets, double Distance)
	{
		double Best = 0.0;
		double BestAbs = TNumericLimits<double>::Max();
		for (const double T : Targets)
		{
			for (const double E : Edges)
			{
				const double D = T - E;
				if (FMath::Abs(D) <= Distance && FMath::Abs(D) < BestAbs)
				{
					Best = D;
					BestAbs = FMath::Abs(D);
				}
			}
		}
		return Best;
	}

	/** Bounds' edges plus the edges of every Other that faces a window at TopLeft/Size. */
	void Targets(FVector2D TopLeft, FVector2D Size, const FBox2D& Bounds, TConstArrayView<FBox2D> Others,
		double Distance, TArray<double>& OutX, TArray<double>& OutY)
	{
		OutX = { Bounds.Min.X, Bounds.Max.X };
		OutY = { Bounds.Min.Y, Bounds.Max.Y };
		for (const FBox2D& O : Others)
		{
			const bool bRowsMeet = O.Min.Y <= TopLeft.Y + Size.Y + Distance && O.Max.Y >= TopLeft.Y - Distance;
			const bool bColumnsMeet = O.Min.X <= TopLeft.X + Size.X + Distance && O.Max.X >= TopLeft.X - Distance;
			if (bRowsMeet) { OutX.Add(O.Min.X); OutX.Add(O.Max.X); }
			if (bColumnsMeet) { OutY.Add(O.Min.Y); OutY.Add(O.Max.Y); }
		}
	}

	FVector2D Clamp(FVector2D P, FVector2D Size, const FBox2D& Bounds)
	{
		return FVector2D(
			FMath::Clamp(P.X, Bounds.Min.X, FMath::Max(Bounds.Min.X, Bounds.Max.X - Size.X)),
			FMath::Clamp(P.Y, Bounds.Min.Y, FMath::Max(Bounds.Min.Y, Bounds.Max.Y - Size.Y)));
	}
}

FVector2D WindowSnap::Place(FVector2D Proposed, FVector2D Size, const FBox2D& Bounds,
	TConstArrayView<FBox2D> Others, double Distance)
{
	FVector2D P = Clamp(Proposed, Size, Bounds);
	TArray<double> Xs, Ys;
	Targets(P, Size, Bounds, Others, Distance, Xs, Ys);
	P.X += Shift({ P.X, P.X + Size.X }, Xs, Distance);
	P.Y += Shift({ P.Y, P.Y + Size.Y }, Ys, Distance);
	// Clamped again: a neighbour's edge can sit outside the bounds after the viewport shrank.
	return Clamp(P, Size, Bounds);
}

FVector2D WindowSnap::Resize(FVector2D TopLeft, FVector2D Proposed, FVector2D MinSize,
	const FBox2D& Bounds, TConstArrayView<FBox2D> Others, double Distance)
{
	auto Fit = [&](FVector2D S)
	{
		return FVector2D(
			FMath::Max(MinSize.X, FMath::Min(S.X, Bounds.Max.X - TopLeft.X)),
			FMath::Max(MinSize.Y, FMath::Min(S.Y, Bounds.Max.Y - TopLeft.Y)));
	};
	FVector2D S = Fit(Proposed);
	TArray<double> Xs, Ys;
	Targets(TopLeft, S, Bounds, Others, Distance, Xs, Ys);
	S.X += Shift({ TopLeft.X + S.X }, Xs, Distance);
	S.Y += Shift({ TopLeft.Y + S.Y }, Ys, Distance);
	return Fit(S);
}
```

- [ ] **Step 5: Build, run `-Filter AirportMgr.UI.WindowSnap`.** Expected: 2 tests pass. If a row fails, recompute it by hand before touching the code - the rows are the spec here.

- [ ] **Step 6: Commit** `git add Source/AirportMgr/UI && git commit -m "feat(ui): WindowSnap - clamp and snap as plain geometry"`

---

### Task 2: Share the click-eating rule

The bar still eats its own chrome clicks through `UAirportMgrPanelWidget`; `UUiWindow` (Task 4) must eat the same way. One function, two callers.

**Files:**
- Create: `Source/AirportMgr/UI/UiClicks.h`, `UI/UiClicks.cpp`
- Modify: `Source/AirportMgr/AirportMgrPanelWidget.cpp` (the `AirportMgrPanelClicks` namespace moves out)

**Interfaces:**
- Produces: `FReply UiClicks::EatUnhandled(const UUserWidget& Widget, const UWidget* Root, FReply Reply, const TCHAR* What);`

- [ ] **Step 1: Confirm the existing test is green** - `-Filter AirportMgr.Panels.ChromeEatsClicks` passes before the move (it is the test for this refactor; no new test - the seam is unchanged at its composition).

- [ ] **Step 2: Create `UI/UiClicks.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"

class UUserWidget;
class UWidget;

namespace UiClicks
{
	/**
	 * Handled when the press can only have come from Widget's own pixels (a Border is hit-testable
	 * but handles nothing, so the press would bubble on to the game viewport as a click on the
	 * ground). A Widget or Root that hits ITSELF may cover the screen, so Slate's answer stands for
	 * those - better a click through than every click in the game swallowed. Logs at the boundary.
	 * Shared by UAirportMgrPanelWidget (the bar) and UUiWindow. ENFORCED BY: AirportMgr.Panels.ChromeEatsClicks,
	 * AirportMgr.UI.Window.ChromeEatsClicks.
	 */
	AIRPORTMGR_API FReply EatUnhandled(const UUserWidget& Widget, const UWidget* Root, FReply Reply, const TCHAR* What);
}
```

- [ ] **Step 3: Create `UI/UiClicks.cpp`** by MOVING `AirportMgrPanelClicks::HitsItself` and `Eat` out of `AirportMgrPanelWidget.cpp` verbatim (comments included), renamed:

```cpp
#include "UI/UiClicks.h"

#include "Blueprint/UserWidget.h"
#include "RoadBuildLog.h"

namespace
{
	/** Whether W's OWN rectangle takes hits - Visible - as opposed to only its children's. */
	bool HitsItself(const UWidget* W)
	{
		return W != nullptr && W->GetVisibility() == ESlateVisibility::Visible;
	}
}

FReply UiClicks::EatUnhandled(const UUserWidget& Widget, const UWidget* Root, FReply Reply, const TCHAR* What)
{
	if (Reply.IsEventHandled())
	{
		return Reply;   // a button (or a Blueprint) took it - its own answer stands
	}
	if (HitsItself(&Widget) || HitsItself(Root))
	{
		return Reply;
	}
	// AT THE BOUNDARY, so "a click on the bar drew a road" is answerable from the log: this
	// line present means the panel stopped it, absent means it never reached the panel.
	UE_LOG(LogRoadBuild, Log, TEXT("%s: mouse %s stopped at the panel"), *Widget.GetName(), What);
	return FReply::Handled();
}
```

In `AirportMgrPanelWidget.cpp`, delete the `namespace AirportMgrPanelClicks { ... }` block (keep nothing of it - it moved), include `UI/UiClicks.h`, and change both callers from `AirportMgrPanelClicks::Eat(*this, ...)` to `UiClicks::EatUnhandled(*this, ...)`.

- [ ] **Step 4: Build; run `-Filter AirportMgr.Panels`.** Expected: `ChromeEatsClicks` and `SharedBaseBuildsOnce` pass; `grep -c "UE_LOG(" Source/AirportMgr/AirportMgrPanelWidget.cpp Source/AirportMgr/UI/UiClicks.cpp` sums to the old count.

- [ ] **Step 5: Commit** `git commit -am "refactor(ui): the click-eating rule moves to UiClicks for the window to share"` (add the two new files first).

---

### Task 3: The panel's side of hosting

Adds the panel base's hosting API. No panel uses it yet (Task 6 migrates them), so behaviour is unchanged; `EnsureCardRoot`/`SetCardShown` stay until Task 6.

**Files:**
- Create: `Source/AirportMgr/UI/UiWindowSpec.h`
- Modify: `Source/AirportMgr/AirportMgrPanelWidget.h/.cpp`, `UIStyle.h` (two metrics)
- Test: `Source/AirportMgr/AirportMgrPanelWidgetTest.cpp`

**Interfaces:**
- Produces:

```cpp
UENUM() enum class EUiWindowAnchor : uint8 { TopLeft, TopRight, AboveBarLeft };
USTRUCT() struct FUiWindowSpec {
	FName Id; FText Title; bool bClosable = true; bool bResizable = true;
	EUiWindowAnchor Anchor = EUiWindowAnchor::TopLeft; FVector2D Offset = FVector2D(12.0, 12.0);
};
// UAirportMgrPanelWidget, public:
virtual bool WantsWindow(FUiWindowSpec& Out) const;     // false: not a window (bar, toasts)
void AttachToHost(UUiWindowHost& InHost, FName InId);   // host calls once, from AddWindow
void RunPanelTick(float DeltaTime);                     // host calls every frame
virtual void OnWindowClosedByPlayer();                  // default: nothing
bool IsShown() const;
int32 PanelTickCountForTest() const;
// protected:
virtual void TickPanel(float DeltaTime);                // default: nothing
void SetShown(bool bShown);
void ForgetPlayerClose();
// UUIStyle: float SnapDistance = 12.0f; FVector2D WindowMinSize = FVector2D(180.0, 90.0);
```

- [ ] **Step 1: Write the failing test** - append to `AirportMgrPanelWidgetTest.cpp`:

```cpp
/**
 * A HOSTED PANEL TICKS ONCE A FRAME, FROM THE HOST. A collapsed window stops Slate ticking its
 * contents, and a panel's tick is what un-hides it (the inspector showing a new selection), so
 * the host ticks every hosted panel itself - which means the panel's OWN NativeTick must then do
 * nothing, or a visible panel is ticked twice. Without a host (the bar, toasts, headless tests)
 * NativeTick keeps doing the work.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAirportMgrPanelHostedTicksOnceTest,
	"AirportMgr.Panels.HostedPanelTicksOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FAirportMgrPanelHostedTicksOnceTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	UInspectorWidget* Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("a panel"), Panel)) { return false; }

	Panel->NativeTickForTest(0.016f);
	TestEqual(TEXT("no host: its own NativeTick runs the panel tick"), Panel->PanelTickCountForTest(), 1);
	Panel->RunPanelTick(0.016f);
	TestEqual(TEXT("RunPanelTick is the one entry"), Panel->PanelTickCountForTest(), 2);
	return true;
}
```

(The hosted half - NativeTick does nothing once attached - is asserted in Task 4, where a host exists.) Add to `UAirportMgrPanelWidget`'s public test seams: `void NativeTickForTest(float DeltaTime) { FGeometry G; NativeTick(G, DeltaTime); }` ONLY if no subclass already declares one with that name - `UBuildBarWidget` and `UOfferInboxWidget` do; move theirs up to the base (delete the two copies, keep their comments on the base's).

- [ ] **Step 2: Build; expect compile failure** (`PanelTickCountForTest`, `RunPanelTick`).

- [ ] **Step 3: Create `UI/UiWindowSpec.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "UiWindowSpec.generated.h"

/** Where a window sits until the player moves it. */
UENUM()
enum class EUiWindowAnchor : uint8
{
	TopLeft,
	TopRight,
	AboveBarLeft,   // bottom-left, riding the bar's top edge as the bar grows (the inspector)
};

/**
 * How a panel wants its window (UAirportMgrPanelWidget::WantsWindow). Defaults are the panel's
 * - its placement lived in its own TopOffset/BarGap properties before windows, and still does.
 */
USTRUCT()
struct FUiWindowSpec
{
	GENERATED_BODY()

	UPROPERTY() FName Id;
	UPROPERTY() FText Title;
	UPROPERTY() bool bClosable = true;
	UPROPERTY() bool bResizable = true;
	UPROPERTY() EUiWindowAnchor Anchor = EUiWindowAnchor::TopLeft;
	/** From the anchored corner, inward; for AboveBarLeft, Y is the gap above the bar's top. */
	UPROPERTY() FVector2D Offset = FVector2D(12.0, 12.0);
};
```

- [ ] **Step 4: Add the two metrics to `UIStyle.h`** after `ControlRadius`:

```cpp
	/** A dragged window's edge within this many uu of a screen edge, the bar's top or another
	 *  window's edge snaps onto it (spec 2026-09-28 section 2). */
	UPROPERTY(EditAnywhere, Category = "Metrics", meta = (ClampMin = "0.0")) float SnapDistance = 12.0f;

	/** The smallest a player can resize a window to; its scroll box takes whatever no longer fits. */
	UPROPERTY(EditAnywhere, Category = "Metrics") FVector2D WindowMinSize = FVector2D(180.0, 90.0);
```

- [ ] **Step 5: Implement the panel base additions.** In `AirportMgrPanelWidget.h`: forward-declare `class UUiWindowHost;`, `#include "UI/UiWindowSpec.h"`, add the public/protected members from **Interfaces** with these doc comments, and in `private:`

```cpp
	/** The host that owns this panel's window; null for the bar, toasts and a headless test. */
	UPROPERTY() TObjectPtr<UUiWindowHost> Host;
	FName WindowId;
	/** What the panel last asked for - applied to the host when it attaches, and read by IsShown
	 *  when there is no host. */
	bool bShownRequested = false;
	int32 PanelTicks = 0;
```

and declare `virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;` in `protected:`. In the .cpp:

```cpp
bool UAirportMgrPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	return false;
}

void UAirportMgrPanelWidget::AttachToHost(UUiWindowHost& InHost, FName InId)
{
	Host = &InHost;
	WindowId = InId;
	InHost.SetShown(WindowId, bShownRequested);   // whatever BuildOnce already asked for
}

void UAirportMgrPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// HOSTED PANELS ARE TICKED BY THE HOST, hidden or not - see RunPanelTick. Doing it here too
	// would tick a visible hosted panel twice. ENFORCED BY: AirportMgr.UI.WindowHost.TicksHiddenPanelsOnce.
	if (Host == nullptr)
	{
		RunPanelTick(InDeltaTime);
	}
}

void UAirportMgrPanelWidget::RunPanelTick(float DeltaTime)
{
	++PanelTicks;
	TickPanel(DeltaTime);
}

void UAirportMgrPanelWidget::TickPanel(float DeltaTime)
{
}

void UAirportMgrPanelWidget::OnWindowClosedByPlayer()
{
}

void UAirportMgrPanelWidget::SetShown(bool bShown)
{
	bShownRequested = bShown;
	if (Host != nullptr)
	{
		Host->SetShown(WindowId, bShown);
	}
}

bool UAirportMgrPanelWidget::IsShown() const
{
	return Host != nullptr ? Host->IsShown(WindowId) : bShownRequested;
}

void UAirportMgrPanelWidget::ForgetPlayerClose()
{
	if (Host != nullptr)
	{
		Host->ForgetDismissal(WindowId);
	}
}
```

`PanelTickCountForTest() const { return PanelTicks; }` inline in the header. This needs `UUiWindowHost` to exist to compile - so create a MINIMAL `UI/UiWindowHost.h/.cpp` now with only `SetShown(FName, bool)`, `IsShown(FName) const`, `ForgetDismissal(FName)` as empty/false bodies; Task 4 fills it in. (Honest scaffolding, not a stub left behind: Task 4's first test fails against exactly these bodies.)

```cpp
// UI/UiWindowHost.h (Task 3 scaffold)
#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UiWindowHost.generated.h"

UCLASS()
class AIRPORTMGR_API UUiWindowHost : public UUserWidget
{
	GENERATED_BODY()
public:
	void SetShown(FName Id, bool bShown);
	bool IsShown(FName Id) const;
	void ForgetDismissal(FName Id);
};

// UI/UiWindowHost.cpp (Task 3 scaffold)
#include "UI/UiWindowHost.h"
void UUiWindowHost::SetShown(FName Id, bool bShown) {}
bool UUiWindowHost::IsShown(FName Id) const { return false; }
void UUiWindowHost::ForgetDismissal(FName Id) {}
```

- [ ] **Step 6: Build; run `-Filter AirportMgr`.** Expected: all pass including `HostedPanelTicksOnce`. The four panels still override `NativeTick` and call `Super::NativeTick` - they now also get a no-op `TickPanel`; nothing else changes.

- [ ] **Step 7: Commit** `git commit -am "feat(ui): the panel base's hosting API - WantsWindow, SetShown, RunPanelTick"` (add new files).

---

### Task 4: UUiWindow and the host - show, hide, close, front, tick

**Files:**
- Create: `Source/AirportMgr/UI/UiWindow.h`, `UI/UiWindow.cpp`, `UI/UiWindowHostTest.cpp`
- Modify: `Source/AirportMgr/UI/UiWindowHost.h/.cpp` (replace the scaffold)
- Modify: `Source/AirportMgr/LedgerPanelWidget.h/.cpp` (`WantsWindow` override - the ledger is the test panel here)

**Interfaces:**
- Consumes: `FUiWindowSpec`, panel hosting API (Task 3), `UUiButton` (step 1), `UiClicks::EatUnhandled` (Task 2).
- Produces:

```cpp
UENUM() enum class EUiWindowGesture : uint8 { None, Move, Resize };
class UUiWindow : public UUserWidget {
	void Build(const UUIStyle& Style, const FUiWindowSpec& Spec, UWidget& Content, UUiWindowHost& Host);
	FName GetId() const;
	UFUNCTION() void HandleClose();
	EUiWindowGesture GestureForTest() const;
	void BeginGestureForTest(EUiWindowGesture Kind, FVector2D HostLocal);
	void MoveGestureForTest(FVector2D HostLocal);
};
class UUiWindowHost : public UUserWidget {
	UUiWindow* AddWindow(UAirportMgrPanelWidget& Panel);  // null if the panel WantsWindow false
	void SetShown(FName Id, bool bShown); bool IsShown(FName Id) const;
	void ForgetDismissal(FName Id); void CloseByPlayer(FName Id); void BringToFront(FName Id);
	void DockAbove(const UBuildBarWidget* Bar);
	void MoveWindow(FName Id, FVector2D ProposedTopLeft);   // Task 5
	void ResizeWindow(FName Id, FVector2D ProposedSize);    // Task 5
	FBox2D WindowRect(FName Id) const;                      // host-local
	FVector2D ToLocal(FVector2D ScreenPosition) const;
	void TickForTest(float DeltaTime);
	void SetViewSizeForTest(FVector2D Size);
	UUiWindow* WindowForTest(FName Id) const;
	int32 ZOrderForTest(FName Id) const;
	double WindowClearanceForTest(FName Id) const;          // Task 5
	const UBuildBarWidget* DockedBarForTest() const;
};
```

- [ ] **Step 1: Give the ledger a window.** In `LedgerPanelWidget.h` public: `virtual bool WantsWindow(FUiWindowSpec& Out) const override;` and protected `virtual void OnWindowClosedByPlayer() override;`. In the .cpp:

```cpp
bool ULedgerPanelWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("ledger");
	Out.Title = LOCTEXT("LedgerWindow", "Ledger");
	Out.Anchor = EUiWindowAnchor::TopRight;
	Out.Offset = FVector2D(12.0, TopOffset);
	return true;
}

void ULedgerPanelWidget::OnWindowClosedByPlayer()
{
	// THE CLOSE BUTTON IS THE TOGGLE: bShowing must agree, or the next B "opens" it hidden and the
	// bar lights a panel nobody can see.
	if (bShowing)
	{
		Toggle();
	}
}
```

- [ ] **Step 2: Write the failing tests** `UI/UiWindowHostTest.cpp`:

```cpp
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "LedgerPanelWidget.h"
#include "Misc/AutomationTest.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiButton.h"
#include "UI/UiWindow.h"
#include "UI/UiWindowHost.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UiWindowHostTest
{
	/** A host with the ledger in it - the panel with the simplest show/hide (a toggle). */
	struct FFixture
	{
		FAirsideTestWorld TestWorld{ /*bSpawnActor=*/false };
		UUiWindowHost* Host = nullptr;
		ULedgerPanelWidget* Ledger = nullptr;
		UUiWindow* Window = nullptr;
		FFixture()
		{
			Host = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
			Ledger = CreateWidget<ULedgerPanelWidget>(TestWorld.World, ULedgerPanelWidget::StaticClass());
			if (Host != nullptr && Ledger != nullptr)
			{
				Host->SetViewSizeForTest(FVector2D(1920.0, 1080.0));
				Window = Host->AddWindow(*Ledger);
			}
		}
	};
}

/**
 * THE HOST OWNS SHOWING. A panel asks; the host shows or collapses the window around it. The
 * ledger starts hidden (BuildOnce asks for hidden) and its toggle is what opens it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowHostShowTest, "AirportMgr.UI.WindowHost.ShowsWhatThePanelAsks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowHostShowTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window around the ledger"), F.Window)) { return false; }
	TestFalse(TEXT("hidden until asked"), F.Host->IsShown(TEXT("ledger")));
	TestEqual(TEXT("and its window is collapsed"), F.Window->GetVisibility(), ESlateVisibility::Collapsed);
	F.Ledger->Toggle();
	TestTrue(TEXT("the toggle shows it"), F.Host->IsShown(TEXT("ledger")));
	TestNotEqual(TEXT("and its window is not collapsed"), F.Window->GetVisibility(), ESlateVisibility::Collapsed);
	TestTrue(TEXT("the panel reads the host's answer"), F.Ledger->IsShown());
	return true;
}

/**
 * THE CLOSE BUTTON IS THE PANEL'S OWN TOGGLE for a toggled panel - closing must leave bShowing
 * false, or the bar keeps lighting a ledger nobody can see.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowCloseTest, "AirportMgr.UI.WindowHost.CloseUntogglesThePanel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowCloseTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	F.Ledger->Toggle();
	F.Window->HandleClose();
	TestFalse(TEXT("closed"), F.Host->IsShown(TEXT("ledger")));
	TestFalse(TEXT("and the ledger agrees it is not showing"), F.Ledger->IsShowing());
	F.Ledger->Toggle();
	TestTrue(TEXT("the next toggle opens it again"), F.Host->IsShown(TEXT("ledger")));
	return true;
}

/**
 * A PLAYER'S CLOSE STICKS while the panel keeps asking to show (the inspector asks every tick
 * while anything is selected), until the panel hides it itself or forgets the close.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowStickyCloseTest, "AirportMgr.UI.WindowHost.PlayerCloseSticksUntilForgotten",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowStickyCloseTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const FName Id(TEXT("ledger"));
	F.Host->SetShown(Id, true);
	F.Host->CloseByPlayer(Id);
	F.Host->SetShown(Id, true);
	TestFalse(TEXT("asking again does not undo the player's close"), F.Host->IsShown(Id));
	F.Host->ForgetDismissal(Id);
	F.Host->SetShown(Id, true);
	TestTrue(TEXT("forgotten, it shows"), F.Host->IsShown(Id));
	F.Host->CloseByPlayer(Id);
	F.Host->SetShown(Id, false);
	F.Host->SetShown(Id, true);
	TestTrue(TEXT("the panel hiding it itself also clears the close"), F.Host->IsShown(Id));
	return true;
}

/** A CLICK RAISES A WINDOW above the others - the ledger and inbox share the top-right corner. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowFrontTest, "AirportMgr.UI.WindowHost.BringToFrontRaises",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowFrontTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const int32 Before = F.Host->ZOrderForTest(TEXT("ledger"));
	F.Host->BringToFront(TEXT("ledger"));
	TestTrue(TEXT("raised"), F.Host->ZOrderForTest(TEXT("ledger")) > Before);
	return true;
}

/**
 * HIDDEN PANELS ARE STILL TICKED, ONCE. The host ticks every hosted panel (a collapsed window
 * stops Slate ticking it, and the tick is what re-shows it); the panel's own NativeTick then
 * does nothing, so a shown panel is not ticked twice. Review Focus 5.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowTickTest, "AirportMgr.UI.WindowHost.TicksHiddenPanelsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowTickTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const int32 Start = F.Ledger->PanelTickCountForTest();
	F.Host->TickForTest(0.016f);
	TestEqual(TEXT("a hidden panel is ticked by the host"), F.Ledger->PanelTickCountForTest(), Start + 1);
	F.Ledger->NativeTickForTest(0.016f);
	TestEqual(TEXT("its own NativeTick adds nothing once hosted"), F.Ledger->PanelTickCountForTest(), Start + 1);
	return true;
}

/**
 * CAPTURE LOST ENDS THE GESTURE (alt-tab mid-drag, a release outside the window). Otherwise the
 * next mouse move keeps dragging a window the player let go of. Review Focus 3.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowCaptureLostTest, "AirportMgr.UI.Window.CaptureLostEndsTheGesture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowCaptureLostTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	F.Ledger->Toggle();
	F.Window->BeginGestureForTest(EUiWindowGesture::Move, FVector2D(100.0, 100.0));
	TestEqual(TEXT("dragging"), F.Window->GestureForTest(), EUiWindowGesture::Move);
	F.Window->TakeWidget()->OnMouseCaptureLost(FCaptureLostEvent(0, 0));
	TestEqual(TEXT("capture lost: no longer dragging"), F.Window->GestureForTest(), EUiWindowGesture::None);
	return true;
}

/** THE WINDOW'S CHROME EATS PRESSES, the bar's rule (UiClicks) - a press on a title never reaches the ground. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowEatsClicksTest, "AirportMgr.UI.Window.ChromeEatsClicks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowEatsClicksTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const TSharedRef<SWidget> Slate = F.Window->TakeWidget();
	TestTrue(TEXT("a press on the chrome is handled"), Slate->OnMouseButtonDown(FGeometry(), FPointerEvent()).IsEventHandled());
	TestFalse(TEXT("the release is left alone"), Slate->OnMouseButtonUp(FGeometry(), FPointerEvent()).IsEventHandled());
	return true;
}

#endif
```

`FCaptureLostEvent(uint32 UserIndex, int32 PointerIndex)`: confirm the constructor with `grep -n "FCaptureLostEvent(" D:/Epic/UE_5.8/Engine/Source/Runtime/SlateCore/Public/Input/Events.h` and adjust the arguments to match.

- [ ] **Step 3: Build twice; expect compile failure** (`UI/UiWindow.h`, `AddWindow` etc.).

- [ ] **Step 4: Write `UI/UiWindow.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiWindowSpec.h"
#include "UiWindow.generated.h"

class UUiButton;
class UUiWindowHost;
class UUIStyle;

/** What a held left button is doing to a window. A phase, not two bools. */
UENUM()
enum class EUiWindowGesture : uint8
{
	None,
	Move,
	Resize,
};

/**
 * The chrome around one panel (UI library step 2): shadow, white card, title bar with an optional
 * close, hairline, a scroll body holding the panel, and a resize grip. It turns mouse input into
 * HOST calls in host-local units and decides nothing about geometry itself - clamping, snapping,
 * z-order and docking are UUiWindowHost's, because each of them needs every window at once.
 */
UCLASS()
class AIRPORTMGR_API UUiWindow : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Builds the chrome around Content. Once, from UUiWindowHost::AddWindow. */
	void Build(const UUIStyle& Style, const FUiWindowSpec& Spec, UWidget& Content, UUiWindowHost& InHost);

	FName GetId() const { return Id; }

	/** The close button. Public for the test that presses it. */
	UFUNCTION() void HandleClose();

	EUiWindowGesture GestureForTest() const { return Gesture; }
	void BeginGestureForTest(EUiWindowGesture Kind, FVector2D HostLocal) { BeginGesture(Kind, HostLocal); }
	void MoveGestureForTest(FVector2D HostLocal) { UpdateGesture(HostLocal); }

protected:
	/** Any press on the window raises it - in the PREVIEW pass, because a press on the panel's
	 *  content is handled by the panel before it could bubble up to here. */
	virtual FReply NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;

private:
	void BeginGesture(EUiWindowGesture Kind, FVector2D HostLocal);
	void UpdateGesture(FVector2D HostLocal);
	void EndGesture(const TCHAR* Why);

	UPROPERTY() TObjectPtr<UUiWindowHost> Host;
	UPROPERTY() TObjectPtr<UWidget> TitleBar;
	UPROPERTY() TObjectPtr<UWidget> Grip;
	UPROPERTY() TObjectPtr<UUiButton> CloseButton;
	FName Id;
	EUiWindowGesture Gesture = EUiWindowGesture::None;
	FVector2D GestureStartMouse = FVector2D::ZeroVector;
	FVector2D GestureStartTopLeft = FVector2D::ZeroVector;
	FVector2D GestureStartSize = FVector2D::ZeroVector;
};
```

- [ ] **Step 5: Write `UI/UiWindow.cpp`:**

```cpp
#include "UI/UiWindow.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "RoadBuildLog.h"
#include "UI/UiButton.h"
#include "UI/UiClicks.h"
#include "UI/UiWindowHost.h"
#include "UIStyle.h"

void UUiWindow::Build(const UUIStyle& Style, const FUiWindowSpec& Spec, UWidget& Content, UUiWindowHost& InHost)
{
	Host = &InHost;
	Id = Spec.Id;

	// THE OUTER FRAME IS A SHADOW - a translucent rounded box one pixel wider on three sides and
	// three below, so a white card lifts off grass and concrete alike. Slate has no drop shadow;
	// padding on a darker box is the cheapest thing that reads as one (the spike's recipe).
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("WindowFrame"));
	Frame->SetBrush(FSlateRoundedBoxBrush(Style.Shadow, Style.WindowRadius + 1.0f));
	Frame->SetPadding(FMargin(1.0f, 1.0f, 1.0f, 3.0f));
	WidgetTree->RootWidget = Frame;

	UBorder* Card = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("WindowCard"));
	Card->SetBrush(FSlateRoundedBoxBrush(Style.Surface, Style.WindowRadius));
	Card->SetPadding(FMargin(0.0f));
	Card->SetClipping(EWidgetClipping::ClipToBounds);
	Frame->SetContent(Card);

	UOverlay* Layers = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	Card->SetContent(Layers);
	UVerticalBox* Chrome = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UOverlaySlot* ChromeSlot = Layers->AddChildToOverlay(Chrome);
	ChromeSlot->SetHorizontalAlignment(HAlign_Fill);
	ChromeSlot->SetVerticalAlignment(VAlign_Fill);

	// VISIBLE with a transparent brush: a Border hit-tests but handles nothing, so a press on the
	// title bubbles to NativeOnMouseButtonDown, which starts the drag. The close button handles
	// its own press first and never gets there.
	UBorder* Bar = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("WindowTitleBar"));
	Bar->SetBrushColor(FLinearColor::Transparent);
	Bar->SetPadding(FMargin(Style.CardPadding.Left, 6.0f, 6.0f, 6.0f));
	Bar->SetVisibility(ESlateVisibility::Visible);
	Chrome->AddChildToVerticalBox(Bar);
	TitleBar = Bar;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Bar->SetContent(Row);
	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("WindowTitle"));
	Title->SetText(Spec.Title);
	Style.ApplyText(*Title, EUITextRole::Title, Style.Ink);
	UHorizontalBoxSlot* TitleSlot = Row->AddChildToHorizontalBox(Title);
	TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TitleSlot->SetVerticalAlignment(VAlign_Center);

	if (Spec.bClosable)
	{
		// A Ghost UUiButton: no fill until hovered, so the close reads as an affordance rather
		// than a second button competing with the panel's own verbs.
		CloseButton = WidgetTree->ConstructWidget<UUiButton>(UUiButton::StaticClass(), TEXT("WindowClose"));
		CloseButton->SetLabel(FText::FromString(FString(TEXT("\u00D7"))));
		CloseButton->Build(Style, EUiButtonKind::Ghost, EUiButtonLayout::Inline, false);
		CloseButton->OnClicked.AddDynamic(this, &UUiWindow::HandleClose);
		UHorizontalBoxSlot* CloseSlot = Row->AddChildToHorizontalBox(CloseButton);
		CloseSlot->SetVerticalAlignment(VAlign_Center);
		CloseSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
	}

	// The hairline under the title. A SizeBox, not UImage::SetDesiredSizeOverride, which drew a
	// 15 px band in the spike: the height has to be imposed from outside the brush.
	USizeBox* Rule = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("WindowRule"));
	Rule->SetHeightOverride(1.0f);
	UBorder* RuleFill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	RuleFill->SetBrushColor(Style.Rule);
	RuleFill->SetPadding(FMargin(0.0f));
	Rule->SetContent(RuleFill);
	Chrome->AddChildToVerticalBox(Rule)->SetHorizontalAlignment(HAlign_Fill);

	// A SCROLL BOX, so a window resized smaller than its panel scrolls instead of cropping.
	// Auto-sized, it is exactly the panel's size.
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("WindowScroll"));
	Scroll->SetScrollBarVisibility(ESlateVisibility::Collapsed);
	Chrome->AddChildToVerticalBox(Scroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UBorder* Pad = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Pad->SetBrushColor(FLinearColor::Transparent);
	// UUIStyle::CardPadding, not a literal: see its own comment (issue #192).
	Pad->SetPadding(Style.CardPadding);
	Pad->SetContent(&Content);
	Scroll->AddChild(Pad);

	if (Spec.bResizable)
	{
		// The grip: a small muted square in the corner, over the content. Visible so it
		// hit-tests; NativeOnMouseButtonDown checks it before the title bar.
		UImage* GripImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("WindowGrip"));
		GripImage->SetBrush(FSlateRoundedBoxBrush(Style.InkMuted * FLinearColor(1.0f, 1.0f, 1.0f, 0.45f), 2.0f));
		GripImage->SetDesiredSizeOverride(FVector2D(9.0, 9.0));
		GripImage->SetVisibility(ESlateVisibility::Visible);
		UOverlaySlot* GripSlot = Layers->AddChildToOverlay(GripImage);
		GripSlot->SetHorizontalAlignment(HAlign_Right);
		GripSlot->SetVerticalAlignment(VAlign_Bottom);
		GripSlot->SetPadding(FMargin(0.0f, 0.0f, 4.0f, 4.0f));
		Grip = GripImage;
	}
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}

void UUiWindow::HandleClose()
{
	if (Host != nullptr)
	{
		Host->CloseByPlayer(Id);
	}
}

FReply UUiWindow::NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Host != nullptr)
	{
		Host->BringToFront(Id);
	}
	return Super::NativeOnPreviewMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UUiWindow::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	const FReply Reply = Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	if (!Reply.IsEventHandled() && Host != nullptr && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const FVector2D At = InMouseEvent.GetScreenSpacePosition();
		// The grip first: it sits inside the card, over the content.
		EUiWindowGesture Wanted = EUiWindowGesture::None;
		if (Grip != nullptr && Grip->GetCachedGeometry().IsUnderLocation(At))
		{
			Wanted = EUiWindowGesture::Resize;
		}
		else if (TitleBar != nullptr && TitleBar->GetCachedGeometry().IsUnderLocation(At))
		{
			Wanted = EUiWindowGesture::Move;
		}
		if (Wanted != EUiWindowGesture::None)
		{
			BeginGesture(Wanted, Host->ToLocal(At));
			return FReply::Handled().CaptureMouse(TakeWidget());
		}
	}
	return UiClicks::EatUnhandled(*this, WidgetTree != nullptr ? WidgetTree->RootWidget.Get() : nullptr, Reply, TEXT("down"));
}

FReply UUiWindow::NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	return UiClicks::EatUnhandled(*this, WidgetTree != nullptr ? WidgetTree->RootWidget.Get() : nullptr,
		Super::NativeOnMouseButtonDoubleClick(InGeometry, InMouseEvent), TEXT("double-click"));
}

FReply UUiWindow::NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Gesture == EUiWindowGesture::None || Host == nullptr)
	{
		return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
	}
	UpdateGesture(Host->ToLocal(InMouseEvent.GetScreenSpacePosition()));
	return FReply::Handled();
}

FReply UUiWindow::NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (Gesture == EUiWindowGesture::None || InMouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
	}
	EndGesture(TEXT("released"));
	return FReply::Handled().ReleaseMouseCapture();
}

void UUiWindow::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
	if (Gesture != EUiWindowGesture::None)
	{
		EndGesture(TEXT("capture lost"));   // CaptureLostEndsTheGesture
	}
}

void UUiWindow::BeginGesture(EUiWindowGesture Kind, FVector2D HostLocal)
{
	if (Host == nullptr)
	{
		return;
	}
	const FBox2D Rect = Host->WindowRect(Id);
	Gesture = Kind;
	GestureStartMouse = HostLocal;
	GestureStartTopLeft = Rect.Min;
	GestureStartSize = Rect.GetSize();
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: %s begins at (%.0f, %.0f) size (%.0f, %.0f)"), *Id.ToString(),
		Kind == EUiWindowGesture::Move ? TEXT("move") : TEXT("resize"),
		Rect.Min.X, Rect.Min.Y, GestureStartSize.X, GestureStartSize.Y);
}

void UUiWindow::UpdateGesture(FVector2D HostLocal)
{
	if (Host == nullptr)
	{
		return;
	}
	const FVector2D Delta = HostLocal - GestureStartMouse;
	if (Gesture == EUiWindowGesture::Move)
	{
		Host->MoveWindow(Id, GestureStartTopLeft + Delta);
	}
	else if (Gesture == EUiWindowGesture::Resize)
	{
		Host->ResizeWindow(Id, GestureStartSize + Delta);
	}
}

void UUiWindow::EndGesture(const TCHAR* Why)
{
	Gesture = EUiWindowGesture::None;
	if (Host != nullptr)
	{
		const FBox2D Rect = Host->WindowRect(Id);
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: gesture ends (%s) at (%.0f, %.0f) size (%.0f, %.0f)"), *Id.ToString(),
			Why, Rect.Min.X, Rect.Min.Y, Rect.GetSize().X, Rect.GetSize().Y);
	}
}
```

- [ ] **Step 6: Replace the host scaffold with `UI/UiWindowHost.h`:**

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/UiWindowSpec.h"
#include "UiWindowHost.generated.h"

class UAirportMgrPanelWidget;
class UBuildBarWidget;
class UCanvasPanel;
class UCanvasPanelSlot;
class UUiWindow;
class UUIStyle;

/** One window the host owns. */
USTRUCT()
struct FUiWindowEntry
{
	GENERATED_BODY()

	UPROPERTY() TObjectPtr<UUiWindow> Window;
	UPROPERTY() TObjectPtr<UAirportMgrPanelWidget> Panel;
	UPROPERTY() TObjectPtr<UCanvasPanelSlot> Slot;
	UPROPERTY() FUiWindowSpec Spec;
	/** The panel wants it shown. */
	bool bWanted = false;
	/** The player closed it; cleared when the panel hides it itself or forgets the close. */
	bool bUserClosed = false;
	/** The player moved or resized it: its slot is top-left anchored and no longer docks. */
	bool bPlaced = false;
};

/**
 * Every window, on one full-screen canvas at Z 1 (UI library step 2). The ONE object that sees
 * more than one window, which is why placement, z-order, snapping, clamping and the inspector's
 * dock above the bar all live here and not in UUiWindow.
 *
 * IT TICKS EVERY HOSTED PANEL ITSELF, hidden or not: a collapsed window stops Slate ticking its
 * contents, and a panel's tick is what decides to show it again.
 * ENFORCED BY: AirportMgr.UI.WindowHost.TicksHiddenPanelsOnce.
 *
 * All positions are in this widget's local units - its canvas slots' own space.
 */
UCLASS()
class AIRPORTMGR_API UUiWindowHost : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual bool Initialize() override;

	/** Wraps Panel in a window if Panel->WantsWindow says so; null (and nothing added) otherwise. */
	UUiWindow* AddWindow(UAirportMgrPanelWidget& Panel);

	void SetShown(FName Id, bool bShown);
	bool IsShown(FName Id) const;
	/** Clears a player's close, so the panel's next SetShown(true) shows it. */
	void ForgetDismissal(FName Id);
	/** The window's close button. Hides it and tells the panel (a toggled panel un-toggles). */
	void CloseByPlayer(FName Id);
	void BringToFront(FName Id);

	/** The bar the AboveBarLeft windows ride, and whose live height is off-limits to all of them. */
	void DockAbove(const UBuildBarWidget* Bar);

	/** A drag or resize, in host-local units, clamped and snapped. Places the window first. */
	void MoveWindow(FName Id, FVector2D ProposedTopLeft);
	void ResizeWindow(FName Id, FVector2D ProposedSize);

	/** The window's rectangle now, host-local. Empty box for an unknown id. */
	FBox2D WindowRect(FName Id) const;
	FVector2D ToLocal(FVector2D ScreenPosition) const;

	void TickForTest(float DeltaTime) { TickWindows(DeltaTime); }
	void SetViewSizeForTest(FVector2D Size) { ViewSize = Size; }
	UUiWindow* WindowForTest(FName Id) const;
	int32 ZOrderForTest(FName Id) const;
	/** An AboveBarLeft window's distance above the screen's bottom, as its slot has it. */
	double WindowClearanceForTest(FName Id) const;
	const UBuildBarWidget* DockedBarForTest() const { return DockBar; }

protected:
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	FUiWindowEntry* Find(FName Id);
	const FUiWindowEntry* Find(FName Id) const;
	void Apply(FUiWindowEntry& E);
	void TickWindows(float DeltaTime);
	/** Folds a window's slot to top-left anchoring at its current rectangle. */
	void Place(FUiWindowEntry& E);
	FVector2D TopLeftOf(const FUiWindowEntry& E) const;
	FVector2D SizeOf(const FUiWindowEntry& E) const;
	/** The screen minus the bar: where a window may be. */
	FBox2D Bounds() const;
	/** Every OTHER shown window's rectangle - hidden ones are not snap targets. */
	TArray<FBox2D> OthersThan(FName Id) const;
	double BarHeight() const;

	UPROPERTY() TArray<FUiWindowEntry> Windows;
	UPROPERTY() TObjectPtr<UCanvasPanel> Canvas;
	UPROPERTY() TObjectPtr<const UBuildBarWidget> DockBar;
	UPROPERTY() TObjectPtr<const UUIStyle> Style;
	FVector2D ViewSize = FVector2D(1920.0, 1080.0);
	int32 TopZ = 0;
};
```

- [ ] **Step 7: Write `UI/UiWindowHost.cpp`** (show/hide/close/front/tick here; `MoveWindow`, `ResizeWindow`, dock and reclamp bodies are filled in by Task 5 - write them now as shown, since Task 5's tests drive them):

```cpp
#include "UI/UiWindowHost.h"

#include "AirportMgrPanelWidget.h"
#include "Blueprint/WidgetTree.h"
#include "BuildBarWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "RoadBuildLog.h"
#include "UI/UiWindow.h"
#include "UI/WindowSnap.h"
#include "UIStyle.h"

bool UUiWindowHost::Initialize()
{
	const bool bOk = Super::Initialize();
	if (!bOk || Canvas != nullptr || HasAnyFlags(RF_ClassDefaultObject) || WidgetTree == nullptr)
	{
		return bOk;
	}
	Style = UAirportMgrUISettings::ResolveStyle();   // never null
	Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("WindowCanvas"));
	WidgetTree->RootWidget = Canvas;
	// Click-transparent: empty screen between windows still reaches the game.
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	return bOk;
}

UUiWindow* UUiWindowHost::AddWindow(UAirportMgrPanelWidget& Panel)
{
	FUiWindowSpec Spec;
	if (Canvas == nullptr || !Panel.WantsWindow(Spec))
	{
		return nullptr;
	}
	UUiWindow* Window = CreateWidget<UUiWindow>(this, UUiWindow::StaticClass());
	Window->Build(*Style, Spec, Panel, *this);

	UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(Window);
	Slot->SetAutoSize(true);
	switch (Spec.Anchor)
	{
	case EUiWindowAnchor::TopRight:
		Slot->SetAnchors(FAnchors(1.0f, 0.0f));
		Slot->SetAlignment(FVector2D(1.0, 0.0));
		Slot->SetPosition(FVector2D(-Spec.Offset.X, Spec.Offset.Y));
		break;
	case EUiWindowAnchor::AboveBarLeft:
		Slot->SetAnchors(FAnchors(0.0f, 1.0f));
		Slot->SetAlignment(FVector2D(0.0, 1.0));
		Slot->SetPosition(FVector2D(Spec.Offset.X, -(BarHeight() + Spec.Offset.Y)));
		break;
	default:
		Slot->SetAnchors(FAnchors(0.0f, 0.0f));
		Slot->SetAlignment(FVector2D::ZeroVector);
		Slot->SetPosition(Spec.Offset);
		break;
	}
	Slot->SetZOrder(++TopZ);

	FUiWindowEntry& E = Windows.AddDefaulted_GetRef();
	E.Window = Window;
	E.Panel = &Panel;
	E.Slot = Slot;
	E.Spec = Spec;
	Apply(E);
	UE_LOG(LogRoadBuild, Log, TEXT("Window %s: hosted (%s)"), *Spec.Id.ToString(), *Panel.GetName());
	Panel.AttachToHost(*this, Spec.Id);   // applies whatever the panel already asked for
	return Window;
}

FUiWindowEntry* UUiWindowHost::Find(FName Id)
{
	return Windows.FindByPredicate([Id](const FUiWindowEntry& E) { return E.Spec.Id == Id; });
}

const FUiWindowEntry* UUiWindowHost::Find(FName Id) const
{
	return Windows.FindByPredicate([Id](const FUiWindowEntry& E) { return E.Spec.Id == Id; });
}

void UUiWindowHost::Apply(FUiWindowEntry& E)
{
	const ESlateVisibility Wanted = (E.bWanted && !E.bUserClosed)
		? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed;
	// SetVisibility has no early-out of its own; panels ask every tick.
	if (E.Window != nullptr && E.Window->GetVisibility() != Wanted)
	{
		E.Window->SetVisibility(Wanted);
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: %s"), *E.Spec.Id.ToString(),
			Wanted == ESlateVisibility::Collapsed ? TEXT("hidden") : TEXT("shown"));
	}
}

void UUiWindowHost::SetShown(FName Id, bool bShown)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		if (!bShown)
		{
			E->bUserClosed = false;   // the panel hid it itself: its next show is a fresh one
		}
		E->bWanted = bShown;
		Apply(*E);
	}
}

bool UUiWindowHost::IsShown(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr && E->bWanted && !E->bUserClosed;
}

void UUiWindowHost::ForgetDismissal(FName Id)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		E->bUserClosed = false;
		Apply(*E);
	}
}

void UUiWindowHost::CloseByPlayer(FName Id)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		E->bUserClosed = true;
		Apply(*E);
		UE_LOG(LogRoadBuild, Log, TEXT("Window %s: closed by the player"), *Id.ToString());
		if (E->Panel != nullptr)
		{
			E->Panel->OnWindowClosedByPlayer();
		}
	}
}

void UUiWindowHost::BringToFront(FName Id)
{
	if (FUiWindowEntry* E = Find(Id))
	{
		if (E->Slot != nullptr && E->Slot->GetZOrder() != TopZ)
		{
			E->Slot->SetZOrder(++TopZ);
		}
	}
}

void UUiWindowHost::DockAbove(const UBuildBarWidget* Bar)
{
	DockBar = Bar;
	UE_LOG(LogRoadBuild, Log, TEXT("Window host: %s"), Bar != nullptr ? TEXT("docked above the build bar") : TEXT("no bar to dock above"));
}

double UUiWindowHost::BarHeight() const
{
	return DockBar != nullptr ? DockBar->LiveHeight() : 0.0;
}

FBox2D UUiWindowHost::Bounds() const
{
	return FBox2D(FVector2D::ZeroVector, FVector2D(ViewSize.X, FMath::Max(0.0, ViewSize.Y - BarHeight())));
}

FVector2D UUiWindowHost::SizeOf(const FUiWindowEntry& E) const
{
	if (E.Slot == nullptr || E.Window == nullptr)
	{
		return FVector2D::ZeroVector;
	}
	return E.Slot->GetAutoSize() ? E.Window->GetDesiredSize() : E.Slot->GetSize();
}

FVector2D UUiWindowHost::TopLeftOf(const FUiWindowEntry& E) const
{
	if (E.Slot == nullptr)
	{
		return FVector2D::ZeroVector;
	}
	// Computed, not read off cached geometry: anchor point + position - alignment * size is what
	// SConstraintCanvas lays out, and it holds before the first paint and in a headless test.
	const FAnchors A = E.Slot->GetAnchors();
	return FVector2D(A.Minimum.X * ViewSize.X, A.Minimum.Y * ViewSize.Y)
		+ E.Slot->GetPosition() - E.Slot->GetAlignment() * SizeOf(E);
}

FBox2D UUiWindowHost::WindowRect(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	if (E == nullptr)
	{
		return FBox2D(ForceInit);
	}
	const FVector2D TL = TopLeftOf(*E);
	return FBox2D(TL, TL + SizeOf(*E));
}

TArray<FBox2D> UUiWindowHost::OthersThan(FName Id) const
{
	TArray<FBox2D> Out;
	for (const FUiWindowEntry& E : Windows)
	{
		if (E.Spec.Id != Id && E.bWanted && !E.bUserClosed)   // SnapsOnlyToShownWindows
		{
			Out.Add(WindowRect(E.Spec.Id));
		}
	}
	return Out;
}

void UUiWindowHost::Place(FUiWindowEntry& E)
{
	if (E.bPlaced || E.Slot == nullptr)
	{
		return;
	}
	const FVector2D TL = TopLeftOf(E);
	E.Slot->SetAnchors(FAnchors(0.0f, 0.0f));
	E.Slot->SetAlignment(FVector2D::ZeroVector);
	E.Slot->SetPosition(TL);
	E.bPlaced = true;
}

void UUiWindowHost::MoveWindow(FName Id, FVector2D ProposedTopLeft)
{
	FUiWindowEntry* E = Find(Id);
	if (E == nullptr || E->Slot == nullptr)
	{
		return;
	}
	Place(*E);
	const TArray<FBox2D> Others = OthersThan(Id);
	E->Slot->SetPosition(WindowSnap::Place(ProposedTopLeft, SizeOf(*E), Bounds(), Others, Style->SnapDistance));
}

void UUiWindowHost::ResizeWindow(FName Id, FVector2D ProposedSize)
{
	FUiWindowEntry* E = Find(Id);
	if (E == nullptr || E->Slot == nullptr || !E->Spec.bResizable)
	{
		return;
	}
	Place(*E);
	const FVector2D TL = E->Slot->GetPosition();
	const TArray<FBox2D> Others = OthersThan(Id);
	const FVector2D Size = WindowSnap::Resize(TL, ProposedSize, Style->WindowMinSize, Bounds(), Others, Style->SnapDistance);
	E->Slot->SetAutoSize(false);
	E->Slot->SetSize(Size);
}

void UUiWindowHost::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (MyGeometry.GetLocalSize().X > 0.0 && MyGeometry.GetLocalSize().Y > 0.0)
	{
		ViewSize = MyGeometry.GetLocalSize();
	}
	TickWindows(InDeltaTime);
}

void UUiWindowHost::TickWindows(float DeltaTime)
{
	for (FUiWindowEntry& E : Windows)
	{
		if (E.Panel != nullptr)
		{
			E.Panel->RunPanelTick(DeltaTime);
		}
		if (E.Slot == nullptr)
		{
			continue;
		}
		if (!E.bPlaced && E.Spec.Anchor == EUiWindowAnchor::AboveBarLeft)
		{
			// RIDES THE BAR'S TOP EDGE as the bar grows and shrinks (the inspector's old DockAbove,
			// 2026-09-27: a fixed offset left it over the bar's left-hand sections). Written only
			// when it changed, so an unchanged bar writes nothing.
			const FVector2D Want(E.Spec.Offset.X, -(BarHeight() + E.Spec.Offset.Y));
			if (!E.Slot->GetPosition().Equals(Want))
			{
				E.Slot->SetPosition(Want);
			}
		}
		else if (E.bPlaced)
		{
			// A PLACED WINDOW STAYS REACHABLE when the view shrinks under it (ReclampsWhenTheViewShrinks):
			// a pure clamp, no snapping - nobody is dragging.
			const FVector2D At = E.Slot->GetPosition();
			const FVector2D Clamped = WindowSnap::Place(At, SizeOf(E), Bounds(), TConstArrayView<FBox2D>(), 0.0);
			if (!Clamped.Equals(At))
			{
				E.Slot->SetPosition(Clamped);
			}
		}
	}
}

FVector2D UUiWindowHost::ToLocal(FVector2D ScreenPosition) const
{
	return GetCachedGeometry().AbsoluteToLocal(ScreenPosition);
}

UUiWindow* UUiWindowHost::WindowForTest(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr ? E->Window.Get() : nullptr;
}

int32 UUiWindowHost::ZOrderForTest(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr && E->Slot != nullptr ? E->Slot->GetZOrder() : INDEX_NONE;
}

double UUiWindowHost::WindowClearanceForTest(FName Id) const
{
	const FUiWindowEntry* E = Find(Id);
	return E != nullptr && E->Slot != nullptr ? -E->Slot->GetPosition().Y : 0.0;
}
```

- [ ] **Step 8: Build; run `-Filter AirportMgr.UI.Window`.** Expected: the 7 tests pass. `ShowsWhatThePanelAsks` needs the ledger's `BuildOnce` to call `SetShown(false)` - it still calls `SetCardShown(false)` until Task 6, so in THIS task also change the ledger's two `SetCardShown(bShowing/false)` calls to `SetShown(...)` (the ledger is migrated first because it is the test panel; its `EnsureCardRoot` card stays until Task 6).

- [ ] **Step 9: Commit** `git add Source/AirportMgr && git commit -m "feat(ui): UUiWindow and UUiWindowHost - show, close, front, host-ticked panels"`

---

### Task 5: Geometry - move, resize, snap, dock, reclamp

The bodies were written in Task 4; this task is their tests, and fixes if a test finds a defect.

**Files:**
- Modify: `Source/AirportMgr/UI/UiWindowHostTest.cpp`

- [ ] **Step 1: Write the tests** - append:

```cpp
/**
 * A DRAG IS CLAMPED AND SNAPPED BY THE HOST - the window cannot leave the screen or go under the
 * bar, and lands flush on an edge within SnapDistance.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowMoveTest, "AirportMgr.UI.WindowHost.MoveClampsAndSnaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowMoveTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const FName Id(TEXT("ledger"));
	F.Ledger->Toggle();
	// A KNOWN SIZE: headless there is no layout, so an auto-sized window measures 0x0 - moved into
	// open space first, or the resize is clamped against the top-right corner it starts in.
	F.Host->MoveWindow(Id, FVector2D(100.0, 100.0));
	F.Host->ResizeWindow(Id, FVector2D(300.0, 200.0));
	F.Host->MoveWindow(Id, FVector2D(10.0, 400.0));
	TestEqual(TEXT("10 px from the left snaps flush"), F.Host->WindowRect(Id).Min, FVector2D(0.0, 400.0));
	F.Host->MoveWindow(Id, FVector2D(5000.0, 5000.0));
	TestEqual(TEXT("dragged far off: clamped to the bottom-right corner"),
		F.Host->WindowRect(Id).Min, FVector2D(1920.0 - 300.0, 1080.0 - 200.0));
	return true;
}

/** A VIEW THAT SHRINKS UNDER A PLACED WINDOW PULLS IT BACK - Review Focus 1. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowReclampTest, "AirportMgr.UI.WindowHost.ReclampsWhenTheViewShrinks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowReclampTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const FName Id(TEXT("ledger"));
	F.Ledger->Toggle();
	F.Host->MoveWindow(Id, FVector2D(100.0, 100.0));   // see MoveClampsAndSnaps: headless size is 0
	F.Host->ResizeWindow(Id, FVector2D(300.0, 200.0));
	F.Host->MoveWindow(Id, FVector2D(1600.0, 800.0));
	F.Host->SetViewSizeForTest(FVector2D(1280.0, 720.0));
	F.Host->TickForTest(0.016f);
	const FBox2D R = F.Host->WindowRect(Id);
	TestTrue(TEXT("its right edge is back on screen"), R.Max.X <= 1280.0);
	TestTrue(TEXT("its bottom edge is back on screen"), R.Max.Y <= 720.0);
	return true;
}

/** A CLOSED WINDOW LEAVES NO INVISIBLE EDGE to snap onto - Review Focus 2. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowSnapShownTest, "AirportMgr.UI.WindowHost.SnapsOnlyToShownWindows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowSnapShownTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	ULandAircraftPanelWidget* Land = CreateWidget<ULandAircraftPanelWidget>(F.TestWorld.World, ULandAircraftPanelWidget::StaticClass());
	if (!TestNotNull(TEXT("a second panel"), Land) || !TestNotNull(TEXT("with a window"), F.Host->AddWindow(*Land))) { return false; }
	const FName Ledger(TEXT("ledger")), LandId(TEXT("land"));
	F.Ledger->Toggle();
	F.Host->MoveWindow(Ledger, FVector2D(100.0, 100.0));   // see MoveClampsAndSnaps: headless size is 0
	F.Host->ResizeWindow(Ledger, FVector2D(300.0, 200.0));
	F.Host->MoveWindow(Ledger, FVector2D(600.0, 300.0));
	Land->Toggle();
	F.Host->MoveWindow(LandId, FVector2D(100.0, 600.0));
	F.Host->ResizeWindow(LandId, FVector2D(300.0, 200.0));
	F.Host->MoveWindow(LandId, FVector2D(908.0, 320.0));
	TestEqual(TEXT("beside the SHOWN ledger it snaps flush"), F.Host->WindowRect(LandId).Min.X, 900.0);
	F.Ledger->Toggle();   // hidden
	F.Host->MoveWindow(LandId, FVector2D(908.0, 320.0));
	TestEqual(TEXT("beside a HIDDEN ledger it stays where it was dropped"), F.Host->WindowRect(LandId).Min.X, 908.0);
	return true;
}
```

(add `#include "LandAircraftPanelWidget.h"`; the Land panel's `WantsWindow` arrives in Task 6 - so write `SnapsOnlyToShownWindows` now but expect it to fail at "with a window" until Task 6; that is its RED. Every other test here must pass in this task.)

- [ ] **Step 2: Build; run `-Filter AirportMgr.UI.WindowHost`.** Expected: `MoveClampsAndSnaps` and `ReclampsWhenTheViewShrinks` PASS against Task 4's bodies; `SnapsOnlyToShownWindows` FAILS on "with a window". If either of the first two fails, the defect is in Task 4's host code - fix it there (systematic-debugging), not in the test.

- [ ] **Step 3: Commit** `git commit -am "test(ui): the host's clamp, snap and reclamp"`

---

### Task 6: The four panels become windows

**Files:**
- Modify: `AirportMgrPanelWidget.h/.cpp` (remove `EnsureCardRoot`, `SetCardShown`, `CardWidget`; add `EnsureContentRoot`)
- Modify: `InspectorWidget.h/.cpp`, `LedgerPanelWidget.cpp`, `LandAircraftPanelWidget.h/.cpp`, `OfferInboxWidget.h/.cpp`
- Test: `InspectorWidgetTest.cpp` (the dock test moves to Task 7's HUD test), `UI/UiWindowHostTest.cpp`

**Interfaces:**
- Produces: `UPanelWidget* UAirportMgrPanelWidget::EnsureContentRoot(FName ContentName);` - a root `UVerticalBox` named ContentName when the asset supplied no root (returned for the subclass to fill), else null.
- Window specs (ids are the persistence keys step 3 will use - do not rename them later):

| Panel | Id | Title | Closable | Anchor | Offset |
|---|---|---|---|---|---|
| Inspector | `inspector` | Inspector | yes | AboveBarLeft | (12, BarGap) |
| Ledger | `ledger` | Ledger | yes | TopRight | (12, TopOffset) |
| Land | `land` | Land an aircraft | yes | TopLeft | (12, TopOffset) |
| Offers | `offers` | Offers | no | TopRight | (12, TopOffset) |

- [ ] **Step 1: Write the failing test** - append to `UI/UiWindowHostTest.cpp`:

```cpp
/**
 * EVERY FLOATING PANEL IS A WINDOW, with the id step 3 will persist under. A panel left out would
 * float as a bare card with no title, close or drag - the look this step exists to remove.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowPanelsTest, "AirportMgr.UI.WindowHost.FourPanelsAreWindows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowPanelsTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	UWorld* W = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), W)) { return false; }
	UUiWindowHost* Host = CreateWidget<UUiWindowHost>(W, UUiWindowHost::StaticClass());
	TestNotNull(TEXT("inspector"), Host->AddWindow(*CreateWidget<UInspectorWidget>(W, UInspectorWidget::StaticClass())));
	TestNotNull(TEXT("ledger"), Host->AddWindow(*CreateWidget<ULedgerPanelWidget>(W, ULedgerPanelWidget::StaticClass())));
	TestNotNull(TEXT("land"), Host->AddWindow(*CreateWidget<ULandAircraftPanelWidget>(W, ULandAircraftPanelWidget::StaticClass())));
	TestNotNull(TEXT("offers"), Host->AddWindow(*CreateWidget<UOfferInboxWidget>(W, UOfferInboxWidget::StaticClass())));
	for (const TCHAR* Id : { TEXT("inspector"), TEXT("ledger"), TEXT("land"), TEXT("offers") })
	{
		TestNotNull(*FString::Printf(TEXT("a window under the id '%s'"), Id), Host->WindowForTest(Id));
	}
	TestTrue(TEXT("the offers window shows from the start - an offer must never be hidden"), Host->IsShown(TEXT("offers")));
	TestNull(TEXT("the bar is not a window"), Host->AddWindow(*CreateWidget<UBuildBarWidget>(W, UBuildBarWidget::StaticClass())));
	return true;
}
```

(includes: `InspectorWidget.h`, `OfferInboxWidget.h`, `BuildBarWidget.h`.)

- [ ] **Step 2: Build; expect FAIL** on inspector/land/offers (no `WantsWindow`).

- [ ] **Step 3: Panel base.** Delete `EnsureCardRoot`, `SetCardShown`, `CardWidget` and their doc comments from `AirportMgrPanelWidget.h/.cpp`; MOVE the WHY content that still applies (the Blueprint-asset path: "an asset already supplied a root ... a code-built card here would replace the designer's layout") onto `EnsureContentRoot`'s comment. Add:

```cpp
	/**
	 * A root VerticalBox named ContentName, returned for the subclass to fill, when the asset gave
	 * no root; null when an asset supplied one (BindWidgetOptional has filled its slots, and a
	 * code-built root would replace the designer's layout). Replaces EnsureCardRoot: the CARD -
	 * surface, padding, corners, placement - is the window's now (UUiWindow), not the panel's.
	 */
	UPanelWidget* EnsureContentRoot(FName ContentName);
```

```cpp
UPanelWidget* UAirportMgrPanelWidget::EnsureContentRoot(FName ContentName)
{
	if (WidgetTree->RootWidget != nullptr)
	{
		return nullptr;
	}
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), ContentName);
	WidgetTree->RootWidget = Column;
	return Column;
}
```

Update `BuildOnce`'s doc comment: the SelfHitTestInvisible-not-Collapsed paragraph becomes "a hosted panel is ticked by the host whatever its visibility (see RunPanelTick); a panel still marks its root SelfHitTestInvisible so its empty space does not eat clicks inside the window" - keep the PIE 2026-09-07 history sentence.

- [ ] **Step 4: Inspector.** Replace `EnsureCardRoot(TEXT("InspectorCard"), ...)` with `EnsureContentRoot(TEXT("InspectorCard"))`; every `SetCardShown(x)` with `SetShown(x)`; `IsShownForTest()` returns `IsShown()`. Rename the `NativeTick` override to `virtual void TickPanel(float DeltaTime) override;` (drop `Super::NativeTick`, drop `UpdateDock()`). DELETE `DockAbove`, `UpdateDock`, `UpdateDockForTest`, `CardClearanceForTest`, `DockedBarForTest`, `DockBar`, `DockedClearance` - the host docks now; keep `BarGap` and its comment (it is the window spec's offset), rewording "Measured from the bar instead (see DockAbove)" to "(see UUiWindowHost::DockAbove)". Keep the "BEFORE Refresh, which may show the card" comment's reason on the host's dock loop instead (it already says why the dock is written each tick). Add:

```cpp
bool UInspectorWidget::WantsWindow(FUiWindowSpec& Out) const
{
	Out.Id = TEXT("inspector");
	Out.Title = NSLOCTEXT("AirportMgr", "InspectorWindow", "Inspector");
	Out.Anchor = EUiWindowAnchor::AboveBarLeft;
	Out.Offset = FVector2D(12.0, BarGap);
	return true;
}
```

and at the top of `Refresh`, after the early-out for no selection:

```cpp
	// A NEW SELECTION REOPENS A WINDOW THE PLAYER CLOSED: the close meant "not this one", and
	// clicking another aircraft is asking to see it (Review Focus 4).
	if (Selection.Kind != LastSelection.Kind || Selection.Id != LastSelection.Id)
	{
		ForgetPlayerClose();
		LastSelection = Selection;
	}
```

with `FSelection LastSelection;` in the private section (and reset it in the no-selection branch: `LastSelection = FSelection();`). Remove the Inspector's `DockAbove` call from `UBuildHudLayer::WireDocking` in the same commit (Task 7 replaces the function).

- [ ] **Step 5: Ledger.** `EnsureCardRoot(TEXT("LedgerCard"), ...)` → `EnsureContentRoot(TEXT("LedgerCard"))`; collapse its `TitleText` (the window title says "Ledger"): after `ApplyText` on it add `TitleText->SetVisibility(ESlateVisibility::Collapsed);   // the window's title bar says it now`; `NativeTick` → `TickPanel` (drop Super); remaining `SetCardShown` → `SetShown`; delete the "CardWidget is found and cached by EnsureCardRoot" comment and the `// CardWidget and PanelStyle moved to the base class` comment's CardWidget half.

- [ ] **Step 6: Land.** Same shape as the ledger, id `land`, title `LOCTEXT("LandWindow", "Land an aircraft")`, `TopLeft`, `(12, TopOffset)`, and `OnWindowClosedByPlayer` = `if (bShowing) { Toggle(); }` with the ledger's comment.

- [ ] **Step 7: Offers.** `EnsureContentRoot(TEXT("InboxCard"))`; `TitleText->SetVisibility(Collapsed)` (the count stays on its row); `WantsWindow` → id `offers`, `NSLOCTEXT("AirportMgr", "InboxWindow", "Offers")`, `bClosable = false` (an offer must never be hidden - its own TopOffset comment), `TopRight`, `(12, TopOffset)`; in `BuildOnce` add `SetShown(true);` with that reason. `NativeTick` → `TickPanel`; `NativeTickForTest` (now on the base) runs `RunPanelTick`.

- [ ] **Step 8: Build; run `-Filter AirportMgr`.** Expected: all pass except `AirportMgr.Inspector.DocksAboveTheBar` which no longer compiles - DELETE it here (its assertions move, verbatim in meaning, into Task 7's HUD test; note that in the commit message). `SnapsOnlyToShownWindows` and `FourPanelsAreWindows` now pass.

- [ ] **Step 9: Refactor contract check.** `UE_LOG(` count across the touched files: the Inspector's `"Inspector: %s"` docked/undocked line moved to the host's `"Window host: %s"` - say so in the commit. Comment lines in the touched files: compare with `git diff --stat` and read the deletions; every WHY either moved or its code is gone.

- [ ] **Step 10: Commit** `git commit -am "feat(ui): inspector, ledger, Land and offers are windows; EnsureCardRoot goes"`

---

### Task 7: The HUD wires the host

**Files:**
- Modify: `Source/AirportMgr/BuildHudLayer.h/.cpp`
- Test: `Source/AirportMgr/InspectorWidgetTest.cpp` (new HUD-level dock test replacing the deleted one)

**Interfaces:**
- Produces: `UPROPERTY(Transient) TObjectPtr<UUiWindowHost> WindowHost;` on `UBuildHudLayer`; `void WireWindows();` (replaces `WireDocking`).

- [ ] **Step 1: Write the failing test** - in `InspectorWidgetTest.cpp`, where `DocksAboveTheBar` was:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectorWindowDocksAboveTheBarTest,
	"AirportMgr.Inspector.WindowDocksAboveTheBar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FInspectorWindowDocksAboveTheBarTest::RunTest(const FString& Parameters)
{
	// THE WINDOW NEVER COVERS THE BAR, however tall the bar grows (2026-09-27: the card sat a
	// fixed 72 uu above the screen's bottom while the bar was 118 uu tall). Through the HUD
	// layer's own wiring, not a hand-called DockAbove, so an unwired seam goes red here. Moved
	// from AirportMgr.Inspector.DocksAboveTheBar when the dock moved into UUiWindowHost.
	FAirsideTestWorld TestWorld(/*bSpawnActor=*/false);
	if (!TestNotNull(TEXT("a world"), TestWorld.World)) { return false; }
	ARoadBuildController* C = TestWorld.World->SpawnActor<ARoadBuildController>();
	if (!TestNotNull(TEXT("controller spawned"), C) || !TestNotNull(TEXT("with a HUD layer"), C->GetHudForTest()))
	{
		return false;
	}
	// A headless controller has no local player, so BeginPlay never created the HUD's widgets;
	// they are put where CreateAll would have put them, then CreateAll's own last step runs.
	UBuildHudLayer* Hud = C->GetHudForTest();
	Hud->BuildBar = CreateWidget<UBuildBarWidget>(TestWorld.World, UBuildBarWidget::StaticClass());
	Hud->Inspector = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	Hud->WindowHost = CreateWidget<UUiWindowHost>(TestWorld.World, UUiWindowHost::StaticClass());
	if (!TestNotNull(TEXT("bar"), Hud->BuildBar.Get()) || !TestNotNull(TEXT("inspector"), Hud->Inspector.Get())
		|| !TestNotNull(TEXT("host"), Hud->WindowHost.Get())) { return false; }
	Hud->WireWindows();
	TestTrue(TEXT("the HUD docks the host on ITS bar"), Hud->WindowHost->DockedBarForTest() == Hud->BuildBar);
	TestNotNull(TEXT("and hosts the inspector"), Hud->WindowHost->WindowForTest(TEXT("inspector")));

	UBuildBarWidget& Bar = *Hud->BuildBar;
	UUiWindowHost& Host = *Hud->WindowHost;
	const double Gap = Hud->Inspector->BarGap;
	const float Wide = static_cast<float>(Bar.SectionRowSizeForTest(6000.0f).X);
	if (!TestTrue(TEXT("the bar's row measures to a real width"), Wide > 100.0f)) { return false; }

	const float OneLine = Bar.BarReservedHeightForTest(Wide * 1.1f);
	Host.TickForTest(0.016f);
	TestEqual(TEXT("the window's bottom sits BarGap above the bar's top edge"),
		Host.WindowClearanceForTest(TEXT("inspector")), static_cast<double>(OneLine) + Gap, 0.5);

	const float TwoLines = Bar.BarReservedHeightForTest(Wide * 0.66f);
	if (!TestTrue(TEXT("the wrapped bar is taller - otherwise this proves nothing"), TwoLines > OneLine + 10.0f)) { return false; }
	Host.TickForTest(0.016f);
	TestEqual(TEXT("the window rises with a growing bar"),
		Host.WindowClearanceForTest(TEXT("inspector")), static_cast<double>(TwoLines) + Gap, 0.5);

	Bar.BarReservedHeightForTest(Wide * 1.1f);
	Host.TickForTest(0.016f);
	TestEqual(TEXT("and comes back down when the bar shrinks"),
		Host.WindowClearanceForTest(TEXT("inspector")), static_cast<double>(OneLine) + Gap, 0.5);

	// ONCE THE PLAYER MOVES IT, IT STAYS: docking would yank a placed window back over the bar.
	Host.MoveWindow(TEXT("inspector"), FVector2D(400.0, 300.0));
	const FBox2D Placed = Host.WindowRect(TEXT("inspector"));
	Bar.BarReservedHeightForTest(Wide * 0.66f);
	Host.TickForTest(0.016f);
	TestEqual(TEXT("a placed window does not follow the bar"), Host.WindowRect(TEXT("inspector")).Min, Placed.Min);
	return true;
}
```

(add `#include "UI/UiWindowHost.h"`.)

- [ ] **Step 2: Build; expect compile failure** (`WindowHost`, `WireWindows`).

- [ ] **Step 3: Implement.** `BuildHudLayer.h`: forward-declare `class UUiWindowHost;`; add

```cpp
	/** Every floating panel's window, on one canvas at Z 1 - see UUiWindowHost. */
	UPROPERTY(Transient)
	TObjectPtr<UUiWindowHost> WindowHost;
```

replace `WireDocking` with

```cpp
	/**
	 * Hands each floating panel to the window host and docks the host above the bar - the
	 * relationships between these widgets. Split from CreateAll so a headless test, which cannot
	 * add widgets to a viewport, can put the widgets in place and prove the wiring.
	 */
	void WireWindows();
```

and update `CreateAll`'s doc comment Z-order sentence (bar 0; window host 1, holding the inspector, inbox, ledger and Land; toasts 2). In `CreateConfiguredWidget`, treat `ZOrder == INDEX_NONE` as "not added to the viewport - the window host takes it" (one `if` around `AddToViewport`; the log line unchanged). `BuildHudLayer.cpp`:

```cpp
void UBuildHudLayer::CreateAll(APlayerController& Owner)
{
	BuildBar = CreateConfiguredWidget<UBuildBarWidget>(Owner, BuildBarClass, 0,
		TEXT("Build bar"), TEXT("BuildBarClass"));
	WindowHost = CreateWidget<UUiWindowHost>(&Owner, UUiWindowHost::StaticClass());
	if (WindowHost != nullptr)
	{
		WindowHost->AddToViewport(1);
	}
	// INDEX_NONE: not added to the viewport - WireWindows hands each to the window host.
	Inspector = CreateConfiguredWidget<UInspectorWidget>(Owner, InspectorClass, INDEX_NONE,
		TEXT("Inspector"), TEXT("InspectorClass"));
	OfferInbox = CreateConfiguredWidget<UOfferInboxWidget>(Owner, OfferInboxClass, INDEX_NONE,
		TEXT("Offer inbox"), TEXT("OfferInboxClass"));
	LedgerPanel = CreateConfiguredWidget<ULedgerPanelWidget>(Owner, LedgerPanelClass, INDEX_NONE,
		TEXT("Ledger panel"), TEXT("LedgerPanelClass"));
	LandPanel = CreateConfiguredWidget<ULandAircraftPanelWidget>(Owner, LandPanelClass, INDEX_NONE,
		TEXT("Land panel"), TEXT("LandPanelClass"));
	ToastStack = CreateConfiguredWidget<UToastStackWidget>(Owner, ToastStackClass, 2,
		TEXT("Toast stack"), TEXT("ToastStackClass"));
	WireWindows();
}

void UBuildHudLayer::WireWindows()
{
	if (WindowHost == nullptr)
	{
		return;
	}
	for (UAirportMgrPanelWidget* Panel : TArray<UAirportMgrPanelWidget*>{ Inspector, OfferInbox, LedgerPanel, LandPanel })
	{
		if (Panel != nullptr)
		{
			WindowHost->AddWindow(*Panel);
		}
	}
	WindowHost->DockAbove(BuildBar);
}
```

- [ ] **Step 4: Build; run `-Filter AirportMgr`.** Expected: all pass, including `WindowDocksAboveTheBar` and the Land test that toggles through the controller without a host (`IsShowing` is the panel's own flag).

- [ ] **Step 5: Commit** `git commit -am "feat(ui): the HUD hands its panels to the window host"`

---

### Task 8: Look, verify, hand over

- [ ] **Step 1: Authoritative run:** `./Tools/Run-AirsideTests.ps1 -Project ...` (no filter). Quote the `N test(s) run` line.

- [ ] **Step 2: PIE on port 8002** (see Global Constraints). `grep` the log for `Window host: docked above the build bar` and four `Window <id>: hosted` lines. `shot <scratch>/step2.png editor`; look: the Offers window top-right with its title bar and no close; nothing else open. Sample: the window's card pixel is 255, the title hairline is E2E6EA-ish.

- [ ] **Step 3: Hand to the user** (synthetic clicks do not reach Slate here): press B and 7, click an aircraft; drag each title bar, drag a grip, drag one window near another and near the bar, click the ×, click a window under another. Log lines that prove each: `Window <id>: move begins`, `gesture ends (released) at`, `closed by the player`, `shown`/`hidden`.

- [ ] **Step 4: No push, no PR** (user, 2026-09-28). Report the branch head.
