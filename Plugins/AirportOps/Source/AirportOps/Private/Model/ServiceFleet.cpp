#include "Model/ServiceFleet.h"

#include "AirportOpsLog.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "ServiceFleet"

FText FServiceFleet::NameOf(FName TypeCode) const
{
	// THE ROW'S OWN NAME, else its code. The one resolver: UFacilityPurchases's labels and UOpsRuntime's toast ask here, so
	// the card, the feed and the ledger name a vehicle the same way.
	const FFuelVehicleSpec* Spec = Board.VehicleSpecs.Find(TypeCode);
	return Spec != nullptr && !Spec->DisplayName.IsEmpty() ? Spec->DisplayName : FText::FromName(TypeCode);
}

double FServiceFleet::PriceOf(FName TypeCode) const
{
	return Board.SpecFor(TypeCode).Price;
}

double FServiceFleet::ResaleOf(FName TypeCode) const
{
	return Board.SpecFor(TypeCode).ResaleValue();
}

FServiceVehicle& FServiceFleet::Create(FName TypeCode, EServiceRole Role, FEntityInstanceId Home, double Cargo)
{
	FServiceVehicle& Vehicle = Board.Vehicles.Add_GetRef(FServiceVehicleLifecycle::Create(Board.NextVehicleId++, TypeCode, Role, Home, Cargo));
	// BOTH COUNTERS: the re-bid keys on the transition counter and the offer verdict on the composition one, and a vehicle
	// that appeared moves each.
	++Board.FleetRevision;
	++Board.FleetCompositionRevision;
	return Vehicle;
}

int32 FServiceFleet::Add(FName TypeCode, FEntityInstanceId Home, EFleetOrigin Origin, double Now)
{
	if (!Home.IsSet() || TypeCode.IsNone())
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Fleet: adding '%s' for depot %d refused - no depot or no type"),
			*TypeCode.ToString(), Home.Index);
		return 0;
	}
	const FServiceVehicleType Type = Board.TypeFor(TypeCode);
	FServiceVehicle& Vehicle = Create(TypeCode, Type.Role, Home, FFuelRolePolicy::CapacityOf(Type));
	// READ BEFORE ANYTHING ELSE CAN MOVE THE ARRAY: Publish only enqueues, but the ledger's post can notify.
	const int32 Id = Vehicle.Id;
	const double Cargo = Vehicle.Cargo;

	// A NEW VEHICLE IS A CHANGE A REFUSED JOB CAN ANSWER DIFFERENTLY - whichever way it came. A refused job is terminal
	// until something changes, the Step's re-offer pass watches the guideline revision alone, and seeding used to
	// re-open nothing. Re-opened, not bid here: the next Step bids it, in its one sequence (the bus's FleetChanged wakes
	// that pass for a purchase; a seeded vehicle is made by the Step itself, which bids after it).
	// ENFORCED BY: AirportOps.Present.Facility.PurchaseWakesTheBoard, AirportOps.Model.Fleet.SeedingReopensRefusedJobs
	int32 Reopened = 0;
	for (FServiceJob& Job : Board.Jobs)
	{
		if (Job.State == EServiceJobState::Unserviceable && Job.Role == Type.Role)
		{
			Board.ReopenRefusedJob(Job);
			++Reopened;
		}
	}

	double Amount = 0.0;
	if (Origin == EFleetOrigin::Bought)
	{
		Amount = PriceOf(TypeCode);
		if (Board.Ledger != nullptr)
		{
			Board.Ledger->Post(Now, ELedgerCategory::Fleet, -Amount, FText::Format(LOCTEXT("BoughtVehicle", "Bought {0}"), NameOf(TypeCode)));
		}
		UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d gains bought vehicle %d %s (%.0f L at %.0f L/min); %d refused job(s) ask again"),
			Home.Index, Id, *TypeCode.ToString(), Cargo, Type.RatePerMinute, Reopened);
	}
	else
	{
		UE_LOG(LogAirportOps, Log, TEXT("Fleet: depot %d gains vehicle %d %s (%.0f L at %.0f L/min); %d refused job(s) ask again"),
			Home.Index, Id, *TypeCode.ToString(), Cargo, Type.RatePerMinute, Reopened);
	}
	if (Board.Bus != nullptr)
	{
		Board.Bus->Publish(FFleetChangedEvent{ Home.Index, Id, TypeCode,
			Origin == EFleetOrigin::Bought ? EFleetChange::Bought : EFleetChange::Seeded, Amount });
	}
	return Id;
}

