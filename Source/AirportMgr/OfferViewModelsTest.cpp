#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "FieldNotification/FieldId.h"
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
	FOfferInboxCountBroadcastsTest,
	"AirportMgr.UI.OfferInbox.PendingCountBroadcasts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOfferInboxCountBroadcastsTest::RunTest(const FString& Parameters)
{
	// THE TEST EXISTS FOR ONE BUG: assigning the member instead of going through
	// UE_MVVM_SET_PROPERTY_VALUE compiles, draws right on the first frame, and then never
	// updates again. Nothing else in the build catches that - not the compiler, not a
	// binding, not a PIE session where the badge merely looks stale.
	URoadNetwork* Net = InboxNetwork();
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>();
	USimClock* Clock = NewObject<USimClock>();
	UFlightBoard* Board = NewObject<UFlightBoard>();
	Board->Allocator = NewObject<UStandAllocator>();

	UOfferInboxViewModel* Inbox = NewObject<UOfferInboxViewModel>();

	int32 Broadcasts = 0;
	const UE::FieldNotification::FFieldId Field =
		UOfferInboxViewModel::FFieldNotificationClassDescriptor::PendingCount;
	Inbox->AddFieldValueChangedDelegate(Field,
		INotifyFieldValueChanged::FFieldValueChangedDelegate::CreateLambda(
			[&Broadcasts](UObject*, UE::FieldNotification::FFieldId) { ++Broadcasts; }));

	Board->AddOffer(InboxOffer(Clock->Now() + 600.0));
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);

	TestEqual(TEXT("the count followed the board"), Inbox->GetPendingCount(), 1);
	TestEqual(TEXT("and it BROADCAST, which is what a binding listens to"), Broadcasts, 1);

	// A refresh that changes nothing must not broadcast: the macro compares before it sets,
	// and a row rebuilt every tick would drop the list view's selection.
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("an unchanged count does not broadcast again"), Broadcasts, 1);

	Board->AddOffer(InboxOffer(Clock->Now() + 900.0));
	Inbox->Refresh(*Board, *Traffic, *Net, *Clock);
	TestEqual(TEXT("a second offer moves the count"), Inbox->GetPendingCount(), 2);
	TestEqual(TEXT("and broadcasts once more"), Broadcasts, 2);
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
	Board->AddOffer(First);
	Board->AddOffer(Second);

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

#endif
