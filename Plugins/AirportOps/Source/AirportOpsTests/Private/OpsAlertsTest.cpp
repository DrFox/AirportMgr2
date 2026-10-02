#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/Airport.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsAlerts.h"
#include "Model/OpsDefinition.h"
#include "Present/OpsRuntime.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Profiles/RoadProfile.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

// STANDING ALERTS, DERIVED FROM STATE (ops alerts spec 2026-09-29 §1): each kind is raised while its
// condition is true and cleared the first recompute after it is not - never by a paired "cleared" event
// a site could forget to send. World-free: the boards and traffic are NewObject'd, as AgentRescueTest's
// FRescueField builds them.

namespace
{
	struct FAlertsField
	{
		FTestAirport Airport;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		UJobBoard* Jobs = nullptr;
		UOfferGenerator* Offers = nullptr;
		ULedger* Ledger = nullptr;
		UOpsAlerts* Alerts = nullptr;
		FOpsEventBus Bus;
		TArray<FOpsAlert> Raised;
		TArray<FOpsAlertKey> Cleared;
		/** Every FAlertChangedEvent (#445): the keys of standing alerts whose words moved. */
		TArray<FOpsAlertKey> Changed;
		/** The airport status the recompute reads; null reads as open, as in a runtime before attach. */
		const UAirport* Status = nullptr;
		UFlight* Flight = nullptr;
		int32 Plane = 0;

