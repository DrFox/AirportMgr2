#include "Model/ServiceFleet.h"

#include "AirportOpsLog.h"
#include "Model/JobBoard.h"
#include "Model/Ledger.h"
#include "Model/OpsDefinition.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"

#define LOCTEXT_NAMESPACE "ServiceFleet"

FText FServiceFleet::NameOf(const UJobBoard& Board, FName TypeCode)
{
	// THE ROW'S OWN NAME, else its code. The one resolver: UFacilityPurchases's labels, UOpsRuntime's toast, the depot
	// card's fleet rows, the vehicle card and the stranded alert ask here, so the card, the feed and the ledger name a
	// vehicle the same way. The catalogue row, not the scenario map: the row is what the vehicle IS (#430).
	const FServiceVehicleType* Row = Board.Catalogue.Find(TypeCode);
	return Row != nullptr && !Row->DisplayName.IsEmpty() ? Row->DisplayName : FText::FromName(TypeCode);
}

double FServiceFleet::PriceOf(FName TypeCode) const
{
	// NO ROW, NO PRICE - and no purchase: UFacilityPurchases refuses a code the catalogue lacks (UnknownType) before it
	// asks, and Add refuses one too. The old fallback row answered the same 0.
	// ENFORCED BY: AirportOps.Model.Facility.RefusalsChargeAndPublishNothing ("UnknownType vehicle"), AirportOps.Fleet.CatalogueDropsARowWithNoChassis
	const FServiceVehicleType* Row = Board.Catalogue.Find(TypeCode);
	return Row != nullptr ? Row->Price : 0.0;
}

double FServiceFleet::RefundOf(const FServiceVehicle& Vehicle) const
{
	// THE ONE READ OF "DOES IT PAY" (#487) - see the header. Seeded is free to acquire and so worth nothing to give back.
	return Vehicle.Origin == EFleetOrigin::Bought ? ResaleOf(Vehicle.TypeCode) : 0.0;
}

double FServiceFleet::ResaleOf(FName TypeCode) const
{
	const FServiceVehicleType* Row = Board.Catalogue.Find(TypeCode);
	return Row != nullptr ? Row->ResaleValue : 0.0;
}

int32 FServiceFleet::ResolveCatalogue(const TMap<FName, FFuelVehicleSpec>& Rows, const TArray<FName>& Starter,
	TFunctionRef<FVehicle(FName)> ResolveChassis)
{
	// REPLACED, NOT MERGED: a re-attach resolves the scenario afresh, and a row the new scenario dropped must go with it.
	Board.Catalogue.Reset();
	for (const TPair<FName, FFuelVehicleSpec>& Scenario : Rows)
	{
		const FName TypeCode = Scenario.Key;
		const FVehicle Chassis = ResolveChassis(TypeCode);
		// NO CHASSIS, NO KIND. Content answers None for a code it builds nothing for; a zero wheelbase is a chassis with
		// no axles, which routes and fits as nothing does. Either used to become a zero-size vehicle every fit gate passed.
		if (TypeCode.IsNone() || Chassis.TypeCode.IsNone() || !Chassis.Chassis.HasAxles())
		{
			UE_LOG(LogAirportOps, Warning,
				TEXT("Fleet: scenario vehicle row '%s' has no chassis in Content (UAirsideSettings::ResolveVehicle) - dropped; it cannot be bought or seeded"),
				*TypeCode.ToString());
			continue;
		}
		const FFuelVehicleSpec& Figures = Scenario.Value;
		FServiceVehicleType& Row = Board.Catalogue.Add(TypeCode);
		Row.TypeCode = TypeCode;
		// FUEL, WRITTEN AS FUEL: the board is fuel's until a second role is scheduled (#443 option a, UJobBoard's header).
		Row.Role = EServiceRole::Fuel;
		Row.Vehicle = Chassis;
		Row.Capacity = Figures.CapacityLitres;
		Row.RatePerMinute = Figures.FlowLitresPerMinute;
		Row.DisplayName = Figures.DisplayName;
		Row.Price = Figures.Price;
		Row.UpkeepPerDay = Figures.UpkeepPerDay;
		// THE ONE RESALE RULE, asked once, here (FFuelVehicleSpec::ResaleValue): ResaleOf reads the answer.
		Row.ResaleValue = Figures.ResaleValue();
	}

	// THE STARTER FLEET IS THE SCENARIO'S LIST, KEPT TO KINDS THAT EXIST: a starter code with no row would be refused by Add
	// once per depot, every seeding; refused here once instead.
	Board.StarterFleet.Reset();
	for (const FName TypeCode : Starter)
	{
		if (Board.Catalogue.Contains(TypeCode))
		{
			Board.StarterFleet.AddUnique(TypeCode);
		}
		else
		{
			UE_LOG(LogAirportOps, Warning, TEXT("Fleet: starter fleet kind '%s' has no catalogue row - dropped from the starter fleet"),
				*TypeCode.ToString());
		}
	}

	TArray<FName> Codes;
	Board.Catalogue.GenerateKeyArray(Codes);
	Codes.Sort(FNameLexicalLess());
	UE_LOG(LogAirportOps, Log, TEXT("Fleet: catalogue of %d kind(s) [%s] from %d scenario row(s); starter fleet [%s]"),
		Codes.Num(), *FString::JoinBy(Codes, TEXT(", "), [](FName Code) { return Code.ToString(); }), Rows.Num(),
		*FString::JoinBy(Board.StarterFleet, TEXT(", "), [](FName Code) { return Code.ToString(); }));
	return Board.Catalogue.Num();
}

