#include "CoreMinimal.h"
#include "Components/VerticalBox.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "OfferInboxWidget.h"
#include "OfferViewModels.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "UIStyle.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxWidgetTest,
	"AirportMgr.UI.OfferInboxWidget.ShowsARowPerOffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxWidgetTest::RunTest(const FString& Parameters)
{
	// THE PANEL IS WIRED. A real widget with no Blueprint behind it, built entirely in code -
	// which is the path a player gets until somebody makes the asset, so it is the path worth
	// pinning. Refresh is called directly, as UInspectorWidget's test does and for the same
	// reason: a headless test never paints, so NativeTick never runs.
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }

	// PLACE A NODE FIRST. ARoadNetworkActor::Network is null until URoadEditFacade::
	// EnsureNetwork makes one on the first edit, so a freshly spawned actor has no network
	// to place an entity in - dereferencing it crashes, which is how this was found.
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	if (!TestNotNull(TEXT("the first edit made a network"), Actor->Network.Get())) { return false; }

	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	Actor->Network->PlaceEntity(Stand, Stand->Anchors, FVector2D::ZeroVector, 0.0, 3600.0,
		Stand->PoseRole, Stand->Trucks);

	UOfferInboxWidget* Widget = CreateWidget<UOfferInboxWidget>(World, UOfferInboxWidget::StaticClass());
	if (!TestNotNull(TEXT("the widget was created"), Widget)) { return false; }
	if (!TestNotNull(TEXT("it built its own viewmodel"), Widget->GetInbox())) { return false; }
	TestNotNull(TEXT("and a code-built column to hang rows on"), Widget->OfferColumn.Get());

	// The board is driven directly rather than through UOpsRuntime: this test is about the
	// widget reading a viewmodel, and a game-instance subsystem needs a game instance.
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	for (int32 Index = 0; Index < 2; ++Index)
	{
		UFlight* Offer = NewObject<UFlight>(GetTransientPackage());
		Offer->Airframe.Wingspan = 3400.0;
		Offer->ArrivesAt = Clock->Now() + 600.0;
		Offer->ExpiresAt = Clock->Now() + 600.0;
		Offer->AirlineName = FText::FromString(TEXT("Meridian"));
		Offer->TypeName = FText::FromString(TEXT("A320"));
		Board->AddOffer(*Clock, Offer);
	}

	Widget->GetInbox()->Refresh(*Board, *Traffic, *Actor->Network, *Clock);
	TestEqual(TEXT("the viewmodel has a row per offer"), Widget->GetInbox()->GetOffers().Num(), 2);
	TestEqual(TEXT("and the badge count agrees with it"), Widget->GetInbox()->GetPendingCount(), 2);

	// Accepting through the widget's own entry point is what a button click does, so this is
	// the click path without a click.
	// THE CARDS THEMSELVES, which nothing covered before: this test drove the VIEWMODEL and
	// stopped there, so PaintRows - the whole code-built path a player actually sees - could
	// have built nothing at all and this still passed. Refresh through the widget is what
	// builds them.
	Widget->PaintRowsForTest();
	TestEqual(TEXT("one card is built per offer, so the code-built path really draws them"),
		Widget->RowWidgetCountForTest(), 2);

	Widget->AcceptRow(0);
	TestEqual(TEXT("accepting a row takes it out of the inbox"),
		Widget->GetInbox()->GetPendingCount(), 1);

	// And the cards follow the viewmodel down, rather than leaving a stale third card.
	Widget->PaintRowsForTest();
	TestEqual(TEXT("the cards follow the offers down"), Widget->RowWidgetCountForTest(), 1);
	return true;
}

/**
 * AN IDLE TICK RESOLVES THE STYLE ZERO TIMES - issue #309, closing the #260 item this issue's
 * own text names. PaintRows called UAirportMgrUISettings::ResolveStyle() (a
 * TSoftObjectPtr::LoadSynchronous) itself every time NativeTick called it, which #187 fixed on
 * the bar and the panels but never reached this file - the exact regression shape CLAUDE.md's
 * "check where a list is consumed" warns about, since #187 fixed the two SITES it named rather
 * than the shape (every panel's per-tick repaint) it should have covered.
 *
 * PaintRowsForTest, not NativeTickForTest, drives the loop below: NativeTick only reaches
 * PaintRows through Refresh, which bails before painting anything unless UOpsRuntimeSubsystem
 * resolves a live UOpsRuntime - a real UGameInstance FAirsideTestWorld deliberately does not
 * build (see its own header) - so a NativeTickForTest-driven version of this test would pass
 * unconditionally, before or after the fix, with PaintRows sitting unreached the whole time. PaintRows is
 * the function issue #309 actually names and the one this test must call to mean anything - the
 * seam OfferInboxWidgetTest's own PaintRowsForTest call above already uses for the same reason.
 * NativeTickForTest exists on the class regardless, matching UBuildBarWidget's own seam, for a
 * future test that supplies a real runtime.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxIdleTickResolvesNoStyleTest,
	"AirportMgr.UI.OfferInboxWidget.IdleTickResolvesNoStyle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxIdleTickResolvesNoStyleTest::RunTest(const FString& Parameters)
{
	FAirsideTestWorld TestWorld;
	UWorld* World = TestWorld.World;
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
	if (!TestNotNull(TEXT("the first edit made a network"), Actor->Network.Get())) { return false; }

	UOfferInboxWidget* Widget = CreateWidget<UOfferInboxWidget>(World, UOfferInboxWidget::StaticClass());
	if (!TestNotNull(TEXT("the widget was created"), Widget)) { return false; }

	// A REAL OFFER ON THE BOARD, not an empty inbox: PaintRows' style read (Style->Accent /
	// Style->Button on the Accept button, per-row, per call) only runs for Rows.Num() > 0 - an
	// empty inbox would pass this test having exercised almost none of the code #309 is about.
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();
	UFlight* Offer = NewObject<UFlight>(GetTransientPackage());
	Offer->Airframe.Wingspan = 3400.0;
	Offer->ArrivesAt = Clock->Now() + 600.0;
	Offer->ExpiresAt = Clock->Now() + 600.0;
	Offer->AirlineName = FText::FromString(TEXT("Meridian"));
	Offer->TypeName = FText::FromString(TEXT("A320"));
	Board->AddOffer(*Clock, Offer);
	Widget->GetInbox()->Refresh(*Board, *Traffic, *Actor->Network, *Clock);
	if (!TestEqual(TEXT("one offer is on the board"), Widget->GetInbox()->GetOffers().Num(), 1)) { return false; }

	// First paint builds the row - a real cost, not what this test measures.
	Widget->PaintRowsForTest();

	const int32 Before = UAirportMgrUISettings::ResolveCallCountForTest();
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		Widget->PaintRowsForTest();
	}
	const int32 After = UAirportMgrUISettings::ResolveCallCountForTest();

	TestEqual(TEXT("ten idle repaints with nothing on the board changed resolve the style zero "
		"times - PaintRows reads PanelStyle, resolved once at construction, not asked again"),
		After - Before, 0);

	return true;
}

#endif
