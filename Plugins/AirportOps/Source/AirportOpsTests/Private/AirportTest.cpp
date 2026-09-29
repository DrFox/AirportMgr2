#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/AirlineRoster.h"
#include "Model/Airport.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSave.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE AIRPORT'S STATUS (spec 2026-09-29-ops-batch3 §3): open, closed by the player, or without a runway -
// derived from the player's intent and the network, and what entering a closed status cancels. World-free
// model tests first, then the composition through UOpsRuntime, which is where every seam is wired.

namespace
{
	/** A field with a runway and Stands stands - the arrival fixture every board test uses. */
	FTestAirport AirportTestField(int32 Stands = 1)
	{
		FTestAirportOptions Options;
		Options.StandCount = Stands;
		return FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), Options);
	}

	URoadNetwork* AirportTestEmptyNetwork()
	{
		return NewObject<URoadNetwork>(GetTransientPackage());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportStatusDerivedTest, "AirportOps.Model.Airport.StatusIsDerived",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportStatusDerivedTest::RunTest(const FString&)
{
	const FTestAirport Field = AirportTestField();
	URoadNetwork* Empty = AirportTestEmptyNetwork();
	UAirport* Airport = NewObject<UAirport>(GetTransientPackage());

	Airport->Reseat(*Empty);
	TestEqual(TEXT("no runway: NoRunway - an airport without a runway is not an airport"), Airport->Status(), EAirportStatus::NoRunway);
	Airport->Refresh(*Field.Net);
	TestEqual(TEXT("a runway: Open"), Airport->Status(), EAirportStatus::Open);

	Airport->SetClosedByPlayer(true, *Field.Net);
	TestEqual(TEXT("closed by the player: ClosedByPlayer, at once - the command re-derives"), Airport->Status(), EAirportStatus::ClosedByPlayer);
	Airport->Refresh(*Empty);
	TestEqual(TEXT("the runway gone as well: STILL ClosedByPlayer - the player's intent wins"), Airport->Status(), EAirportStatus::ClosedByPlayer);
	Airport->Refresh(*Field.Net);
	TestEqual(TEXT("a runway back: still closed - only the player reopens"), Airport->Status(), EAirportStatus::ClosedByPlayer);

	Airport->SetClosedByPlayer(false, *Empty);
	TestEqual(TEXT("reopened with no runway: NoRunway"), Airport->Status(), EAirportStatus::NoRunway);
	Airport->SetClosedByPlayer(false, *Field.Net);
	TestEqual(TEXT("and with one: Open"), Airport->Status(), EAirportStatus::Open);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportChangePublishesTest, "AirportOps.Model.Airport.ChangePublishesOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportChangePublishesTest::RunTest(const FString&)
{
	const FTestAirport Field = AirportTestField();
	URoadNetwork* Empty = AirportTestEmptyNetwork();
	FOpsEventBus Bus;
	TArray<FAirportStatusChangedEvent> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FAirportStatusChangedEvent>(EOpsTier::Sim, TEXT("test"), [&Seen](const FAirportStatusChangedEvent& E) { Seen.Add(E); });
	Bus.EndWiring();
	UAirport* Airport = NewObject<UAirport>(GetTransientPackage());
	Airport->Bus = &Bus;

	Airport->Reseat(*Empty);
	Bus.Drain();
	TestEqual(TEXT("a reseat (an attach, a load) is not a change the player made: no event"), Seen.Num(), 0);
	TestEqual(TEXT("but it does re-derive"), Airport->Status(), EAirportStatus::NoRunway);

	TestTrue(TEXT("a runway built: a change"), Airport->Refresh(*Field.Net));
	Bus.Drain();
	if (!TestEqual(TEXT("published once"), Seen.Num(), 1)) { return false; }
	TestEqual(TEXT("from"), Seen[0].Old, EAirportStatus::NoRunway);
	TestEqual(TEXT("to"), Seen[0].New, EAirportStatus::Open);

	TestFalse(TEXT("the same network again: no change"), Airport->Refresh(*Field.Net));
	Bus.Drain();
	TestEqual(TEXT("and no event"), Seen.Num(), 1);

	TestTrue(TEXT("the command changes it"), Airport->SetClosedByPlayer(true, *Field.Net));
	TestFalse(TEXT("closing a closed airport changes nothing"), Airport->SetClosedByPlayer(true, *Field.Net));
	Bus.Drain();
	if (!TestEqual(TEXT("one event for the command, none for the repeat"), Seen.Num(), 2)) { return false; }
	TestEqual(TEXT("to ClosedByPlayer"), Seen[1].New, EAirportStatus::ClosedByPlayer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportSaveTest, "AirportOps.Model.Airport.SaveKeepsClosedByPlayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportSaveTest::RunTest(const FString&)
{
	const FTestAirport Field = AirportTestField();
	UAirport* Saved = NewObject<UAirport>(GetTransientPackage());
	Saved->SetClosedByPlayer(true, *Field.Net);
	FOpsSnapshot Snapshot;
	OpsSave::CaptureBlob(*Saved, Snapshot);
	TestTrue(TEXT("its blob is keyed \"Airport\""), Snapshot.Blobs.Contains(TEXT("Airport")));

	UAirport* Loaded = NewObject<UAirport>(GetTransientPackage());
	OpsSave::RestoreBlob(Snapshot, *Loaded);
	TestTrue(TEXT("the player's intent survives a save"), Loaded->IsClosedByPlayer());
	Loaded->Reseat(*Field.Net);
	TestEqual(TEXT("and the status re-derives from it"), Loaded->Status(), EAirportStatus::ClosedByPlayer);

	UAirport* Older = NewObject<UAirport>(GetTransientPackage());
	Older->SetClosedByPlayer(true, *Field.Net);
	OpsSave::RestoreBlob(FOpsSnapshot(), *Older);
	TestFalse(TEXT("a snapshot with no \"Airport\" blob is an open airport, whatever the session had"), Older->IsClosedByPlayer());
	return true;
}

namespace
{
	/** A board over a two-stand field, publishing onto a bus with recorders. 1 game s per real s. */
	struct FAirportBoardRig
	{
		FTestAirport Field;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		FOpsEventBus Bus;
		TArray<FFlightCancelledEvent> Cancelled;
		TArray<FOfferExpiredEvent> Expired;

		FAirportBoardRig()
		{
			Field = AirportTestField(2);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Clock->SetUniformDay(USimClock::SecondsPerDay);
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Board->Bus = &Bus;
			Bus.BeginWiring();
			Bus.Subscribe<FFlightCancelledEvent>(EOpsTier::Sim, TEXT("test"), [this](const FFlightCancelledEvent& E) { Cancelled.Add(E); });
			Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Sim, TEXT("test"), [this](const FOfferExpiredEvent& E) { Expired.Add(E); });
			Bus.EndWiring();
		}

		UFlight* Offer(FName Airline = TEXT("Cumbria"))
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = UAirsideSettings::ResolveDefaultAirframe();
			Flight->AirlineId = Airline;
			Flight->OfferWindowSeconds = 60.0;
			Flight->OfferSecondsLeft = 60.0;
			Flight->ApproachFocus = Field.Threshold;
			Board->AddOffer(*Clock, Flight);
			return Flight;
		}

		UFlight* Accepted(double Lead)
		{
			UFlight* Flight = Offer();
			Flight->LeadTimeSeconds = Lead;
			return Board->Accept(*Traffic, *Field.Net, *Clock, *Flight) ? Flight : nullptr;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportCancelUnarrivedTest, "AirportOps.Model.FlightBoard.CancelUnarrivedCancelsAndWithdraws",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportCancelUnarrivedTest::RunTest(const FString&)
{
	FAirportBoardRig R;
	// INBOUND: accepted with no lead, so its ETA callback puts it in the queue on the next advance.
	UFlight* Holding = R.Accepted(0.0);
	if (!TestNotNull(TEXT("the first accept holds a stand"), Holding)) { return false; }
	R.Clock->Advance(1.0);
	if (!TestEqual(TEXT("it is holding for the runway"), Holding->Phase, EFlightPhase::Inbound)) { return false; }
	// ACCEPTED: a long lead, so it is still on the clock when the airport closes.
	UFlight* Coming = R.Accepted(100000.0);
	if (!TestNotNull(TEXT("the second accept holds the other stand"), Coming)) { return false; }
	UFlight* Offered = R.Offer();
	TestFalse(TEXT("CONTROL: both stands are held, so a third flight cannot be accepted"),
		R.Board->Accept(*R.Traffic, *R.Field.Net, *R.Clock, *Offered));
	// LANDING: committed to the runway - an aircraft already in the world.
	UFlight* Landing = NewObject<UFlight>(GetTransientPackage());
	Landing->AgentId = 5;
	Landing->Phase = EFlightPhase::Landing;
	R.Board->AddOffer(*R.Clock, Landing);
	TestEqual(TEXT("two flights would be cancelled - the confirm's N"), R.Board->UnarrivedCount(), 2);
	TestEqual(TEXT("one aircraft is committed to the runway - the drain readout"), R.Board->OnGroundCount(), 1);

	TestEqual(TEXT("the close cancels the two unarrived flights"), R.Board->CancelUnarrived(*R.Traffic, *R.Clock, ECancelReason::AirportClosed), 2);
	TestEqual(TEXT("Inbound -> Cancelled"), Holding->Phase, EFlightPhase::Cancelled);
	TestEqual(TEXT("Accepted -> Cancelled"), Coming->Phase, EFlightPhase::Cancelled);
	TestEqual(TEXT("Offered -> Withdrawn, not Expired: nobody let it lapse"), Offered->Phase, EFlightPhase::Withdrawn);
	TestEqual(TEXT("Landing untouched: it is on the runway and finishes"), Landing->Phase, EFlightPhase::Landing);
	TestEqual(TEXT("the queue is empty"), R.Board->Queue().Num(), 0);
	TestEqual(TEXT("the inbox is empty"), R.Board->PendingOfferCount(), 0);
	TestEqual(TEXT("nothing left to cancel"), R.Board->UnarrivedCount(), 0);

	R.Bus.Drain();
	if (!TestEqual(TEXT("one FlightCancelled per cancelled flight - a withdrawal is not one"), R.Cancelled.Num(), 2)) { return false; }
	TestTrue(TEXT("each with its reason"), R.Cancelled[0].Reason == ECancelReason::AirportClosed && R.Cancelled[1].Reason == ECancelReason::AirportClosed);
	TestEqual(TEXT("naming its airline"), R.Cancelled[0].AirlineId, FName(TEXT("Cumbria")));
	TestEqual(TEXT("and no OfferExpired, so no Ignored penalty"), R.Expired.Num(), 0);

	// THE HOLDS WENT BACK: the offer refused above for want of a stand is acceptable now.
	UFlight* After = R.Offer();
	After->LeadTimeSeconds = 1.0e7;
	TestTrue(TEXT("both stands are free again"), R.Board->Accept(*R.Traffic, *R.Field.Net, *R.Clock, *After));
	// THE ARRIVAL WAS DISARMED: the cancelled flight's ETA passing must not put it back in the queue.
	R.Clock->Advance(200000.0);
	TestEqual(TEXT("its clock entry is gone - still Cancelled after its ETA"), Coming->Phase, EFlightPhase::Cancelled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportCancelByAgentTest, "AirportOps.Model.FlightBoard.CancelByAgentPublishesUnstuck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportCancelByAgentTest::RunTest(const FString&)
{
	// THE SECOND PUBLISHER: a despawned aeroplane's flight is cancelled too, and the roster hears it - for nothing.
	FAirportBoardRig R;
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->AirlineId = TEXT("Cumbria");
	Flight->AgentId = 9;
	Flight->Phase = EFlightPhase::TaxiIn;
	R.Board->AddOffer(*R.Clock, Flight);
	TestTrue(TEXT("agent 9's flight is cancelled"), R.Board->CancelByAgent(9, 0.0));
	R.Bus.Drain();
	if (!TestEqual(TEXT("published once"), R.Cancelled.Num(), 1)) { return false; }
	TestEqual(TEXT("naming the flight"), R.Cancelled[0].FlightId, Flight->Id);
	TestEqual(TEXT("its airline"), R.Cancelled[0].AirlineId, FName(TEXT("Cumbria")));
	TestEqual(TEXT("and why"), R.Cancelled[0].Reason, ECancelReason::Unstuck);
	return true;
}

// ---------------------------------------------------------------------------------------------------------
// COMPOSITION: the runtime's own wiring.

namespace
{
	/** An attached runtime over a world with a road and, when bRunway, a runway placed BEFORE the attach - so the
	 *  attach's silent re-derive already finds it. Settled: a few frames drained. */
	UOpsRuntime* AirportTestRuntime(FAirsideTestWorld& World, bool bRunway)
	{
		ARoadNetworkActor* Actor = World.Actor;
		const int32 A = Actor->PlaceNode(FVector2D(0.0, 30000.0));
		const int32 B = Actor->PlaceNode(FVector2D(20000.0, 30000.0));
		Actor->ConnectNodes(A, B);
		if (bRunway)
		{
			Actor->MinimumRunwayLength = 100.0;
			Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), TestProfiles::Runway());
		}
		UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
		Runtime->Attach(Actor);
		for (int32 Tick = 0; Tick < 3; ++Tick) { Runtime->Tick(0.0); }
		return Runtime;
	}

	/** A stand the allocator can hold, clear of everything else. */
	void AirportTestStand(URoadNetwork& Net)
	{
		UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
		Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 90000.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);
	}

	/** An offer of Airline added to the runtime's board, with a lead long enough never to come due in a test. */
	UFlight* AirportTestOffer(UOpsRuntime& Runtime, FName Airline)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Airframe.Wingspan = 3400.0;
		Flight->AirlineId = Airline;
		Flight->OfferWindowSeconds = 1.0e6;
		Flight->OfferSecondsLeft = 1.0e6;
		Flight->LeadTimeSeconds = 1.0e7;
		Runtime.GetFlightBoard()->AddOffer(*Runtime.GetClock(), Flight);
		return Flight;
	}

	int32 AirportTestAlertsOf(const UOpsRuntime& Runtime, EAlertKind Kind)
	{
		return Runtime.GetAlerts()->GetAlerts().FilterByPredicate([Kind](const FOpsAlert& A) { return A.Key.Kind == Kind; }).Num();
	}

	/** Delete runway segments, last first, until the network has none. Bounded by the segment count. */
	void AirportTestDeleteRunways(ARoadNetworkActor& Actor)
	{
		for (int32 Index = Actor.Network->GetSegments().Num() - 1; Index >= 0 && UAirport::HasRunway(*Actor.Network); --Index)
		{
			Actor.DeleteSegment(Index);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportCloseCancelsTest, "AirportOps.Present.Airport.CloseCancelsThroughTheBus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportCloseCancelsTest::RunTest(const FString&)
{
	// THE CHAIN THROUGH THE RUNTIME: the command re-derives the status; the change is published; the flight board's
	// Sim handler cancels what has not arrived; the roster, hearing each cancellation, charges the airline.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportTestRuntime(TestWorld, /*bRunway=*/true);
	if (!TestEqual(TEXT("a field with a runway opens"), Runtime->GetAirport()->Status(), EAirportStatus::Open)) { return false; }
	URoadNetwork* Net = TestWorld.Actor->Network;
	AirportTestStand(*Net);
	Runtime->Tick(0.0);

	const FName Airline = TEXT("AirportTestCloseAirline");
	Runtime->GetAirlines()->Ensure(Airline);
	// THE SCENARIO ASSET CARRIES THE NEW FIELD: DA_Scenario_Default stores only deltas, so a field added since it
	// was saved loads its constructor default. A zero here would make the drop below true with nothing wired.
	if (!TestTrue(TEXT("the scenario's closure penalty is a real cost"), Runtime->GetAirlines()->Tuning.ClosureCancelPenalty > 0.0)) { return false; }
	UFlight* Coming = AirportTestOffer(*Runtime, Airline);
	UGroundTraffic* Model = TestWorld.Actor->GetTraffic()->GetModel();
	if (!TestTrue(TEXT("an accepted flight"), Runtime->GetFlightBoard()->Accept(*Model, *Net, *Runtime->GetClock(), *Coming))) { return false; }
	UFlight* Offered = AirportTestOffer(*Runtime, Airline);
	Runtime->Tick(0.0);
	const double Before = Runtime->GetAirlines()->Find(Airline)->Satisfaction;

	TestTrue(TEXT("the command closes it"), Runtime->SetAirportClosed(true));
	TestEqual(TEXT("at once - a command re-derives, it does not wait for an event"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	TestEqual(TEXT("but the cancellation is the change's, on the next drain"), Coming->Phase, EFlightPhase::Accepted);
	Runtime->Tick(0.0);
	TestEqual(TEXT("the accepted flight is cancelled"), Coming->Phase, EFlightPhase::Cancelled);
	TestEqual(TEXT("the offer withdrawn"), Offered->Phase, EFlightPhase::Withdrawn);
	TestEqual(TEXT("and its airline charged the closure penalty, once"), Runtime->GetAirlines()->Find(Airline)->Satisfaction,
		Before - Runtime->GetAirlines()->Tuning.ClosureCancelPenalty, 1e-9);

	TestTrue(TEXT("reopening is the same command"), Runtime->SetAirportClosed(false));
	TestEqual(TEXT("open again"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportDirtiesAlertsTest, "AirportOps.Present.Airport.StatusChangeDirtiesAlerts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportDirtiesAlertsTest::RunTest(const FString&)
{
	// THE ALERTS PASS HEARS A STATUS CHANGE - a closure has no network change of its own, and it is what turns the
	// airline alerts off (they are judged only while Open). Measured as the pass running, on an empty board, so
	// no cancellation can be what dirtied it.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportTestRuntime(TestWorld, /*bRunway=*/true);
	const int32 Settled = Runtime->GetAlerts()->RecomputeCountForTest();
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("a quiet frame runs no alerts pass"), Runtime->GetAlerts()->RecomputeCountForTest(), Settled)) { return false; }
	Runtime->SetAirportClosed(true);
	Runtime->Tick(0.0);
	TestEqual(TEXT("the status change ran the alerts pass once"), Runtime->GetAlerts()->RecomputeCountForTest(), Settled + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportRunwayComesAndGoesTest, "AirportOps.Present.Airport.RunwayComesAndGoes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportRunwayComesAndGoesTest::RunTest(const FString&)
{
	// A NEW GAME WITH NO RUNWAY: no offers, one alert, and no airline asked anything (so no AirlineCannotCome, and
	// no toast per airline). Then a runway built opens it; deleting it closes it again.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportTestRuntime(TestWorld, /*bRunway=*/false);
	TestEqual(TEXT("a runway-less field has no runway"), Runtime->GetAirport()->Status(), EAirportStatus::NoRunway);
	TestEqual(TEXT("and says so, once"), AirportTestAlertsOf(*Runtime, EAlertKind::NoRunway), 1);

	const double OneMinute = UOfferGenerator::TickSeconds / Runtime->GetClock()->TimeScale() * 1.01;
	Runtime->Tick(OneMinute);
	TestTrue(TEXT("CONTROL: a generator minute ran"), Runtime->OfferTicksForTest() > 0);
	TestEqual(TEXT("and asked no airline whether it could come"), Runtime->GetOfferGenerator()->AdmissionChecksForTest(), 0);
	TestEqual(TEXT("so no airline alert"), AirportTestAlertsOf(*Runtime, EAlertKind::AirlineCannotCome), 0);
	TestEqual(TEXT("and no offer"), Runtime->GetFlightBoard()->PendingOfferCount(), 0);

	ARoadNetworkActor* Actor = TestWorld.Actor;
	Actor->MinimumRunwayLength = 100.0;
	if (!TestTrue(TEXT("a runway is built"), Actor->PlaceRunway(FVector2D(0.0, -50000.0), FVector2D(6000.0, -50000.0), TestProfiles::Runway()))) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("a runway opens it"), Runtime->GetAirport()->Status(), EAirportStatus::Open);
	TestEqual(TEXT("and the alert clears"), AirportTestAlertsOf(*Runtime, EAlertKind::NoRunway), 0);

	AirportTestDeleteRunways(*Actor);
	if (!TestFalse(TEXT("the runway is deleted"), UAirport::HasRunway(*Actor->Network))) { return false; }
	Runtime->Tick(0.0);
	TestEqual(TEXT("deleting the last runway: NoRunway"), Runtime->GetAirport()->Status(), EAirportStatus::NoRunway);
	TestEqual(TEXT("and the alert is back"), AirportTestAlertsOf(*Runtime, EAlertKind::NoRunway), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportLoadTest, "AirportOps.Present.Airport.LoadRederivesWithoutCancelling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportLoadTest::RunTest(const FString&)
{
	// A LOAD RE-DERIVES AND NEVER RE-CANCELS (spec §3 "Load"): everything a closure cancels was cancelled when it
	// was saved. To make a re-run VISIBLE, the saved closed airport carries an Accepted flight planted by hand -
	// a load that re-ran the cancellation would cancel it.
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UOpsRuntime* Runtime = AirportTestRuntime(TestWorld, /*bRunway=*/true);
	Runtime->SetAirportClosed(true);
	Runtime->Tick(0.0);
	UFlight* Planted = NewObject<UFlight>(GetTransientPackage());
	Planted->Phase = EFlightPhase::Accepted;
	Planted->ArrivesAt = 1.0e9;
	Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Planted);
	const int32 PlantedId = Planted->Id;
	const FString Slot = TEXT("AirportOpsTest_AirportLoad");
	if (!TestTrue(TEXT("save writes"), Runtime->SaveToSlot(Slot))) { return false; }

	Runtime->SetAirportClosed(false);
	Runtime->Tick(0.0);
	if (!TestEqual(TEXT("reopened before the load"), Runtime->GetAirport()->Status(), EAirportStatus::Open)) { return false; }

	if (!TestTrue(TEXT("load reads"), Runtime->LoadFromSlot(Slot))) { return false; }
	TestEqual(TEXT("the loaded airport is closed, re-derived by the load itself"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	for (int32 Tick = 0; Tick < 3; ++Tick) { Runtime->Tick(0.0); }
	const UFlight* Restored = Runtime->GetFlightBoard()->FindByIdForTest(PlantedId);
	if (!TestNotNull(TEXT("the planted flight is restored"), Restored)) { return false; }
	TestEqual(TEXT("and NOT cancelled - the load published no status change"), Restored->Phase, EFlightPhase::Accepted);
	return true;
}

namespace
{
	/** The fuel fixture's guideline line, as UnfuelledDepartureLowersAirline lays it. Prefixed: unity build. */
	void AirportTestLayLine(URoadNetwork& Net, const FVector2D& From, const FVector2D& To, FGuidelineNodeId& OutA, FGuidelineNodeId& OutB)
	{
		OutA = Net.AddGuidelineNode(From);
		OutB = Net.AddGuidelineNode(To);
		FGuidelineEdge Edge;
		Edge.A = OutA;
		Edge.B = OutB;
		Edge.Control = (From + To) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
		Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
		Edge.Direction = EGuidelineDir::Bidirectional;
		Edge.Width = 600.0;
		Edge.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(Edge));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirportDrainsTest, "AirportOps.Present.Airport.ClosedAirportDrains",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirportDrainsTest::RunTest(const FString&)
{
	// CLOSED IS NOT FROZEN (spec §0): aircraft already on the ground are serviced and depart until the airport is
	// empty. The field is UnfuelledDepartureLowersAirline's: a taxiway, a runway north of it, a stand.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("an actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 60000.0));
	if (!TestNotNull(TEXT("a network"), Actor->Network.Get())) { return false; }
	URoadNetwork& Net = *Actor->Network;
	FGuidelineNodeId TaxiSouth, TaxiNorth;
	AirportTestLayLine(Net, FVector2D(-10000.0, -10000.0), FVector2D(-10000.0, 10000.0), TaxiSouth, TaxiNorth);
	URoadProfile* Strip = TestProfiles::Runway();
	const FRoadNodeId West = Net.AddNode(FVector2D(-50000.0, 20000.0));
	const FRoadNodeId Mid = Net.AddNode(FVector2D(-10000.0, 20000.0));
	const FRoadNodeId East = Net.AddNode(FVector2D(50000.0, 20000.0));
	Net.AddStraightSegment(West, Mid, Strip);
	Net.AddStraightSegment(Mid, East, Strip);
	const FGuidelineNodeId OnStrip = Net.AddGuidelineNode(FVector2D(-10000.0, 20000.0), false);
	{
		FGuidelineEdge ToStrip;
		ToStrip.A = TaxiNorth;
		ToStrip.B = OnStrip;
		ToStrip.Control = FVector2D(-10000.0, 15000.0);
		ToStrip.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
		ToStrip.AllowedTraffic.Add(ETraversalClass::Emergency);
		ToStrip.Direction = EGuidelineDir::Bidirectional;
		ToStrip.Width = 600.0;
		ToStrip.bDerived = true;
		Net.AddGuidelineEdge(MoveTemp(ToStrip));
	}
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Stand = Net.PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0,
		3600.0, StandDef->PoseRole, StandDef->Trucks);
	FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());
	Net.MarkGuidelinesDerived();

	UOpsRuntime* Runtime = NewObject<UOpsRuntime>();
	Runtime->Attach(Actor);
	if (!TestEqual(TEXT("the field has a runway, so it opens"), Runtime->GetAirport()->Status(), EAirportStatus::Open)) { return false; }

	const FRoutePlan Plan = TestGraph::Probe(Net, TaxiSouth, Net.GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("the aircraft routes to the stand"), Plan.IsValid())) { return false; }
	FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Airframe.TurnaroundSeconds = 60.0;
	if (!TestTrue(TEXT("and dispatches"), Actor->DispatchAgent(Plan, Airframe))) { return false; }
	const int32 Aircraft = Actor->GetTraffic()->GetNewestAgentId();
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->AgentId = Aircraft;
	Flight->Phase = EFlightPhase::TaxiIn;
	Flight->Airframe = Airframe;
	Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);

	if (!TestTrue(TEXT("closed with an aircraft on the ground"), Runtime->SetAirportClosed(true))) { return false; }
	TestEqual(TEXT("which the bar reads as draining one"), Runtime->GetFlightBoard()->OnGroundCount(), 1);

	constexpr float Step = 1.0f / 30.0f;
	bool bEverCancelled = false;
	for (int32 Tick = 0; Tick < 30 * 300 && Runtime->GetFlightBoard()->OnGroundCount() > 0; ++Tick)
	{
		Actor->Tick(Step);
		Runtime->Tick(Step);
		bEverCancelled |= Flight->Phase == EFlightPhase::Cancelled;
	}
	TestFalse(TEXT("a flight on the ground is never cancelled by a closure"), bEverCancelled);
	TestEqual(TEXT("the closed airport drained: no aircraft left on the ground"), Runtime->GetFlightBoard()->OnGroundCount(), 0);
	TestEqual(TEXT("the flight left by the runway"), Flight->Phase, EFlightPhase::Departed);
	TestEqual(TEXT("and the airport is still closed"), Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer);
	return true;
}

#endif