FServiceVehicle& FServiceFleet::Create(FName TypeCode, EServiceRole Role, FEntityInstanceId Home, double Cargo, EFleetOrigin Origin)
{
	FServiceVehicle& Vehicle = Board.Vehicles.Add_GetRef(FServiceVehicleLifecycle::Create(Board.NextVehicleId++, TypeCode, Role, Home, Cargo));
	Vehicle.Origin = Origin;
	// BOTH COUNTERS: the re-bid keys on the transition counter and the offer verdict on the composition one, and a vehicle
	// that appeared moves each.
	++Board.FleetRevision;
	++Board.FleetCompositionRevision;
	return Vehicle;
}

int32 FServiceFleet::Add(FName TypeCode, FEntityInstanceId Home, EFleetOrigin Origin, double Now)
{
	// A KIND THE CATALOGUE LACKS IS REFUSED (#430), like a None one: its row would have no chassis, and the vehicle would
	// be a zero-size one every fit gate passed. UFacilityPurchases refuses it first (UnknownType); this is the door's own.
	if (!Home.IsSet() || TypeCode.IsNone() || !Board.Catalogue.Contains(TypeCode))
	{
		UE_LOG(LogAirportOps, Warning, TEXT("Fleet: adding '%s' for depot %d refused - no depot, no type, or no catalogue row"),
			*TypeCode.ToString(), Home.Index);
		return 0;
	}
	const FServiceVehicleType Type = Board.TypeFor(TypeCode);
	FServiceVehicle& Vehicle = Create(TypeCode, Type.Role, Home, FFuelRolePolicy::CapacityOf(Type), Origin);
	// READ BEFORE ANYTHING ELSE CAN MOVE THE ARRAY: Publish only enqueues, but the ledger's post can notify.
	const int32 Id = Vehicle.Id;
	const double Cargo = Vehicle.Cargo;

	// A NEW VEHICLE IS A CHANGE A REFUSED JOB CAN ANSWER DIFFERENTLY - whichever way it came. A refused job is terminal
	// until something changes, the Step's re-offer pass watches the guideline revision alone, and seeding used to
	// re-open nothing. Re-opened, not bid here: the next Step bids it, in its one sequence (the bus's FleetChanged wakes
	// that pass for a purchase; a seeded vehicle is made by the "FleetSeed" pass, which runs before the job board's pass
	// in the same drain, so that pass bids after it).
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
	const double Credit = RefundOf(Board.Vehicles[Index]);
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
	//
	// ONLY WHAT WAS BOUGHT (#487): Credit is RefundOf, read above before the remove - zero for a SEEDED vehicle. A starter depot's
	// free fleet paid out when the depot was bulldozed, and bulldoze-then-re-place made that a repeatable money source.
	// ENFORCED BY: AirportOps.Model.Facility.DepotRemovalCreditsItsVehicles, AirportOps.Model.Facility.SellCreditsResale,
	// AirportOps.Model.Fleet.SeededVehicleFetchesNothing
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

int32 FServiceFleet::ForgetRemovedDepots(const URoadNetwork& Network)
{
	// BY THE DEPOT, NOT BY ITS VEHICLES (#487, PR #491 review) - see the declaration. A sold-out starter depot has no vehicle to
	// withdraw, so a forget keyed on withdrawals never reached it; asking the network which depots still stand reaches them all.
	// A stale id (the slot's generation moved on) is null here too, so ids of depots bulldozed long ago are pruned as well.
	TArray<FEntityInstanceId> Gone;
	for (const FEntityInstanceId DepotId : Board.SeededDepots)
	{
		const FEntityInstance* Depot = Network.GetEntity(DepotId);
		if (Depot == nullptr || !Depot->bAlive)
		{
			Gone.Add(DepotId);
		}
	}
	for (const FEntityInstanceId DepotId : Gone)
	{
		Board.SeededDepots.Remove(DepotId);
	}
	if (Gone.Num() > 0)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Fleet: %d removed depot(s) forgotten - one restored by an undo is seeded again"), Gone.Num());
	}
	return Gone.Num();
}

