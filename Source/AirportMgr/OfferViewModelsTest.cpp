#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "OfferViewModels.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	URoadNetwork* InboxNetwork()
	{
		URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Net->PlaceEntity(Stand, Stand->Anchors, FVector2D::ZeroVector, 0.0, 3600.0,
			Stand->PoseRole, Stand->Trucks);
		return Net;
	}

	UFlight* InboxOffer(double ArrivesAt)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Airframe.Wingspan = 3400.0;
		Flight->ArrivesAt = ArrivesAt;
		Flight->ExpiresAt = ArrivesAt;
		Flight->AirlineName = FText::FromString(TEXT("Meridian"));
		Flight->TypeName = FText::FromString(TEXT("A320"));
		return Flight;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxCountUpdatesEachRefreshTest,
	"AirportMgr.UI.OfferInbox.PendingCountUpdatesEachRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxCountUpdatesEachRefreshTest::RunTest(const FString& Parameters)
{
	// RENAMED AND REWRITTEN (issue #191): this used to prove PendingCount was set through
	// UE_MVVM_SET_PROPERTY_VALUE rather than a plain assignment, by counting how many times
	// a bound delegate fired - the bug being that a plain assignment "compiles, draws right
	// on the first frame, and then never updates again" under a real MVVM binding. #191
	// dropped UMVVMViewModelBase because nothing ever bound that field (no Content/UI
	// Blueprint exists), so plain assignment is now the ONLY mechanism, and the thing worth
	// pinning is simply that GetPendingCount() tracks the board across repeated refreshes -
	// see OfferInboxWidget::PaintRows, which is what actually reads this every tick.
	URoadNetwork* Net = InboxNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UOfferInboxViewModel* Inbox = NewObject<UOfferInboxViewModel>();

	Board->AddOffer(*Clock, InboxOffer(Clock->Now() + 600.0));
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("the count followed the board"), Inbox->GetPendingCount(), 1);

	// A refresh that changes nothing must still read back correctly - the row-set gate (see
	// UOfferInboxViewModel::Refresh's comment) must not stop the count from being current.
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("an unchanged board leaves the count unchanged"), Inbox->GetPendingCount(), 1);

	Board->AddOffer(*Clock, InboxOffer(Clock->Now() + 900.0));
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("a second offer moves the count"), Inbox->GetPendingCount(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxOneRowListTest,
	"AirportMgr.UI.OfferInbox.OneRowList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxOneRowListTest::RunTest(const FString& Parameters)
{
	// ISSUE #191: Rows used to have a hand-mirrored Offers array kept in step at every place
	// either changed - CLAUDE.md's "lists that must agree are one list". GetOffers() now
	// builds its raw-pointer view from Rows on demand, so RowsForTest() (the source of
	// truth) and GetOffers() (what the list view and Accept/DeclineRow index into) can never
	// disagree - there is exactly one list, not two a caller could forget to keep in sync.
	URoadNetwork* Net = InboxNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	Board->AddOffer(*Clock, InboxOffer(Clock->Now() + 600.0));
	Board->AddOffer(*Clock, InboxOffer(Clock->Now() + 900.0));

	UOfferInboxViewModel* Inbox = NewObject<UOfferInboxViewModel>();
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);

	TestEqual(TEXT("the list view's item count agrees with the one row list"),
		Inbox->GetOffers().Num(), Inbox->RowsForTest().Num());

	Inbox->Accept(Inbox->GetOffers()[0]);
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);

	TestEqual(TEXT("and still agrees once a row has left - nothing to fall out of step"),
		Inbox->GetOffers().Num(), Inbox->RowsForTest().Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxAcceptGoesThroughTheBoardTest,
	"AirportMgr.UI.OfferInbox.AcceptGoesThroughTheBoard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxAcceptGoesThroughTheBoardTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = InboxNetwork();   // ONE stand
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UFlight* First = InboxOffer(Clock->Now() + 600.0);
	UFlight* Second = InboxOffer(Clock->Now() + 600.0);
	Board->AddOffer(*Clock, First);
	Board->AddOffer(*Clock, Second);

	UOfferInboxViewModel* Inbox = NewObject<UOfferInboxViewModel>();
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("two rows"), Inbox->GetOffers().Num(), 2);

	TestTrue(TEXT("accepting the first row accepts the flight"),
		Inbox->Accept(Inbox->GetOffers()[0]));
	TestEqual(TEXT("and the board moved it, which is the one door working"),
		First->Phase, EFlightPhase::Accepted);
	TestEqual(TEXT("so one offer is left"), Inbox->GetPendingCount(), 1);

	// The remaining offer cannot be accepted - the only stand is held - and the row must say
	// so rather than offering a button that silently fails.
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	UOfferViewModel* Row = Inbox->GetOffers().Num() > 0 ? Inbox->GetOffers()[0] : nullptr;
	if (!TestNotNull(TEXT("a row for the second offer"), Row)) { return false; }
	TestFalse(TEXT("it is shown un-acceptable"), Row->IsAcceptable());
	TestFalse(TEXT("with a reason in it"), Row->GetRefusal().IsEmpty());

	TestFalse(TEXT("and accepting it is refused"), Inbox->Accept(Row));
	TestEqual(TEXT("leaving the flight in the inbox"), Second->Phase, EFlightPhase::Offered);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxWhyNotAcceptablePolledOnceThenCachedTest,
	"AirportMgr.UI.OfferInbox.WhyNotAcceptablePolledOnceThenCached",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxWhyNotAcceptablePolledOnceThenCachedTest::RunTest(const FString& Parameters)
{
	// ISSUE #169: WhyNotAcceptable is a full ArrivalPlanner::Plan - a route search per stand,
	// then per runway exit - and NativeTick used to call it for every row, every frame, with
	// nothing gating it. A quiet inbox (no offer added, accepted or declined; no graph edit;
	// no occupancy change) must call it ONCE PER ROW, ever, not once per row per tick -
	// UFlightBoard::GetWhyNotAcceptableCallsForTest counts the search itself, not the cached
	// answer, so a fix that merely returned the same Why without re-deriving it would still
	// fail this.
	URoadNetwork* Net = InboxNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	constexpr int32 OfferCount = 3;
	for (int32 Index = 0; Index < OfferCount; ++Index)
	{
		Board->AddOffer(*Clock, InboxOffer(Clock->Now() + 600.0));
	}

	UOfferInboxViewModel* Inbox = NewObject<UOfferInboxViewModel>();

	constexpr int32 Ticks = 5;
	for (int32 Tick = 0; Tick < Ticks; ++Tick)
	{
		Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	}

	TestEqual(TEXT("every offer got a row"), Inbox->GetOffers().Num(), OfferCount);
	TestEqual(TEXT("N offers over K quiet ticks cost N searches, not N*K"),
		Board->GetWhyNotAcceptableCallsForTest(), OfferCount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOfferInboxWhyNotAcceptableRecomputesOnEachRevisionTest,
	"AirportMgr.UI.OfferInbox.WhyNotAcceptableRecomputesOnEachRevision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxWhyNotAcceptableRecomputesOnEachRevisionTest::RunTest(const FString& Parameters)
{
	// THE OTHER HALF of the #169 fix: a cache that never invalidated would be as wrong as one
	// that never cached. One row is watched through a bump of each of the three things
	// UOfferViewModel gates on - the board, the guideline graph, and occupancy - and each
	// bump must cost EXACTLY one more search of that one row, no more (a bump the row does
	// not depend on leaking into a recompute) and no less (a bump it does depend on being
	// missed).
	URoadNetwork* Net = InboxNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	Board->AddOffer(*Clock, InboxOffer(Clock->Now() + 600.0));

	UOfferInboxViewModel* Inbox = NewObject<UOfferInboxViewModel>();
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("the one row's answer was searched once"),
		Board->GetWhyNotAcceptableCallsForTest(), 1);

	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("a repeat tick with nothing changed does not search again"),
		Board->GetWhyNotAcceptableCallsForTest(), 1);

	// 1. THE BOARD. A second offer added and immediately declined never becomes a row - Rows
	//    ends this call exactly as it started - but UFlightBoard::Revision moved twice, and the
	//    surviving row's own cached revision is now behind it.
	UFlight* Transient = InboxOffer(Clock->Now() + 900.0);
	Board->AddOffer(*Clock, Transient);
	Board->Decline(*Clock, *Transient);
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("still one row"), Inbox->GetOffers().Num(), 1);
	TestEqual(TEXT("a board revision the row depends on bumped it, once"),
		Board->GetWhyNotAcceptableCallsForTest(), 2);

	// 2. THE GUIDELINE GRAPH. Net.AddGuidelineNode is the same mutator NodeReachTest's own
	//    "adding an edge bumped the revision" case uses - a hand-drawn edit, nothing to do
	//    with this flight or this board.
	const uint32 GuidelineBefore = Net->GetGuidelineRevision();
	Net->AddGuidelineNode(FVector2D(50000.0, 50000.0), /*bDerived=*/false);
	TestTrue(TEXT("the graph edit bumped the guideline revision"),
		Net->GetGuidelineRevision() > GuidelineBefore);
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("a guideline revision the row depends on bumped it, once"),
		Board->GetWhyNotAcceptableCallsForTest(), 3);

	// 3. OCCUPANCY. HoldStand with no agent at all - the AirportOps reservation path, not a
	//    dispatch - is enough: it is one of the two things ArrivalPlanner::Plan actually reads
	//    off the occupancy table (see UGroundTraffic::OccupancyRevision's own comment).
	FGuidelineNodeId SomeStand;
	SomeStand.Index = 99;
	TestTrue(TEXT("the hold was granted"), Traffic->HoldStand(-1, SomeStand));
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("an occupancy revision the row depends on bumped it, once"),
		Board->GetWhyNotAcceptableCallsForTest(), 4);

	// A last quiet tick proves the cache closed again rather than staying open after one miss.
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("and a further quiet tick still does not search again"),
		Board->GetWhyNotAcceptableCallsForTest(), 4);
	return true;
}

#endif