		bool Build()
		{
			const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
			FTestAirportOptions Options;
			Options.StandCount = 2;
			Airport = FTestAirport::Build(Airframe, Options);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Jobs = NewObject<UJobBoard>(GetTransientPackage());
			Offers = NewObject<UOfferGenerator>(GetTransientPackage());
			Ledger = NewObject<ULedger>(GetTransientPackage());
			Ledger->Open(1000.0);
			Alerts = NewObject<UOpsAlerts>(GetTransientPackage());
			Alerts->Bus = &Bus;
			Bus.BeginWiring();
			Bus.Subscribe<FAlertRaisedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FAlertRaisedEvent& E) { Raised.Add(E.Alert); });
			Bus.Subscribe<FAlertClearedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FAlertClearedEvent& E) { Cleared.Add(E.Key); });
			Bus.Subscribe<FAlertChangedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FAlertChangedEvent& E) { Changed.Add(E.Key); });
			// THE FLIGHT BOARD HEARS THE TRAFFIC'S PHASES on this bus, as UOpsRuntime wires it (Sim tier).
			Bus.Subscribe<FAgentPhaseEvent>(EOpsTier::Sim, TEXT("FlightBoard"), [this](const FAgentPhaseEvent& E)
			{
				Board->OnAgentPhase(*Airport.Net, *Clock, E);
			});
			Bus.EndWiring();

			const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*Airport.Net, Airport.ExitAt, ETraversalClass::Aircraft, 200.0);
			if (!Exit.IsSet() || Airport.Stands.Num() < 2)
			{
				return false;
			}
			const FRoutePlan Plan = TestGraph::Probe(*Airport.Net, Exit, Airport.Pose(Airport.Stands[0]), ETraversalClass::Aircraft);
			if (!Plan.IsValid())
			{
				return false;
			}
			Plane = Traffic->DispatchAgent(Airport.Net, Plan, Airframe, ETraversalClass::Aircraft, 0.0);
			Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = Airframe;
			Flight->Callsign = TEXT("CU 204");
			Flight->AgentId = Plane;
			Flight->SetPhaseForTest(EFlightPhase::TaxiIn);
			Board->AddOffer(*Clock, Flight);
			// PUBLISHED INTO THE BUS, heard when Recompute drains it - what UOpsRuntime does (#436). This relay used to
			// call the board inside the traffic's broadcast, an ordering production never has.
			Traffic->OnAgentPhaseChanged.AddLambda([this](const FAgentTransition& Transition)
			{
				Bus.Publish(FAgentPhaseEvent{ Transition });
			});
			return Plane > 0;
		}

		void Recompute()
		{
			FOpsAlertSources Sources;
			Sources.Flights = Board;
			Sources.Jobs = Jobs;
			Sources.Traffic = Traffic;
			Sources.Network = Airport.Net;
			Sources.Offers = Offers;
			Sources.Ledger = Ledger;
			Sources.Airport = Status;
			// THE PHASES FIRST, as production's drain has them: the Sim tier settles the boards before the alerts pass
			// reads them.
			Bus.Drain();
			Alerts->Recompute(Sources, Clock->Now());
			Bus.Drain();
		}

		int32 RaisedOf(EAlertKind Kind) const
		{
			return Raised.FilterByPredicate([Kind](const FOpsAlert& A) { return A.Key.Kind == Kind; }).Num();
		}
		int32 ClearedOf(EAlertKind Kind) const
		{
			return Cleared.FilterByPredicate([Kind](const FOpsAlertKey& K) { return K.Kind == Kind; }).Num();
		}
		int32 ChangedOf(EAlertKind Kind) const
		{
			return Changed.FilterByPredicate([Kind](const FOpsAlertKey& K) { return K.Kind == Kind; }).Num();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsQuietTest, "AirportOps.Model.Alerts.QuietAirportRaisesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsQuietTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("an aircraft taxiing in"), F.Build())) { return false; }
	F.Recompute();
	TestEqual(TEXT("a healthy airport - a taxiing aircraft, money in the bank, no jobs, no jam - has no alert"),
		F.Raised.Num(), 0);
	TestEqual(TEXT("and none is held"), F.Alerts->GetAlerts().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsStrandedFlightTest, "AirportOps.Model.Alerts.FlightStrandedRaisesAndClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsStrandedFlightTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("an aircraft taxiing in"), F.Build())) { return false; }
	if (!TestTrue(TEXT("stranded"), FGroundTrafficTestAccess(*F.Traffic).Strand(F.Plane))) { return false; }
	F.Traffic->Advance(0.2, F.Airport.Net);
	F.Recompute();
	if (!TestEqual(TEXT("a stranded flight raises one alert"), F.RaisedOf(EAlertKind::FlightStranded), 1)) { return false; }
	const FOpsAlert& Alert = F.Raised[0];
	TestEqual(TEXT("keyed by the flight"), Alert.Key.Id, F.Flight->Id);
	TestTrue(TEXT("naming it by callsign"), Alert.Text.ToString().Contains(TEXT("CU 204")));
	TestEqual(TEXT("and focused on its aircraft"), Alert.Focus.Kind, EAlertFocusKind::Agent);
	TestEqual(TEXT("that aircraft"), Alert.Focus.Id, F.Plane);

	F.Recompute();
	TestEqual(TEXT("re-detecting the same condition does not raise it again"), F.RaisedOf(EAlertKind::FlightStranded), 1);

	F.Traffic->RetireAgent(F.Plane);
	F.Recompute();
	TestEqual(TEXT("retired, the flight is no longer stranded - the alert clears with no 'unstranded' event anywhere"),
		F.ClearedOf(EAlertKind::FlightStranded), 1);
	TestEqual(TEXT("and is no longer held"), F.Alerts->GetAlerts().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsVehicleTest, "AirportOps.Model.Alerts.VehicleStrandedRaises",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsVehicleTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("an agent to strand"), F.Build())) { return false; }
	// THE GAME'S CATALOGUE, so the alert can name the kind (#430): it said "FUEL 7 is stranded" of a "Bowser".
	UOpsRuntime::ResolveVehicleCatalogue(*F.Jobs, *GetDefault<UScenario>());
	FServiceVehicle& Truck = F.Jobs->AddVehicleForTest(TEXT("FUEL"), FEntityInstanceId(), EServiceVehicleState::ToJob, 1000.0);
	Truck.AgentId = F.Plane;   // the rule reads only the agent's phase - any stranded agent will do
	const int32 TruckId = Truck.Id;
	FGroundTrafficTestAccess(*F.Traffic).Strand(F.Plane);
	F.Traffic->Advance(0.2, F.Airport.Net);
	F.Recompute();
	TestEqual(TEXT("a service vehicle whose agent is stranded raises its own alert"), F.RaisedOf(EAlertKind::VehicleStranded), 1);
	const FOpsAlert* Alert = F.Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& Each) { return Each.Key.Kind == EAlertKind::VehicleStranded; });
	if (!TestNotNull(TEXT("the alert is held"), Alert)) { return false; }
	TestEqual(TEXT("and names the kind as the card and the ledger do"), Alert->Text.ToString(),
		FString::Printf(TEXT("Bowser #%d is stranded - unstick it"), TruckId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsJobTest, "AirportOps.Model.Alerts.JobUnserviceableRaisesAndClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsJobTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	FServiceJob& Job = F.Jobs->AddJobForTest(F.Plane, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, 0);
	Job.Stand = F.Airport.Stands[0];
	const int32 JobId = Job.Id;
	F.Recompute();
	if (!TestEqual(TEXT("an unserviceable job raises one alert"), F.RaisedOf(EAlertKind::JobUnserviceable), 1)) { return false; }
	TestTrue(TEXT("saying what is missing, in the board's own words"),
		F.Raised[0].Text.ToString().Contains(UJobBoard::RefusalText(EServiceRefusal::NoDepot)));
	TestEqual(TEXT("focused on the stand"), F.Raised[0].Focus.Kind, EAlertFocusKind::Entity);

	for (const FServiceJob& Each : F.Jobs->GetJobs())
	{
		if (Each.Id == JobId)
		{
			// THE PLAYER BUILT A DEPOT and a vehicle took the job: Queued, its reason cleared by UJobBoard::Assign. NOT merely Open with the
			// reason still on it - that is "refused, asking again" (FServiceJob::IsStillRefused, #445), which keeps the alert.
			FServiceJob& Taken = const_cast<FServiceJob&>(Each);
			Taken.State = EServiceJobState::Queued;
			Taken.Why = EServiceRefusal::None;
		}
	}
	F.Recompute();
	TestEqual(TEXT("servable again, the alert clears"), F.ClearedOf(EAlertKind::JobUnserviceable), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsStandLostTest, "AirportOps.Model.Alerts.HeldStandLostRaisesAndClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsStandLostTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UFlight* Coming = NewObject<UFlight>(GetTransientPackage());
	Coming->Callsign = TEXT("CU 900");
	Coming->SetPhaseForTest(EFlightPhase::Accepted);
	Coming->Stand = F.Airport.Stands[1];
	F.Board->AddOffer(*F.Clock, Coming);
	F.Recompute();
	TestEqual(TEXT("an accepted flight holding a stand that exists is fine"), F.RaisedOf(EAlertKind::HeldStandLost), 0);

	F.Airport.Net->RemoveEntity(F.Airport.Stands[1]);
	F.Recompute();
	TestEqual(TEXT("its stand deleted, it has nowhere to park - raised"), F.RaisedOf(EAlertKind::HeldStandLost), 1);

	Coming->Stand = FEntityInstanceId();
	F.Recompute();
	TestEqual(TEXT("holding nothing any more, the alert clears"), F.ClearedOf(EAlertKind::HeldStandLost), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsUnlandableTest, "AirportOps.Model.Alerts.UnlandableHoldingFlightRaisesAnAlert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsUnlandableTest::RunTest(const FString&)
{
	// #442's PIN: A FLIGHT ACCEPTED WHEN THE AIRPORT COULD TAKE IT, THEN THE ONLY EXIT DELETED. The plan's refusal is permanent (only the
	// player can clear it), the flight holds its stand for ever, and there was no alert and no way out but closing the airport.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UFlight* Coming = NewObject<UFlight>(GetTransientPackage());
	Coming->Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Coming->Callsign = TEXT("CU 900");
	Coming->OfferWindowSeconds = 60.0;
	Coming->OfferSecondsLeft = 60.0;
	Coming->LeadTimeSeconds = 1.0;
	Coming->RunwayPreference = F.Airport.Threshold;
	F.Board->AddOffer(*F.Clock, Coming);
	// A DISPATCHER THAT REFUSES, so the queue judges the flight and keeps it: a flight cleared to land would be Landing, not holding.
	F.Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return false; };
	if (!TestTrue(TEXT("accepted while the airport could take it"), F.Board->Accept(*F.Traffic, *F.Airport.Net, *F.Clock, *Coming))) { return false; }
	const FEntityInstanceId Held = Coming->Stand;

	F.Clock->Advance(1.0);   // its ETA: the clock's callback puts it in the queue
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	if (!TestEqual(TEXT("PRECONDITION: it is holding"), Coming->GetPhase(), EFlightPhase::Inbound)) { return false; }
	F.Recompute();
	TestEqual(TEXT("CONTROL: a holding flight that CAN land raises no such alert"), F.RaisedOf(EAlertKind::FlightCannotLand), 0);

	// THE ONLY EXIT DELETED - the taxiway that leaves the runway - and the guideline graph re-derived. The queue judges it afresh.
	for (int32 Index = F.Airport.Net->GetSegments().Num() - 1; Index >= 0; --Index)
	{
		const FRoadSegment& Segment = F.Airport.Net->GetSegments()[Index];
		if (Segment.Profile != nullptr && !Segment.Profile->bContinuousThroughJunctions)
		{
			F.Airport.Net->RemoveSegment(F.Airport.Net->SegmentIdAt(Index));
		}
	}
	TestGraph::Derive(*F.Airport.Net);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	TestTrue(TEXT("PRECONDITION: the planner now refuses it for a reason only the player can clear"),
		ArrivalPlanner::IsPermanentRefusal(F.Board->UnlandableWhy(*Coming, *F.Airport.Net)));
	TestEqual(TEXT("and it is still holding"), Coming->GetPhase(), EFlightPhase::Inbound);

	F.Recompute();
	if (!TestEqual(TEXT("an alert names the holding flight that can never land"), F.RaisedOf(EAlertKind::FlightCannotLand), 1)) { return false; }
	const FOpsAlert* Alert = F.Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::FlightCannotLand; });
	if (!TestNotNull(TEXT("it is held"), Alert)) { return false; }
	TestEqual(TEXT("keyed by the flight"), Alert->Key.Id, Coming->Id);
	TestTrue(TEXT("naming it by its callsign"), Alert->Text.ToString().Contains(TEXT("CU 900")));
	TestTrue(TEXT("and saying what to do"), Alert->Text.ToString().Contains(TEXT("cancel")));
	TestEqual(TEXT("with somewhere for Go to take the camera"), Alert->Focus.Kind, EAlertFocusKind::Point);

	// CANCELLED: the stand it held is free again and the alert clears - by itself, because its condition stopped being true.
	const FEntityInstanceId StillHeld = Coming->Stand;
	if (!TestTrue(TEXT("the player cancels it"), F.Board->CancelByPlayer(*F.Traffic, *F.Clock, Coming->Id))) { return false; }
	const FEntityInstance* Stand = F.Airport.Net->GetEntity(StillHeld);
	TestTrue(TEXT("the stand it held is released"), Stand != nullptr && !F.Traffic->IsStandHeld(Stand->PoseNode, 0));
	F.Recompute();
	TestEqual(TEXT("and the alert clears"), F.ClearedOf(EAlertKind::FlightCannotLand), 1);
	TestEqual(TEXT("leaving none"), F.Alerts->GetAlerts().FilterByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::FlightCannotLand; }).Num(), 0);
	(void)Held;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsFixedAirportTest, "AirportOps.Model.Alerts.FixedAirportClearsTheAlertBehindABusyRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsFixedAirportTest::RunTest(const FString&)
{
	// THE ALERT MUST NOT OUTLIVE THE FIX (#442 review): ClearanceFor is asked only once the runway is found free, so a holding flight behind
	// a BUSY runway keeps the "no exit" it was judged with after the player rebuilds the exit - and the alert, read from that cache, stayed
	// up until the runway freed. A judgement older than the guideline graph is None.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UFlight* Coming = NewObject<UFlight>(GetTransientPackage());
	Coming->Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Coming->Callsign = TEXT("CU 900");
	Coming->OfferWindowSeconds = 60.0;
	Coming->OfferSecondsLeft = 60.0;
	Coming->LeadTimeSeconds = 1.0;
	Coming->RunwayPreference = F.Airport.Threshold;
	F.Board->AddOffer(*F.Clock, Coming);
	F.Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return false; };
	if (!TestTrue(TEXT("accepted while the airport could take it"), F.Board->Accept(*F.Traffic, *F.Airport.Net, *F.Clock, *Coming))) { return false; }
	F.Clock->Advance(1.0);

	// THE TAXIWAY OUT DELETED - its ends and profile kept, so the same road can be laid again - and the graph re-derived.
	FRoadNodeId TaxiA, TaxiB;
	URoadProfile* TaxiProfile = nullptr;
	for (int32 Index = F.Airport.Net->GetSegments().Num() - 1; Index >= 0; --Index)
	{
		const FRoadSegment& Segment = F.Airport.Net->GetSegments()[Index];
		if (Segment.Profile != nullptr && !Segment.Profile->bContinuousThroughJunctions)
		{
			TaxiA = Segment.A;
			TaxiB = Segment.B;
			TaxiProfile = Segment.Profile;
			F.Airport.Net->RemoveSegment(F.Airport.Net->SegmentIdAt(Index));
			break;
		}
	}
	if (!TestNotNull(TEXT("the taxiway was found and removed"), TaxiProfile)) { return false; }
	TestGraph::Derive(*F.Airport.Net);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	if (!TestEqual(TEXT("PRECONDITION: the flight that cannot land has an alert"), F.RaisedOf(EAlertKind::FlightCannotLand), 1)) { return false; }

	// THE RUNWAY IS BUSY, then the player REBUILDS THE EXIT - really: the whole derivation a rebuild runs, with the anchor links the graph-only
	// Derive above dropped, so the planner itself now says the field can take the flight (PRECONDITION below). The queue does not ask a flight
	// about its clearance while its runway is held; the alert pass can read only what the queue has judged - and since #445 the queue re-dates
	// every unarrived flight on a graph change (FArrivalQueue::JudgeUnarrived), so the alert clears because the judgement is fresh and says None.
	for (const FTrafficResource& Surface : F.Airport.Net->RunwaySurfaces(F.Airport.ThresholdSegment))
	{
		FTrafficClaim Claim;
		Claim.AgentId = 99;
		Claim.Resource = Surface;
		Claim.bOccupied = true;
		FTrafficClaim Blocker;
		F.Traffic->OccupancyForTest().TryClaim(Claim, Blocker);
	}
	TestGraph::Lay(*F.Airport.Net, TaxiA, TaxiB, TaxiProfile);
	TestGraph::Rebuild(*F.Airport.Net);
	if (!TestEqual(TEXT("PRECONDITION: the fix is real - the planner itself, with no occupancy, now lets the flight land"),
		ArrivalPlanner::Plan(*F.Airport.Net, Coming->RunwayPreference, Coming->Airframe).Why, EArrivalRefusal::None)) { return false; }
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	if (!TestEqual(TEXT("PRECONDITION: the flight is still holding - the busy runway kept it from being cleared"), Coming->GetPhase(), EFlightPhase::Inbound)) { return false; }

	TestEqual(TEXT("the queue re-dated its verdict against the fixed graph: nothing left to alert"), F.Board->UnlandableWhy(*Coming, *F.Airport.Net), EArrivalRefusal::None);
	F.Recompute();
	TestEqual(TEXT("so the alert clears the recompute after the fix, without waiting for the runway"), F.ClearedOf(EAlertKind::FlightCannotLand), 1);
	TestEqual(TEXT("and none is held"), F.Alerts->GetAlerts().FilterByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::FlightCannotLand; }).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsAirlineTest, "AirportOps.Model.Alerts.AirlineCannotComeRaisesAndClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsAirlineTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	FAirlineOfferState& State = F.Offers->States.FindOrAdd(TEXT("CumbriaAir"));
	State.bCouldCome = false;
	F.Recompute();
	if (!TestEqual(TEXT("an airline that cannot use the airport raises an alert"), F.RaisedOf(EAlertKind::AirlineCannotCome), 1)) { return false; }
	TestEqual(TEXT("keyed by the airline's name, which survives a re-attach"), F.Raised[0].Key.Name, FName(TEXT("CumbriaAir")));
	TestEqual(TEXT("with nowhere in the world to look"), F.Raised[0].Focus.Kind, EAlertFocusKind::None);
	F.Offers->States.FindOrAdd(TEXT("CumbriaAir")).bCouldCome = true;
	F.Recompute();
	TestEqual(TEXT("once it can come again, cleared"), F.ClearedOf(EAlertKind::AirlineCannotCome), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsNoRunwayTest, "AirportOps.Model.Alerts.NoRunwayRaised",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsNoRunwayTest::RunTest(const FString&)
{
	// SPEC 2026-09-29-ops-batch3 §3: an airport with no runway gets no offers, and the player is told why - derived
	// from the status like every other alert. A deliberate closure raises nothing: the player knows.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UAirport* Airport = NewObject<UAirport>(GetTransientPackage());
	F.Status = Airport;
	// AN AIRLINE ALREADY JUDGED UNABLE TO COME, from before: while the airport is not open the generator asks no
	// airline anything, so a verdict left standing would be a stale alert.
	F.Offers->States.FindOrAdd(TEXT("CumbriaAir")).bCouldCome = false;

	Airport->Reseat(*NewObject<URoadNetwork>(GetTransientPackage()));
	if (!TestEqual(TEXT("an empty network: the airport has no runway"), Airport->Status(), EAirportStatus::NoRunway)) { return false; }
	F.Recompute();
	if (!TestEqual(TEXT("no runway raises its alert"), F.RaisedOf(EAlertKind::NoRunway), 1)) { return false; }
	const FOpsAlert* Alert = F.Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::NoRunway; });
	if (!TestNotNull(TEXT("and holds it"), Alert)) { return false; }
	TestEqual(TEXT("worded to say what to build"), Alert->Text.ToString(), FString(TEXT("No runway - build one to receive offers")));
	TestEqual(TEXT("with nowhere in the world to look"), Alert->Focus.Kind, EAlertFocusKind::None);
	TestEqual(TEXT("and no airline alert while nothing is asked of the airlines"), F.RaisedOf(EAlertKind::AirlineCannotCome), 0);

	Airport->SetClosedByPlayer(true, *F.Airport.Net);
	F.Recompute();
	TestEqual(TEXT("closed by the player: the runway alert clears"), F.ClearedOf(EAlertKind::NoRunway), 1);
	TestEqual(TEXT("and a deliberate closure raises nothing"), F.Alerts->GetAlerts().Num(), 0);

	Airport->SetClosedByPlayer(false, *F.Airport.Net);
	F.Recompute();
	TestEqual(TEXT("open with a runway: the airline's verdict is an alert again"), F.RaisedOf(EAlertKind::AirlineCannotCome), 1);
	TestEqual(TEXT("and no runway alert"), F.RaisedOf(EAlertKind::NoRunway), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsOverdrawnTest, "AirportOps.Model.Alerts.OverdrawnRaisesAndClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsOverdrawnTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	F.Ledger->Post(0.0, ELedgerCategory::Upkeep, -5000.0, FText::FromString(TEXT("test")));
	F.Recompute();
	TestEqual(TEXT("a negative balance locks building - raised"), F.RaisedOf(EAlertKind::Overdrawn), 1);
	F.Ledger->Post(0.0, ELedgerCategory::LandingFee, 10000.0, FText::FromString(TEXT("test")));
	F.Recompute();
	TestEqual(TEXT("back in credit - cleared"), F.ClearedOf(EAlertKind::Overdrawn), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsResetTest, "AirportOps.Model.Alerts.ResetReRaisesWhatIsTrue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsResetTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	F.Ledger->Post(0.0, ELedgerCategory::Upkeep, -5000.0, FText::FromString(TEXT("test")));
	F.Recompute();
	if (!TestEqual(TEXT("setup: the overdrawn alert was raised once"), F.RaisedOf(EAlertKind::Overdrawn), 1)) { return false; }
	TestFalse(TEXT("and the first raise is a raise, not a re-raise"), F.Raised.Last().bReRaised);
	F.Alerts->Reset();
	// DRAINED BEFORE ClearedOf IS READ (#463): Reset publishes onto the bus and the Cleared subscriber hears only on a Drain, so reading
	// it straight after Reset passed with Reset announcing a clear for every alert it emptied. Drained, a Cleared that Reset sent is in F.Cleared.
	F.Bus.Drain();
	TestEqual(TEXT("Reset empties the set without announcing anything (a load's UI mirror is cleared by its caller)"),
		F.ClearedOf(EAlertKind::Overdrawn), 0);
	F.Recompute();
	TestEqual(TEXT("and the next recompute raises again everything still true - nothing is saved, nothing is lost"),
		F.RaisedOf(EAlertKind::Overdrawn), 2);
	TestTrue(TEXT("marked as a re-raise - the alert list shows it and the toast does not, or a load re-announces every standing problem as news"),
		F.Raised.Last().bReRaised);
	return true;
}

// A DEADLOCK RING NOBODY IN CAN TURN OUT OF IS NOT ALWAYS MADE OF AIRCRAFT (issue #455). The alert used to be
// all-aircraft only and said "{0} aircraft deadlocked" for every ring it raised; since #455 a ring can include a truck
// backing along a bay's leg (or an aeroplane being pushed), members with no second line. The text must say what is in
// the ring, not call the truck an aircraft. A bay is laid far off on the airport's own network - three one-way legs,
// the middle one a reverse - the truck is driven until it is Reversing, and a wait is staged between it and the
// airport's aircraft through the traffic model's own scripting door, as the inspector's deadlock test stages one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsDeadlockTruckTest, "AirportOps.Model.Alerts.DeadlockWithAReversingTruckDoesNotCallItAnAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsDeadlockTruckTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("an aircraft taxiing in"), F.Build())) { return false; }
	URoadNetwork& Net = *F.Airport.Net;

	const FGuidelineNodeId Approach = TestGraph::Node(Net, -90000.0, -84000.0);
	const FGuidelineNodeId Service = TestGraph::Node(Net, -90000.0, -88000.0);
	const FGuidelineNodeId Cleared = TestGraph::Node(Net, -90000.0, -85500.0);
	const FGuidelineNodeId Exit = TestGraph::Node(Net, -82000.0, -85500.0);
	auto Lay = [&Net](FGuidelineNodeId From, FGuidelineNodeId To, bool bReverse)
	{
		FGuidelineEdge Edge;
		Edge.A = From;
		Edge.B = To;
		Edge.Control = (Net.GetGuidelineNode(From)->Position + Net.GetGuidelineNode(To)->Position) * 0.5;
		Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::GroundVehicle);
		Edge.Direction = EGuidelineDir::AToB;
		Edge.Width = 400.0;
		Edge.bDerived = false;
		Edge.bReverseLeg = bReverse;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	};
	Lay(Approach, Service, false);
	Lay(Service, Cleared, true);
	Lay(Cleared, Exit, false);
	const FRoutePlan Route = TestGraph::Probe(Net, Approach, Exit, ETraversalClass::GroundVehicle);
	if (!TestTrue(TEXT("the bay routes as arrive, reverse, depart"),
		Route.IsValid() && Route.Steps.Num() == 3 && Route.Steps[1].bReverseLeg)) { return false; }

	FVehicle Vehicle = UAirsideSettings::ResolveDefaultVehicle();
	Vehicle.Chassis = UAirsideSettings::ResolveLargestServiceVehicle();
	const int32 Truck = F.Traffic->DispatchAgent(F.Airport.Net, Route, Vehicle, ETraversalClass::GroundVehicle, 1.0);
	if (!TestTrue(TEXT("the truck is dispatched"), Truck > 0)) { return false; }
	bool bReversing = false;
	for (int32 Tick = 0; Tick < 6000 && !bReversing; ++Tick)
	{
		F.Traffic->Advance(0.05, F.Airport.Net);
		const FRoadAgent* T = F.Traffic->FindAgent(Truck);
		bReversing = T != nullptr && T->Phase == EAgentPhase::Reversing;
	}
	if (!TestTrue(TEXT("and it backs off the service point"), bReversing)) { return false; }

	// THE RING: the truck waits on the aircraft, the aircraft on the truck, both past the stall threshold.
	FGroundTrafficTestAccess Access(*F.Traffic);
	Access.ScriptWait(Truck, FTrafficResource::OfNode(Cleared), F.Plane, F.Traffic->Rules.StallSeconds * 2.0);
	Access.ScriptWait(F.Plane, FTrafficResource::OfNode(Approach), Truck, F.Traffic->Rules.StallSeconds * 2.0);
	F.Recompute();

	if (!TestEqual(TEXT("the ring through a reversing truck raises one Deadlock alert"), F.RaisedOf(EAlertKind::Deadlock), 1)) { return false; }
	FString Text;
	for (const FOpsAlert& Alert : F.Raised)
	{
		if (Alert.Key.Kind == EAlertKind::Deadlock) { Text = Alert.Text.ToString(); }
	}
	TestTrue(*FString::Printf(TEXT("it counts the aircraft as one ('%s')"), *Text), Text.Contains(TEXT("1 aircraft")));
	TestTrue(*FString::Printf(TEXT("and the truck as a vehicle, not a second aircraft ('%s')"), *Text),
		Text.Contains(TEXT("1 vehicle")) && !Text.Contains(TEXT("2 aircraft")));
	TestTrue(TEXT("and gives the remedy the inspector's card gives"), Text.Contains(UOpsAlerts::DeadlockRemedy().ToString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsReofferTest, "AirportOps.Model.Alerts.RefusedJobReofferedDoesNotFlicker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsReofferTest::RunTest(const FString&)
{
	// #445's FIRST PIN, through the job board's own Step: a refused job is re-offered AFTER the bids (UJobBoard::Step), so for the frame between
	// the re-offer and the next bid it is Open - and the alerts pass, reading "Unserviceable" alone, cleared the alert and raised it again on
	// the next frame with a fresh toast and a reset RaisedAt: once per road drawn while trying to connect a depot. The owner (FServiceJob::
	// IsStillRefused) says it is still refused now.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	FServiceJob& Job = F.Jobs->AddJobForTest(F.Plane, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, F.Airport.Net->GetGuidelineRevision() + 1);
	Job.Stand = F.Airport.Stands[0];
	const int32 JobId = Job.Id;
	F.Recompute();
	if (!TestEqual(TEXT("PRECONDITION: the refused job is alerted once"), F.RaisedOf(EAlertKind::JobUnserviceable), 1)) { return false; }
	const double RaisedAt = F.Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::JobUnserviceable; })->RaisedAt;

	// THE ROAD THE PLAYER DREW: the step re-offers the job (its refusal was made at a revision the airport is no longer at).
	F.Jobs->Step(*F.Traffic, *F.Airport.Net, *F.Clock);
	const FServiceJob* Reoffered = F.Jobs->GetJobs().FindByPredicate([JobId](const FServiceJob& Each) { return Each.Id == JobId; });
	if (!TestTrue(TEXT("PRECONDITION: the step left it Open - re-offered, its bid pending"), Reoffered != nullptr && Reoffered->State == EServiceJobState::Open)) { return false; }
	F.Recompute();
	TestEqual(TEXT("re-offered and not yet bid, the alert is NOT cleared - refused, asking again is still refused"), F.ClearedOf(EAlertKind::JobUnserviceable), 0);

	// THE NEXT FRAMES: the bid refuses it again (no depot), and the alert goes on standing.
	F.Jobs->Step(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	F.Recompute();
	TestEqual(TEXT("three frames on: no clear"), F.ClearedOf(EAlertKind::JobUnserviceable), 0);
	TestEqual(TEXT("and no second raise - so no second toast"), F.RaisedOf(EAlertKind::JobUnserviceable), 1);
	const FOpsAlert* Held = F.Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::JobUnserviceable; });
	if (TestNotNull(TEXT("still held"), Held))
	{
		TestEqual(TEXT("with its first RaisedAt, not reset by the road"), Held->RaisedAt, RaisedAt);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsChangedReasonTest, "AirportOps.Model.Alerts.ChangedReasonIsAnnounced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsChangedReasonTest::RunTest(const FString&)
{
	// #445's SECOND PIN, the model's half: a STANDING alert whose reason changes refreshed its text and published nothing, so a UI that kept
	// a copy of the list kept the old reason. Now the model says so - an event naming the key, no raise (no toast), RaisedAt kept - and the
	// text the model holds is the new one.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	FServiceJob& Job = F.Jobs->AddJobForTest(F.Plane, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, F.Airport.Net->GetGuidelineRevision());
	Job.Stand = F.Airport.Stands[0];
	const int32 JobId = Job.Id;
	F.Recompute();
	if (!TestEqual(TEXT("PRECONDITION: raised with reason A"), F.RaisedOf(EAlertKind::JobUnserviceable), 1)) { return false; }
	TestTrue(TEXT("saying it (A)"), F.Alerts->GetAlerts()[0].Text.ToString().Contains(UJobBoard::RefusalText(EServiceRefusal::NoDepot)));
	TestEqual(TEXT("an unchanged recompute announces no change"), F.ChangedOf(EAlertKind::JobUnserviceable), 0);
	F.Recompute();
	TestEqual(TEXT("not even on the second look"), F.ChangedOf(EAlertKind::JobUnserviceable), 0);

	// THE REASON CHANGES - the player built a depot, and what is missing is now its pump.
	for (const FServiceJob& Each : F.Jobs->GetJobs())
	{
		if (Each.Id == JobId) { const_cast<FServiceJob&>(Each).Why = EServiceRefusal::NoPump; }
	}
	F.Recompute();
	TestEqual(TEXT("the change is announced once, naming the alert"), F.ChangedOf(EAlertKind::JobUnserviceable), 1);
	TestEqual(TEXT("it is not a raise - no second toast"), F.RaisedOf(EAlertKind::JobUnserviceable), 1);
	TestEqual(TEXT("and not a clear"), F.ClearedOf(EAlertKind::JobUnserviceable), 0);
	if (TestEqual(TEXT("the model holds one alert"), F.Alerts->GetAlerts().Num(), 1))
	{
		const FString Text = F.Alerts->GetAlerts()[0].Text.ToString();
		TestTrue(TEXT("saying the NEW reason (B)"), Text.Contains(UJobBoard::RefusalText(EServiceRefusal::NoPump)));
		TestFalse(TEXT("and not the old"), Text.Contains(UJobBoard::RefusalText(EServiceRefusal::NoDepot)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsNodeDragTest, "AirportOps.Present.Alerts.AlertSurvivesANodeDrag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsNodeDragTest::RunTest(const FString&)
{
	// #445 / #484 REVIEW: mid-drag the planner answers GraphBeingEdited (a transient refusal - it clears when the player lets go), and the
	// queue pass stored it over the standing permanent verdict, so UnlandableWhy read None for the length of the drag: the FlightCannotLand
	// alert cleared, and the drop raised it again with a fresh toast. A transient "being edited" must not clear a standing alert.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UFlight* Coming = NewObject<UFlight>(GetTransientPackage());
	Coming->Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Coming->Callsign = TEXT("CU 900");
	Coming->OfferWindowSeconds = 60.0;
	Coming->OfferSecondsLeft = 60.0;
	Coming->LeadTimeSeconds = 1.0;
	Coming->RunwayPreference = F.Airport.Threshold;
	F.Board->AddOffer(*F.Clock, Coming);
	F.Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return false; };
	if (!TestTrue(TEXT("accepted while the airport could take it"), F.Board->Accept(*F.Traffic, *F.Airport.Net, *F.Clock, *Coming))) { return false; }
	F.Clock->Advance(1.0);
	for (int32 Index = F.Airport.Net->GetSegments().Num() - 1; Index >= 0; --Index)
	{
		const FRoadSegment& Segment = F.Airport.Net->GetSegments()[Index];
		if (Segment.Profile != nullptr && !Segment.Profile->bContinuousThroughJunctions)
		{
			F.Airport.Net->RemoveSegment(F.Airport.Net->SegmentIdAt(Index));
		}
	}
	TestGraph::Derive(*F.Airport.Net);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	if (!TestEqual(TEXT("PRECONDITION: the holding flight that can never land is alerted"), F.RaisedOf(EAlertKind::FlightCannotLand), 1)) { return false; }

	// THE PLAYER PICKS A NODE UP: the road's clocks move and the graph is behind it. The queue pass runs meanwhile - an aircraft retiring
	// moves the occupancy revision, so the cached clearance is asked afresh, and the planner says GraphBeingEdited.
	F.Airport.Net->AddNode(FVector2D(-150000.0, 150000.0));
	if (!TestTrue(TEXT("PRECONDITION: the guideline graph is behind the road"), F.Airport.Net->AreGuidelinesBehindRoad())) { return false; }
	const uint32 OccupancyBefore = F.Traffic->OccupancyRevision();
	F.Traffic->RetireAgent(F.Plane);
	if (!TestTrue(TEXT("PRECONDITION: the occupancy moved, so the clearance is not served from its cache"), F.Traffic->OccupancyRevision() != OccupancyBefore)) { return false; }
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	TestEqual(TEXT("mid-drag the alert does not clear"), F.ClearedOf(EAlertKind::FlightCannotLand), 0);
	TestEqual(TEXT("and the model still holds it"),
		F.Alerts->GetAlerts().FilterByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::FlightCannotLand; }).Num(), 1);

	// THE DROP: the graph is derived again, and the queue judges the flight afresh - still permanent.
	TestGraph::Derive(*F.Airport.Net);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	TestEqual(TEXT("across the whole drag and its drop: no clear"), F.ClearedOf(EAlertKind::FlightCannotLand), 0);
	TestEqual(TEXT("and no second raise - one toast in total"), F.RaisedOf(EAlertKind::FlightCannotLand), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsAcceptedUnlandableTest, "AirportOps.Model.Alerts.AcceptedFlightThatCanNeverLandIsAlertedBeforeItsEta",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsAcceptedUnlandableTest::RunTest(const FString&)
{
	// #445 (found in the #484 review): only a HOLDING flight was judged, so an accepted one whose airport had been changed so it can never
	// land said nothing until its ETA brought it into the queue - minutes of game time after the player did it. The queue pass judges
	// accepted flights too, against the same IsPermanentRefusal.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UFlight* Coming = NewObject<UFlight>(GetTransientPackage());
	Coming->Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Coming->Callsign = TEXT("CU 901");
	Coming->OfferWindowSeconds = 60.0;
	Coming->OfferSecondsLeft = 60.0;
	Coming->LeadTimeSeconds = 1.0e7;   // its ETA is nowhere near
	Coming->RunwayPreference = F.Airport.Threshold;
	F.Board->AddOffer(*F.Clock, Coming);
	if (!TestTrue(TEXT("accepted while the airport could take it"), F.Board->Accept(*F.Traffic, *F.Airport.Net, *F.Clock, *Coming))) { return false; }
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	TestEqual(TEXT("CONTROL: an accepted flight the airport CAN take raises no such alert"), F.RaisedOf(EAlertKind::FlightCannotLand), 0);

	for (int32 Index = F.Airport.Net->GetSegments().Num() - 1; Index >= 0; --Index)
	{
		const FRoadSegment& Segment = F.Airport.Net->GetSegments()[Index];
		if (Segment.Profile != nullptr && !Segment.Profile->bContinuousThroughJunctions)
		{
			F.Airport.Net->RemoveSegment(F.Airport.Net->SegmentIdAt(Index));
		}
	}
	TestGraph::Derive(*F.Airport.Net);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);   // the queue pass, which an edit's NetworkChanged dirties
	if (!TestEqual(TEXT("PRECONDITION: still Accepted - its ETA has not come"), Coming->GetPhase(), EFlightPhase::Accepted)) { return false; }
	TestTrue(TEXT("PRECONDITION: its plan is now refused for a reason only the player can clear"),
		ArrivalPlanner::IsPermanentRefusal(F.Board->UnlandableWhy(*Coming, *F.Airport.Net)));
	F.Recompute();
	if (!TestEqual(TEXT("the accepted flight that can never land is alerted before it is due"), F.RaisedOf(EAlertKind::FlightCannotLand), 1)) { return false; }
	const FOpsAlert* Alert = F.Alerts->GetAlerts().FindByPredicate([](const FOpsAlert& A) { return A.Key.Kind == EAlertKind::FlightCannotLand; });
	if (!TestNotNull(TEXT("held"), Alert)) { return false; }
	TestTrue(TEXT("keyed by the flight, named by its callsign"), Alert->Key.Id == Coming->Id && Alert->Text.ToString().Contains(TEXT("CU 901")));
	TestFalse(TEXT("and not called 'holding' - it has not come yet"), Alert->Text.ToString().Contains(TEXT("holding")));
	TestTrue(TEXT("and saying what to do"), Alert->Text.ToString().Contains(TEXT("cancel")));

	// ITS ETA COMES: the same alert, in the words for a flight that is holding - a change of words, not a second raise.
	F.Clock->Advance(1.0e7 + 1.0);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	TestEqual(TEXT("the flight is holding now"), Coming->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("still one raise in total"), F.RaisedOf(EAlertKind::FlightCannotLand), 1);
	TestEqual(TEXT("never cleared between"), F.ClearedOf(EAlertKind::FlightCannotLand), 0);
	TestEqual(TEXT("its new words announced as a change"), F.ChangedOf(EAlertKind::FlightCannotLand), 1);

	if (!TestTrue(TEXT("the player cancels it"), F.Board->CancelByPlayer(*F.Traffic, *F.Clock, Coming->Id))) { return false; }
	F.Recompute();
	TestEqual(TEXT("and the alert clears"), F.ClearedOf(EAlertKind::FlightCannotLand), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsBusyRunwayEditTest, "AirportOps.Model.Alerts.HoldingFlightAlertSurvivesAnUnrelatedEditBehindABusyRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsBusyRunwayEditTest::RunTest(const FString&)
{
	// #445 REVIEW, the #442 review's follow-up: ClearanceFor is asked only once the runway is found free, so a holding flight behind a BUSY runway was never
	// re-judged after an edit - its cached verdict went stale, UnlandableWhy read None, the alert cleared, and it was raised again (a fresh toast) when the
	// runway freed. Every unrelated road drawn while the runway was busy did it. The queue re-dates its unarrived flights on a graph change whatever the
	// runway is doing (FArrivalQueue::JudgeUnarrived), so the alert stands.
	FAlertsField F;
	if (!TestTrue(TEXT("a field"), F.Build())) { return false; }
	UFlight* Coming = NewObject<UFlight>(GetTransientPackage());
	Coming->Airframe = UAirsideSettings::ResolveDefaultAirframe();
	Coming->Callsign = TEXT("CU 900");
	Coming->OfferWindowSeconds = 60.0;
	Coming->OfferSecondsLeft = 60.0;
	Coming->LeadTimeSeconds = 1.0;
	Coming->RunwayPreference = F.Airport.Threshold;
	F.Board->AddOffer(*F.Clock, Coming);
	F.Board->Dispatcher = [](const FVector2D&, const FAirframe&) { return false; };
	if (!TestTrue(TEXT("accepted while the airport could take it"), F.Board->Accept(*F.Traffic, *F.Airport.Net, *F.Clock, *Coming))) { return false; }
	F.Clock->Advance(1.0);
	for (int32 Index = F.Airport.Net->GetSegments().Num() - 1; Index >= 0; --Index)
	{
		const FRoadSegment& Segment = F.Airport.Net->GetSegments()[Index];
		if (Segment.Profile != nullptr && !Segment.Profile->bContinuousThroughJunctions)
		{
			F.Airport.Net->RemoveSegment(F.Airport.Net->SegmentIdAt(Index));
		}
	}
	TestGraph::Derive(*F.Airport.Net);
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	if (!TestEqual(TEXT("PRECONDITION: the holding flight that can never land is alerted"), F.RaisedOf(EAlertKind::FlightCannotLand), 1)) { return false; }

	// THE RUNWAY IS BUSY - the flight is not asked about its clearance while it is - and the player draws an unrelated road.
	for (const FTrafficResource& Surface : F.Airport.Net->RunwaySurfaces(F.Airport.ThresholdSegment))
	{
		FTrafficClaim Claim;
		Claim.AgentId = 99;
		Claim.Resource = Surface;
		Claim.bOccupied = true;
		FTrafficClaim Blocker;
		F.Traffic->OccupancyForTest().TryClaim(Claim, Blocker);
	}
	const uint32 RevisionBefore = F.Airport.Net->GetGuidelineRevision();
	F.Airport.Net->AddNode(FVector2D(-150000.0, 150000.0));
	TestGraph::Derive(*F.Airport.Net);
	if (!TestTrue(TEXT("PRECONDITION: the edit moved the guideline graph"), F.Airport.Net->GetGuidelineRevision() != RevisionBefore)) { return false; }
	F.Board->TickQueue(*F.Traffic, *F.Airport.Net, *F.Clock);
	F.Recompute();
	TestEqual(TEXT("behind a busy runway, an unrelated edit: the alert does not clear"), F.ClearedOf(EAlertKind::FlightCannotLand), 0);
	TestEqual(TEXT("and is not raised again - one toast"), F.RaisedOf(EAlertKind::FlightCannotLand), 1);
	return true;
}

// THE DEADLOCK SAYS WHERE (taxiway naming spec 2026-10-02): on a 40-stand airport "2 aircraft deadlocked" cannot be found.
// Two aircraft on the airport's taxiway wait on each other; once the network is named, the alert names the taxiway the
// lowest member is on - and before, it says what it always said.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpsAlertsDeadlockWhereTest, "AirportOps.Model.Alerts.DeadlockSaysWhere",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FOpsAlertsDeadlockWhereTest::RunTest(const FString&)
{
	FAlertsField F;
	if (!TestTrue(TEXT("an aircraft taxiing in"), F.Build())) { return false; }
	// THE LOWEST MEMBER (the alert's key and focus) is F.Plane, taxiing in from the exit; it is moved off the runway's
	// centreline node first, where WhereIs would answer the runway's pair instead of the taxiway.
	const auto OnRunway = [&F]()
	{
		const FRoadAgent* Moving = F.Traffic->FindAgent(F.Plane);
		const FRoutePlan& Route = Moving->PlanInProgress();
		const FGuidelineEdge* Edge = F.Airport.Net->GetGuidelineEdge(
			Route.Steps[UGroundTraffic::CurrentStep(Route, Moving->DistanceAlongPlan())].Edge);
		return Edge != nullptr && F.Airport.Net->IsRunwaySegment(Edge->DerivedFrom);
	};
	for (int32 Tick = 0; Tick < 4000 && OnRunway(); ++Tick) { F.Traffic->Advance(0.05, F.Airport.Net); }
	if (!TestFalse(TEXT("setup: the first aircraft is off the runway"), OnRunway())) { return false; }
	// A SECOND AIRCRAFT, from the other stand out to the exit - a higher id, so F.Plane stays the lowest member.
	const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*F.Airport.Net, F.Airport.ExitAt, ETraversalClass::Aircraft, 200.0);
	const FRoutePlan Out = TestGraph::Probe(*F.Airport.Net, F.Airport.Pose(F.Airport.Stands[1]), Exit, ETraversalClass::Aircraft);
	const int32 Second = Out.IsValid() ? F.Traffic->DispatchAgent(F.Airport.Net, Out, UAirsideSettings::ResolveDefaultAirframe(),
		ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("a second aircraft"), Second > F.Plane)) { return false; }
	FGroundTrafficTestAccess Access(*F.Traffic);
	const FGuidelineNodeId Any = F.Airport.Pose(F.Airport.Stands[0]);
	Access.ScriptWait(F.Plane, FTrafficResource::OfNode(Any), Second, F.Traffic->Rules.StallSeconds * 2.0);
	Access.ScriptWait(Second, FTrafficResource::OfNode(Any), F.Plane, F.Traffic->Rules.StallSeconds * 2.0);
	F.Airport.Net->NormaliseTaxiways(FTaxiwayNamingRules());
	F.Recompute();
	FString Text;
	for (const FOpsAlert& Alert : F.Raised)
	{
		if (Alert.Key.Kind == EAlertKind::Deadlock) { Text = Alert.Text.ToString(); }
	}
	TestTrue(FString::Printf(TEXT("the deadlock says where ('%s')"), *Text), Text.Contains(TEXT("2 aircraft deadlocked on A - ")));
	TestTrue(TEXT("and still gives the remedy"), Text.Contains(UOpsAlerts::DeadlockRemedy().ToString()));
	return true;
}

#endif