bool FServiceFleet::Withdraw(int32 VehicleId, EFleetReason Reason, double Now)
{
	if (Reason == EFleetReason::Sold && !Board.CanRemoveVehicle(VehicleId))
	{
		return false;
	}
	const int32 Index = Board.Vehicles.IndexOfByPredicate([VehicleId](const FServiceVehicle& V) { return V.Id == VehicleId; });
	if (Index == INDEX_NONE)
	{
		return false;
	}
	// COPIED BEFORE THE REMOVE, which invalidates the vehicle.
	const FName TypeCode = Board.Vehicles[Index].TypeCode;
	const FEntityInstanceId Home = Board.Vehicles[Index].Home;
	const int32 Id = Board.Vehicles[Index].Id;
	if (Reason == EFleetReason::Sold)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Fleet: vehicle %d %s leaves depot %d"), Id, *TypeCode.ToString(), Home.Index);
	}
	Board.Vehicles.RemoveAt(Index);
	++Board.FleetRevision;
	++Board.FleetCompositionRevision;

	// PAID FOR, AS A SALE IS (ruled 2026-09-30): the player bought it, and removing its depot - a bulldoze, an undo of
	// the placement - is not a reason to lose its value. Resale, not the price: a vehicle that leaves for money leaves at
	// one rate (FFuelVehicleSpec::ResaleValue). Undo never touches this (R8): a re-placed depot does not buy the vehicle
	// back. A ZERO VALUE POSTS NO LINE: a row with no price has nothing to credit, and a "Sold X 0" line is noise.
	// ENFORCED BY: AirportOps.Model.Facility.DepotRemovalCreditsItsVehicles, AirportOps.Model.Facility.SellCreditsResale
	const double Credit = ResaleOf(TypeCode);
	if (Board.Ledger != nullptr && Credit > 0.0)
	{
		const FText Line = Reason == EFleetReason::Sold
			? FText::Format(LOCTEXT("SoldVehicle", "Sold {0} #{1}"), NameOf(TypeCode), FText::AsNumber(Id))
			: FText::Format(LOCTEXT("DepotRemovedVehicle", "{0} #{1} - depot removed"), NameOf(TypeCode), FText::AsNumber(Id));
		Board.Ledger->Post(Now, ELedgerCategory::Fleet, Credit, Line);
		if (Reason == EFleetReason::DepotRemoved)
		{
			UE_LOG(LogAirportOps, Log, TEXT("Purchase: depot %d removed; vehicle %d %s credited %.0f"),
				Home.Index, Id, *TypeCode.ToString(), Credit);
		}
	}
	if (Board.Bus != nullptr)
	{
		Board.Bus->Publish(FFleetChangedEvent{ Home.Index, Id, TypeCode,
			Reason == EFleetReason::Sold ? EFleetChange::Sold : EFleetChange::Withdrawn, Credit });
	}
	return true;
}

int32 FServiceFleet::SeedStarterFleets(const URoadNetwork& Network, double Now)
{
	// NEW DEPOTS GET THEIR PLACEHOLDER FLEET (spec §3.4): Trucks of every kind in FleetTypes, Idle and full. Once per
	// depot, so a vehicle that is out never gets a twin at home. FleetTypes read once: it copies the letter table.
	int32 Added = 0;
	const TArray<FName> Types = Board.FleetTypes();
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Depot = Entities[Index];
		if (!Depot.bAlive || Depot.PoseRole != EServiceRole::Fuel || Depot.Trucks <= 0)
		{
			continue;
		}
		const FEntityInstanceId DepotId = Network.EntityIdAt(Index);
		if (Board.SeededDepots.Contains(DepotId))
		{
			continue;
		}
		Board.SeededDepots.Add(DepotId);
		for (int32 Count = 0; Count < Depot.Trucks; ++Count)
		{
			for (const FName TypeCode : Types)
			{
				Added += Add(TypeCode, DepotId, EFleetOrigin::Seeded, Now) != 0 ? 1 : 0;
			}
		}
	}
	return Added;
}

void FServiceFleet::Clear()
{
	Board.Vehicles.Reset();
	// CLEARED, THEN RESTORED FROM THE BLOB (it is saved): a snapshot without it re-seeds each depot once, which is the old behaviour.
	Board.SeededDepots.Reset();
	++Board.FleetRevision;
	++Board.FleetCompositionRevision;
}

void FServiceFleet::Restored()
{
	for (const FServiceVehicle& Vehicle : Board.Vehicles)
	{
		// ITS DEPOT HAS BEEN SEEN: the placeholder must not add a second fleet beside a restored one.
		// ENFORCED BY: AirportOps.Fuel.RestoredFleetIsNotReseeded
		if (Vehicle.Home.IsSet())
		{
			Board.SeededDepots.Add(Vehicle.Home);
		}
	}
	++Board.FleetCompositionRevision;
	++Board.FleetRevision;
}

FServiceVehicle& FServiceFleet::AddForTest(FName TypeCode, FEntityInstanceId Home, EServiceVehicleState State, double Cargo)
{
	FServiceVehicle& Vehicle = Create(TypeCode, EServiceRole::Fuel, Home, Cargo);
	FServiceVehicleLifecycle::SeedStateForTest(Vehicle, State);
	if (Home.IsSet())
	{
		Board.SeededDepots.Add(Home);
	}
	return Vehicle;
}

#undef LOCTEXT_NAMESPACE