int32 FServiceFleet::SeedStarterFleets(const URoadNetwork& Network, double Now)
{
	// NEW DEPOTS GET THEIR PLACEHOLDER FLEET (spec §3.4): Trucks of every kind in StarterFleet, Idle and full. Once per
	// depot, so a vehicle that is out never gets a twin at home. StarterFleet read once, by copy: Add can publish, and a
	// subscriber is not promised to leave the board's list alone. (FleetTypes() was read once because it copied the
	// letter table; it is the scenario's own list since #430.)
	int32 Added = 0;
	const TArray<FName> Types = Board.StarterFleet;
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
	// NO LOOP MARKING A RESTORED VEHICLE'S DEPOT SEEN (#462 #11, 2026-10-01): SeededDepots is SAVED, so a load hands every seen depot back
	// whole - a sold-out one included - and the placeholder cannot add a second fleet beside a restored one. The loop that re-derived the set
	// from the vehicles was a shim for a snapshot written before the set was saved (2026-09-29): nothing pinned it (deleting it turned no
	// test red), there are no player saves (owner ruling 2026-09-23), and dead migration code is removed (2026-09-30).
	// ENFORCED BY: AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad, AirportOps.Present.Fleet.LoadDoesNotReseedADepotThatHasVehicles
	// (both go red if a restore forgets the seen depots)
	++Board.FleetCompositionRevision;
	++Board.FleetRevision;
}

FServiceVehicle& FServiceFleet::AddForTest(FName TypeCode, FEntityInstanceId Home, EServiceVehicleState State, double Cargo)
{
	// BOUGHT, as every fixture built with this door has been treated since it was written: they model a fleet the player owns, and
	// the tests that want a starter vehicle make one through Add(Seeded) - the door the game uses.
	FServiceVehicle& Vehicle = Create(TypeCode, EServiceRole::Fuel, Home, Cargo, EFleetOrigin::Bought);
	FServiceVehicleLifecycle::SeedStateForTest(Vehicle, State);
	if (Home.IsSet())
	{
		Board.SeededDepots.Add(Home);
	}
	return Vehicle;
}

#undef LOCTEXT_NAMESPACE
