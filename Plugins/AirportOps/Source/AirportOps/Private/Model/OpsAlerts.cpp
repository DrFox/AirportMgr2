#include "Model/OpsAlerts.h"
#include "AirportOpsLog.h"
#include "Model/AirlineDefinition.h"
#include "Model/ArrivalPlanner.h"
#include "Model/Airport.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsNames.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAllocator.h"

namespace
{
	FOpsAlert OpsAlertOf(EAlertKind Kind, int32 Id, FName Name, FText Text)
	{
		FOpsAlert Alert;
		Alert.Key.Kind = Kind;
		Alert.Key.Id = Id;
		Alert.Key.Name = Name;
		Alert.Text = MoveTemp(Text);
		return Alert;
	}

	void OpsAlertFocusAgent(FOpsAlert& Alert, const FRoadAgent& Agent)
	{
		Alert.Focus.Kind = EAlertFocusKind::Agent;
		Alert.Focus.Id = Agent.Id;
		Alert.Focus.Point = Agent.GroundPosition();
	}
}

FText UOpsAlerts::DeadlockRemedy()
{
	return NSLOCTEXT("OpsAlerts", "DeadlockRemedy", "the layout needs another way round or out");
}

void UOpsAlerts::Reset()
{
	Alerts.Reset();
	bAfterReset = true;
	if (Bus != nullptr)
	{
		Bus->Publish(FAlertsResetEvent{});
	}
}

void UOpsAlerts::StageForTest(const FOpsAlert& Alert)
{
	if (FOpsAlert* Existing = Alerts.FindByPredicate([&Alert](const FOpsAlert& A) { return A.Key == Alert.Key; }))
	{
		*Existing = Alert;
		return;
	}
	Alerts.Add(Alert);
}

void UOpsAlerts::UnstageForTest(const FOpsAlertKey& Key)
{
	Alerts.RemoveAll([&Key](const FOpsAlert& A) { return A.Key == Key; });
}

