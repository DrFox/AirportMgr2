#include "CoreMinimal.h"
#include "Components/VerticalBox.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
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
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FWorldContext& Ctx = GEngine->CreateNewWorldContext(EWorldType::Game);
	Ctx.SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };

	ARoadNetworkActor* Actor = World->SpawnActor<ARoadNetworkActor>();
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
		Board->AddOffer(Offer);
	}

	Widget->GetInbox()->Refresh(*Board, *Traffic, *Actor->Network, *Clock);
	TestEqual(TEXT("the viewmodel has a row per offer"), Widget->GetInbox()->GetOffers().Num(), 2);
	TestEqual(TEXT("and the badge count agrees with it"), Widget->GetInbox()->GetPendingCount(), 2);

	// Accepting through the widget's own entry point is what a button click does, so this is
	// the click path without a click.
	Widget->AcceptRow(0);
	TestEqual(TEXT("accepting a row takes it out of the inbox"),
		Widget->GetInbox()->GetPendingCount(), 1);
	return true;
}

#endif
