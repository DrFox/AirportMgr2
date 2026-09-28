#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ScrollBox.h"
#include "BuildBarWidget.h"
#include "InspectorWidget.h"
#include "LandAircraftPanelWidget.h"
#include "LedgerPanelWidget.h"
#include "Misc/AutomationTest.h"
#include "OfferInboxWidget.h"
#include "Testing/AirsideTestWorld.h"
#include "UI/UiButton.h"
#include "UI/UiLayoutStore.h"
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
	// ONE ID, ONE WINDOW: a second panel claiming "ledger" would make every Find ambiguous.
	ULedgerPanelWidget* Second = CreateWidget<ULedgerPanelWidget>(F.TestWorld.World, ULedgerPanelWidget::StaticClass());
	AddExpectedError(TEXT("claimed an id already hosted"), EAutomationExpectedErrorFlags::Contains, 1);
	TestNull(TEXT("a duplicate id is refused"), F.Host->AddWindow(*Second));
	// A click on the window already on top must not re-number it every frame of a drag.
	const int32 Before = F.Host->ZOrderForTest(TEXT("ledger"));
	F.Host->BringToFront(TEXT("ledger"));
	TestEqual(TEXT("already on top: left alone"), F.Host->ZOrderForTest(TEXT("ledger")), Before);
	// Raising over ANOTHER window: AirportMgr.UI.WindowHost.FourPanelsAreWindows, once there are four.
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
	TestEqual(TEXT("10 px in snaps onto the 12 px margin line, not flush - the default inset is a place to rest"),
		F.Host->WindowRect(Id).Min, FVector2D(12.0, 400.0));
	F.Host->MoveWindow(Id, FVector2D(4.0, 400.0));
	TestEqual(TEXT("4 px in is nearer the edge: flush"), F.Host->WindowRect(Id).Min, FVector2D(0.0, 400.0));
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
	if (!TestNotNull(TEXT("a host"), Host)) { return false; }
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

	// RAISING OVER ANOTHER WINDOW (moved here from BringToFrontRaises, which had only one): the
	// inspector was added first, so every later window sits above it until it is clicked.
	TestTrue(TEXT("the first window starts below the last"), Host->ZOrderForTest(TEXT("inspector")) < Host->ZOrderForTest(TEXT("offers")));
	Host->BringToFront(TEXT("inspector"));
	TestTrue(TEXT("a click lifts it over the last"), Host->ZOrderForTest(TEXT("inspector")) > Host->ZOrderForTest(TEXT("offers")));
	return true;
}

/**
 * THE FIRST PIXEL OF A DRAG DOES NOT JUMP. Every default inset is 12 px and the snap distance is
 * 12, so without a margin line a 1 px drag threw a window resting at its default place flush into
 * the screen corner (final review 2026-09-28).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowFirstPixelTest, "AirportMgr.UI.WindowHost.FirstPixelOfADragDoesNotJump",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowFirstPixelTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const FName Id(TEXT("ledger"));
	F.Ledger->Toggle();
	F.Host->MoveWindow(Id, FVector2D(100.0, 100.0));   // see MoveClampsAndSnaps: headless size is 0
	F.Host->ResizeWindow(Id, FVector2D(300.0, 200.0));
	const FVector2D Rest(1920.0 - 300.0 - 12.0, 12.0);   // its default top-right inset
	F.Host->MoveWindow(Id, Rest);
	F.Host->MoveWindow(Id, Rest + FVector2D(1.0, 1.0));
	TestEqual(TEXT("a 1 px drag from the default place stays at the default place"), F.Host->WindowRect(Id).Min, Rest);
	return true;
}

/**
 * CONTENT A WINDOW NO LONGER FITS IS SIGNALLED, not silently cut off: the scroll bar shows when
 * it is needed. And the offers window cannot be resized at all - an offer must never be the thing
 * that scrolls away (OfferInboxWidget's TopOffset comment; final review 2026-09-28).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiWindowOverflowTest, "AirportMgr.UI.WindowHost.OverflowIsSignalledAndOffersNeverShrink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiWindowOverflowTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	const UScrollBox* Scroll = Cast<UScrollBox>(F.Window->WidgetTree->FindWidget(TEXT("WindowScroll")));
	if (!TestNotNull(TEXT("the window has its scroll body"), Scroll)) { return false; }
	TestNotEqual(TEXT("its scroll bar may show when the content overflows"), Scroll->GetScrollBarVisibility(), ESlateVisibility::Collapsed);

	UOfferInboxWidget* Offers = CreateWidget<UOfferInboxWidget>(F.TestWorld.World, UOfferInboxWidget::StaticClass());
	if (!TestNotNull(TEXT("an offers window"), F.Host->AddWindow(*Offers))) { return false; }
	const FVector2D Before = F.Host->WindowRect(TEXT("offers")).GetSize();
	F.Host->ResizeWindow(TEXT("offers"), FVector2D(180.0, 90.0));
	TestEqual(TEXT("the offers window refuses a resize"), F.Host->WindowRect(TEXT("offers")).GetSize(), Before);
	return true;
}

namespace UiWindowHostTest
{
	/** A host whose ledger has a memory store attached. */
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
 * A WINDOW COMES BACK WHERE THE PLAYER LEFT IT - position and size - on the next launch.
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