void UOpsAlerts::Recompute(const FOpsAlertSources& Sources, double Now)
{
	++RecomputeCount;
	TArray<FOpsAlert> Found;

	// FLIGHTS: stranded aircraft, and accepted flights whose held stand has gone.
	if (Sources.Flights != nullptr)
	{
		for (const UFlight* Flight : Sources.Flights->Live())
		{
			if (Flight == nullptr)
			{
				continue;
			}
			const FRoadAgent* Agent = Sources.Traffic != nullptr && Flight->AgentId != INDEX_NONE
				? Sources.Traffic->FindAgent(Flight->AgentId) : nullptr;
			if (Agent != nullptr && Agent->Phase == EAgentPhase::Stranded)
			{
				FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::FlightStranded, Flight->Id, NAME_None,
					FText::Format(NSLOCTEXT("OpsAlerts", "FlightStranded", "Flight {0} is stranded - retire or unstick it"),
						FText::FromString(Flight->Callsign))));
				OpsAlertFocusAgent(Alert, *Agent);
			}

			// UStandAllocator::HeldStandIsGone - the one test Reapply logs its Warning on ("an aeroplane is
			// coming for a stand nobody is holding"), for the phases Reapply is given: not yet on the ground.
			if (Flight->IsUnarrived() && Sources.Network != nullptr && UStandAllocator::HeldStandIsGone(*Flight, *Sources.Network))
			{
				{
					FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::HeldStandLost, Flight->Id, NAME_None,
						FText::Format(NSLOCTEXT("OpsAlerts", "HeldStandLost", "Flight {0}'s stand was removed - it has nowhere to park"),
							FText::FromString(Flight->Callsign))));
					Alert.Focus.Kind = EAlertFocusKind::Point;
					Alert.Focus.Point = Flight->RunwayPreference;
				}
			}

			// A HOLDING FLIGHT THAT CAN NEVER LAND (#442): its cached clearance is a refusal only the player can clear - the exit
			// deleted, the runway set to departures only, a bigger stand to build. Derived from the board's own cache (UFlightBoard::
			// UnlandableWhy, which never plans), so the alert clears by itself the moment the airport is fixed and the queue pass
			// re-judges it. The sentence is the planner's own, with its figures, and ends with the two things the player can do.
			// ENFORCED BY: AirportOps.Model.Alerts.UnlandableHoldingFlightRaisesAnAlert
			// NEEDS THE NETWORK: a judgement is only as fresh as the guideline graph it was made against (UnlandableWhy), and with no
			// network the alert cannot tell a current refusal from a stale one, so it raises none.
			// AN ACCEPTED FLIGHT TOO (#445): one accepted when the airport could take it, whose airport was then changed so it never
			// can - the exit deleted, the runway set to departures only - used to be alerted only once its ETA had brought it into
			// the queue, minutes of game time after the player did it. The same judgement, the same IsPermanentRefusal: the queue
			// pass judges an accepted flight when the network changes (UFlightBoard::JudgeUnarrived), so this reads a cache either way.
			// ENFORCED BY: AirportOps.Model.Alerts.AcceptedFlightThatCanNeverLandIsAlertedBeforeItsEta
			const EArrivalRefusal Unlandable = Sources.Network != nullptr ? Sources.Flights->UnlandableWhy(*Flight, *Sources.Network)
				: EArrivalRefusal::None;
			if (Unlandable != EArrivalRefusal::None)
			{
				const FText Sentence = FText::FromString(ArrivalPlanner::DescribeRefusal(Unlandable, Flight->Airframe.Wingspan));
				// "HOLDING" ONLY WHEN IT IS: an accepted flight has not come yet. The key is the flight's either way, so the row
				// keeps its place (and its toast history) when the ETA comes - the words change, which FAlertChangedEvent announces.
				FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::FlightCannotLand, Flight->Id, NAME_None,
					Flight->GetPhase() == EFlightPhase::Inbound
						? FText::Format(NSLOCTEXT("OpsAlerts", "FlightCannotLand", "Flight {0} is holding and cannot land. {1} Fix the airport, or cancel the flight."),
							FText::FromString(Flight->Callsign), Sentence)
						: FText::Format(NSLOCTEXT("OpsAlerts", "AcceptedFlightCannotLand", "Flight {0} is accepted and will not be able to land. {1} Fix the airport, or cancel the flight."),
							FText::FromString(Flight->Callsign), Sentence)));
				// NO AEROPLANE TO GO TO: it is off the map. The point the planner orders runways from is the nearest thing to a place.
				Alert.Focus.Kind = EAlertFocusKind::Point;
				Alert.Focus.Point = Flight->RunwayPreference;
			}
		}
	}

	// SERVICE: stranded vehicles, and jobs no vehicle can serve.
	if (Sources.Jobs != nullptr)
	{
		if (Sources.Traffic != nullptr)
		{
			for (const FServiceVehicle& Vehicle : Sources.Jobs->GetVehicles())
			{
				if (Vehicle.AgentId != 0 && UJobBoard::IsStranded(Vehicle, *Sources.Traffic))
				{
					FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::VehicleStranded, Vehicle.Id, NAME_None,
						// THE KIND'S NAME, as the card and the ledger say it (#430) - "Bowser #7", not "FUEL 7".
						FText::Format(NSLOCTEXT("OpsAlerts", "VehicleStranded", "{0} #{1} is stranded - unstick it"),
							FServiceFleet::NameOf(*Sources.Jobs, Vehicle.TypeCode), FText::AsNumber(Vehicle.Id))));
					if (const FRoadAgent* Agent = Sources.Traffic->FindAgent(Vehicle.AgentId))
					{
						OpsAlertFocusAgent(Alert, *Agent);
					}
				}
			}
		}
		for (const FServiceJob& Job : Sources.Jobs->GetJobs())
		{
			// REFUSED, OR ASKING AGAIN AFTER A REFUSAL (FServiceJob::IsStillRefused): the re-offer is Open for the frame before the
			// next bid, and reading "Unserviceable" alone cleared the alert and raised it again - a fresh toast per road drawn.
			// ENFORCED BY: AirportOps.Model.Alerts.RefusedJobReofferedDoesNotFlicker
			if (!Job.IsStillRefused())
			{
				continue;
			}
			// THE BOARD'S OWN WORDS for what is missing (UJobBoard::RefusalText) - a second wording here would
			// be a second account of why a stand gets no fuel. AND THE STAND'S OWN NUMBER, the one on the stand card and painted at its
			// turn-off (OpsNames::StandLabel): it printed the entity index, so the alert said "stand 0" beside a sign reading 4 (#447).
			FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::JobUnserviceable, Job.Id, NAME_None,
				FText::Format(NSLOCTEXT("OpsAlerts", "JobUnserviceable", "No fuel for stand {0}: {1}"),
					FText::FromString(OpsNames::StandLabel(Sources.Network, Job.Stand)), FText::FromString(UJobBoard::RefusalText(Job.Why)))));
			const FEntityInstance* Stand = Sources.Network != nullptr ? Sources.Network->GetEntity(Job.Stand) : nullptr;
			if (Stand != nullptr)
			{
				Alert.Focus.Kind = EAlertFocusKind::Entity;
				Alert.Focus.Id = Job.Stand.Index;
				Alert.Focus.Point = Stand->Position;
			}
		}
	}

	// THE AIRPORT'S STATUS (spec 2026-09-29-ops-batch3 §3). No runway is the player's to fix; a closure is the
	// player's own choice and raises nothing. While not Open the generator judges no airline, so a verdict left
	// from before the closure would be a stale alert: airlines are reported only while Open.
	const bool bOpen = Sources.Airport == nullptr || Sources.Airport->AdmitsArrivals();
	if (Sources.Airport != nullptr && Sources.Airport->Status() == EAirportStatus::NoRunway)
	{
		Found.Add(OpsAlertOf(EAlertKind::NoRunway, 0, NAME_None,
			NSLOCTEXT("OpsAlerts", "NoRunway", "No runway - build one to receive offers")));
	}

	// AIRLINES that cannot use the airport, named as the generator's own log line names them.
	if (Sources.Offers != nullptr && bOpen)
	{
		for (const TPair<FName, FAirlineOfferState>& Each : Sources.Offers->States)
		{
			if (Each.Value.bCouldCome)
			{
				continue;
			}
			FText Name = FText::FromName(Each.Key);
			for (const FAirlineOffers& Offering : Sources.Airlines)
			{
				if (Offering.Airline != nullptr && Offering.Airline->GetFName() == Each.Key)
				{
					Name = Offering.Airline->DisplayName;
				}
			}
			const FString Why = Sources.Offers->DescribeWhyNot(Each.Key);
			Found.Add(OpsAlertOf(EAlertKind::AirlineCannotCome, 0, Each.Key, Why.IsEmpty()
				? FText::Format(NSLOCTEXT("OpsAlerts", "AirlineCannotComeBare", "{0} cannot use this airport"), Name)
				: FText::Format(NSLOCTEXT("OpsAlerts", "AirlineCannotCome", "{0} cannot use this airport: {1}"), Name, FText::FromString(Why))));
		}
	}

	// DEADLOCKS NOBODY IN THE RING CAN TURN OUT OF - the class the resolver's own comment calls "a DESIGN problem
	// the player must be told about". Keyed by the lowest member, as the resolver keys a jam.
	//
	// NOT ALWAYS ALL AIRCRAFT (#455): a ring can now include a truck backing along a bay's leg, or an aeroplane being
	// pushed - members with no second line - so the text says what is in it (the count by class) instead of calling
	// every member an aircraft, and the remedy is worded for a way round OR OUT. The focus stays on the lowest-id
	// member whatever it is: the Show button takes the player to the jam, and any member of a ring is in it.
	// ENFORCED BY: AirportOps.Model.Alerts.DeadlockWithAReversingTruckDoesNotCallItAnAircraft
	if (Sources.Traffic != nullptr)
	{
		TArray<TArray<int32>> Cycles;
		Sources.Traffic->CurrentDeadlocks(Cycles);
		for (const TArray<int32>& Cycle : Cycles)
		{
			if (Cycle.Num() == 0)
			{
				continue;
			}
			const int32 Lowest = FMath::Min(Cycle);
			int32 Aircraft = 0;
			for (const int32 Member : Cycle)
			{
				const FRoadAgent* Who = Sources.Traffic->FindAgent(Member);
				Aircraft += (Who != nullptr && Who->Class == ETraversalClass::Aircraft) ? 1 : 0;
			}
			const int32 Vehicles = Cycle.Num() - Aircraft;
			FText Text;
			if (Vehicles == 0)
			{
				Text = FText::Format(NSLOCTEXT("OpsAlerts", "Deadlock", "{0} aircraft deadlocked - {1}"),
					FText::AsNumber(Aircraft), DeadlockRemedy());
			}
			else if (Aircraft == 0)
			{
				Text = FText::Format(NSLOCTEXT("OpsAlerts", "DeadlockVehicles", "{0} {0}|plural(one=vehicle,other=vehicles) deadlocked - {1}"),
					Vehicles, DeadlockRemedy());
			}
			else
			{
				Text = FText::Format(NSLOCTEXT("OpsAlerts", "DeadlockMixed", "{0} aircraft and {1} {1}|plural(one=vehicle,other=vehicles) deadlocked - {2}"),
					Aircraft, Vehicles, DeadlockRemedy());
			}
			FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::Deadlock, Lowest, NAME_None, Text));
			if (const FRoadAgent* Agent = Sources.Traffic->FindAgent(Lowest))
			{
				OpsAlertFocusAgent(Alert, *Agent);
			}
		}
	}

	// MONEY: a negative balance locks every paid placement (ULedger::CanAfford).
	if (Sources.Ledger != nullptr && Sources.Ledger->IsOverdrawn())
	{
		Found.Add(OpsAlertOf(EAlertKind::Overdrawn, 0, NAME_None,
			NSLOCTEXT("OpsAlerts", "Overdrawn", "Overdrawn - building is locked")));
	}

	// THE DIFF. A condition still true keeps its first RaisedAt but takes the fresh text and focus (an
	// aircraft moves; a refusal's reason can change); a new one is raised; a gone one is cleared.
	for (FOpsAlert& Now_ : Found)
	{
		const FOpsAlert* Was = Alerts.FindByPredicate([&Now_](const FOpsAlert& A) { return A.Key == Now_.Key; });
		if (Was != nullptr)
		{
			Now_.RaisedAt = Was->RaisedAt;
			// BUT SAID WHEN THE WORDS MOVED (#445): this refreshed the text and published nothing, so a window that kept its own
			// copy of the list kept the old reason for as long as the problem stood - and told the player to build the wrong thing.
			// The event invalidates and names the key; the model is the truth. NOT A RAISE: no toast, RaisedAt kept. Focus moves
			// with an aircraft every frame and is read when a row's Go is clicked, so it is no change.
			Now_.bReRaised = Was->bReRaised;
			// EXACT, not FText::EqualTo (a collation compare at its default level): a changed word is a change whatever a locale folds together.
			if (!Was->Text.ToString().Equals(Now_.Text.ToString(), ESearchCase::CaseSensitive))
			{
				UE_LOG(LogAirportOps, Verbose, TEXT("Alert changed: %s"), *Now_.Text.ToString());
				if (Bus != nullptr)
				{
					Bus->Publish(FAlertChangedEvent{ Now_.Key });
				}
			}
			continue;
		}
		Now_.RaisedAt = Now;
		Now_.bReRaised = bAfterReset;
		// VERBOSE: the bus's own "Bus: + AlertRaised {...}" line is the Log-level record (stage 1 review).
		UE_LOG(LogAirportOps, Verbose, TEXT("Alert raised: %s"), *Now_.Text.ToString());
		if (Bus != nullptr)
		{
			Bus->Publish(FAlertRaisedEvent{ Now_ });
		}
	}
	for (const FOpsAlert& Was : Alerts)
	{
		if (!Found.ContainsByPredicate([&Was](const FOpsAlert& A) { return A.Key == Was.Key; }))
		{
			UE_LOG(LogAirportOps, Verbose, TEXT("Alert cleared: %s"), *Was.Text.ToString());
			if (Bus != nullptr)
			{
				Bus->Publish(FAlertClearedEvent{ Was.Key });
			}
		}
	}
	Alerts = MoveTemp(Found);
	bAfterReset = false;
}
