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
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
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
			Flight->Phase = EFlightPhase::TaxiIn;
			Board->AddOffer(*Clock, Flight);
			UFlightBoard* Bound = Board;
			URoadNetwork* Graph = Airport.Net;
			UGroundTraffic* Model = Traffic;
			USimClock* Time = Clock;
			Traffic->OnAgentPhaseChanged.AddLambda([Bound, Graph, Model, Time](int32 Id, EAgentPhase From, EAgentPhase To)
			{
				Bound->OnAgentPhase(*Model, *Graph, *Time, Id, From, To);
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
	FServiceVehicle& Truck = F.Jobs->AddVehicleForTest(TEXT("FUEL"), FEntityInstanceId(), EServiceVehicleState::ToJob, 1000.0);
	Truck.AgentId = F.Plane;   // the rule reads only the agent's phase - any stranded agent will do
	FGroundTrafficTestAccess(*F.Traffic).Strand(F.Plane);
	F.Traffic->Advance(0.2, F.Airport.Net);
	F.Recompute();
	TestEqual(TEXT("a service vehicle whose agent is stranded raises its own alert"), F.RaisedOf(EAlertKind::VehicleStranded), 1);
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
			const_cast<FServiceJob&>(Each).State = EServiceJobState::Open;   // the player built a depot
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
	Coming->Phase = EFlightPhase::Accepted;
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
	F.Alerts->Reset();
	TestEqual(TEXT("Reset empties the set without announcing anything (a load's UI mirror is cleared by its caller)"),
		F.ClearedOf(EAlertKind::Overdrawn), 0);
	F.Recompute();
	TestEqual(TEXT("and the next recompute raises again everything still true - nothing is saved, nothing is lost"),
		F.RaisedOf(EAlertKind::Overdrawn), 2);
	return true;
}

#endif