/** A DRAG WRITES ONCE, at its end - not a flush per frame (Review Focus 4). */
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

/**
 * THE SAVED LAYOUT WAITS FOR THE BAR TO SETTLE. The bar's first measure runs its wrap box at
 * SWrapBox's 100-unit starting width, so its first height reads several sections tall; judging the
 * saved layout then threw away every window saved just above the real bar, every launch (final
 * review 2026-09-28, traced to SWrapBox.cpp:17/52). The judgement waits for two equal readings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutWaitsForBarTest, "AirportMgr.UI.WindowHost.RestoreWaitsForTheBarToSettle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutWaitsForBarTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	UBuildBarWidget* Bar = CreateWidget<UBuildBarWidget>(F.TestWorld.World, UBuildBarWidget::StaticClass());
	if (!TestNotNull(TEXT("a bar"), Bar)) { return false; }
	const float Wide = static_cast<float>(Bar->SectionRowSizeForTest(6000.0f).X);
	const float Real = Bar->BarReservedHeightForTest(Wide * 1.1f);
	const float Inflated = Bar->BarReservedHeightForTest(Wide * 0.2f);   // what frame 1 reads
	if (!TestTrue(TEXT("the inflated reading is taller - otherwise this proves nothing"), Inflated > Real + 50.0f)) { return false; }

	// Saved just above the REAL bar: fits it, does not fit the inflated one.
	FUiWindowPlacement Saved;
	Saved.Size = FVector2D(300.0, 200.0);
	Saved.bSized = true;
	Saved.TopLeft = FVector2D(400.0, 1080.0 - Real - 200.0 - 10.0);
	F.Store->Write(TEXT("ledger"), Saved);
	F.Host->DockAbove(Bar);
	F.Ledger->Toggle();

	F.Host->TickForTest(0.016f);            // frame 1: inflated - must not judge yet
	Bar->BarReservedHeightForTest(Wide * 1.1f);
	F.Host->TickForTest(0.016f);            // changed - still waiting
	F.Host->TickForTest(0.016f);            // stable - judged now, against the real bar
	TestEqual(TEXT("restored where it was saved, not thrown back to the default"),
		F.Host->WindowRect(TEXT("ledger")).Min, Saved.TopLeft);
	return true;
}

/**
 * AN AUTO-SIZED WINDOW IS JUDGED AT LEAST AT THE MINIMUM SIZE. A hidden window measures 0x0, so a
 * layout from an ultrawide screen "fitted" at its top-left and was then clamped into the corner
 * the first time it showed - the placement Review Focus 1 rules out (final review 2026-09-28).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiLayoutAutoSizeFitTest, "AirportMgr.UI.WindowHost.AutoSizedRestoreIsJudgedAtMinimumSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FUiLayoutAutoSizeFitTest::RunTest(const FString& Parameters)
{
	UiWindowHostTest::FStoredFixture F;
	if (!TestNotNull(TEXT("a window"), F.Window)) { return false; }
	FUiWindowPlacement Saved;
	Saved.TopLeft = FVector2D(1850.0, 200.0);   // an ultrawide's right-hand side; 1850 + 180 > 1920
	F.Store->Write(TEXT("offers"), Saved);
	UOfferInboxWidget* Offers = CreateWidget<UOfferInboxWidget>(F.TestWorld.World, UOfferInboxWidget::StaticClass());
	F.Host->AddWindow(*Offers);
	F.Host->TickForTest(0.016f);
	TestEqual(TEXT("too near the edge for even the minimum size: the default top-right placement"),
		F.Host->WindowRect(TEXT("offers")).Min, FVector2D(1920.0 - 12.0, 12.0));
	return true;
}

#endif
