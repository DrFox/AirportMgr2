#include "Model/OpsAlerts.h"
#include "AirportOpsLog.h"
#include "Model/AirlineDefinition.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OfferGenerator.h"
#include "Model/OpsEventBus.h"
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

void UOpsAlerts::Reset()
{
	Alerts.Reset();
	bAfterReset = true;
	if (Bus != nullptr)
	{
		Bus->Publish(FAlertsResetEvent{});
	}
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
			const bool bComing = Flight->Phase == EFlightPhase::Accepted || Flight->Phase == EFlightPhase::Inbound;
			if (bComing && Sources.Network != nullptr && UStandAllocator::HeldStandIsGone(*Flight, *Sources.Network))
			{
				{
					FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::HeldStandLost, Flight->Id, NAME_None,
						FText::Format(NSLOCTEXT("OpsAlerts", "HeldStandLost", "Flight {0}'s stand was removed - it has nowhere to park"),
							FText::FromString(Flight->Callsign))));
					Alert.Focus.Kind = EAlertFocusKind::Point;
					Alert.Focus.Point = Flight->ApproachFocus;
				}
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
						FText::Format(NSLOCTEXT("OpsAlerts", "VehicleStranded", "{0} {1} is stranded - unstick it"),
							FText::FromName(Vehicle.TypeCode), FText::AsNumber(Vehicle.Id))));
					if (const FRoadAgent* Agent = Sources.Traffic->FindAgent(Vehicle.AgentId))
					{
						OpsAlertFocusAgent(Alert, *Agent);
					}
				}
			}
		}
		for (const FServiceJob& Job : Sources.Jobs->GetJobs())
		{
			if (Job.State != EServiceJobState::Unserviceable)
			{
				continue;
			}
			// THE BOARD'S OWN WORDS for what is missing (UJobBoard::RefusalText) - a second wording here would
			// be a second account of why a stand gets no fuel.
			FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::JobUnserviceable, Job.Id, NAME_None,
				FText::Format(NSLOCTEXT("OpsAlerts", "JobUnserviceable", "No fuel for stand {0}: {1}"),
					FText::AsNumber(Job.Stand.Index), FText::FromString(UJobBoard::RefusalText(Job.Why)))));
			const FEntityInstance* Stand = Sources.Network != nullptr ? Sources.Network->GetEntity(Job.Stand) : nullptr;
			if (Stand != nullptr)
			{
				Alert.Focus.Kind = EAlertFocusKind::Entity;
				Alert.Focus.Id = Job.Stand.Index;
				Alert.Focus.Point = Stand->Position;
			}
		}
	}

	// AIRLINES that cannot use the airport, named as the generator's own log line names them.
	if (Sources.Offers != nullptr)
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

	// ALL-AIRCRAFT DEADLOCKS - the class the resolver's own comment calls "a DESIGN problem the player must be
	// told about". Keyed by the lowest member, as the resolver keys a jam.
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
			FOpsAlert& Alert = Found.Add_GetRef(OpsAlertOf(EAlertKind::Deadlock, Lowest, NAME_None,
				FText::Format(NSLOCTEXT("OpsAlerts", "Deadlock", "{0} aircraft deadlocked - the layout needs another way round"),
					FText::AsNumber(Cycle.Num()))));
			if (const FRoadAgent* Agent = Sources.Traffic->FindAgent(Lowest))
			{
				OpsAlertFocusAgent(Alert, *Agent);
			}
		}
	}

	// MONEY: a negative balance locks every paid placement (ULedger::CanAfford).
	if (Sources.Ledger != nullptr && Sources.Ledger->Balance() < 0.0)
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
