#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/ServiceVehicle.h"

struct FFuelVehicleSpec;
class UJobBoard;
class URoadNetwork;

// EFleetOrigin, the two ways IN of FServiceFleet::Add, lives in ServiceVehicle.h since #487: a vehicle now CARRIES how it
// came (FServiceVehicle::Origin), and a USTRUCT field needs a UENUM, which a header with no .generated.h cannot declare.

/** Why a vehicle is leaving: the two ways OUT of FServiceFleet::Withdraw. Plain enum, like EFleetChange. */
enum class EFleetReason : uint8
{
	/** The player sold it (UFacilityPurchases::SellVehicle): only an idle vehicle may go, and it is credited its resale. */
	Sold,
	/** Its depot was removed (a bulldoze, an undo of the placement): it goes whatever it was doing, credited its resale. */
	DepotRemoved
};

/**
 * THE FLEET'S MEMBERSHIP DOOR (issue #443): every vehicle that joins or leaves a depot's fleet goes through Add or
 * Withdraw, and every change owes the same three things whichever way it came.
 *
 * WHAT THIS REPLACED was four doors with four sets of side effects. The player's purchase went through
 * UJobBoard::AddPurchasedVehicle (its ledger line and FleetChanged came from UFacilityPurchases, the re-open of refused
 * jobs from the board); the starter seeding was a loop in SyncFleet that re-opened nothing and told nobody; the player's
 * sale was RemoveVehicle plus UFacilityPurchases; and a removed depot's vehicles were withdrawn by SyncFleet, where THE
 * JOB BOARD POSTED FLEET MONEY ITSELF, in its own wording, and published no event. So the feed could never say "Bowser #3
 * credited, depot removed", and any FleetChanged subscriber missed half the changes. The three things every change owes:
 *  - ITS LEDGER LINE: a purchase charges the type's price, a sale or a removal credits its resale value, a seeded vehicle
 *    is free. ELedgerCategory::Fleet is posted from ServiceFleet.cpp alone (rule 43);
 *  - ITS EVENT: FFleetChangedEvent{Bought | Sold | Seeded | Withdrawn}, published to the board's bus when it has one;
 *  - ITS RE-OPEN, on an add: a refused job is terminal until something changes, and a vehicle that did not exist is the
 *    change (the Step's re-offer pass watches the guideline revision alone), so every add - a seeded one too - re-opens
 *    the refused jobs of its role. Re-opened, not bid here: the next Step bids them, in its one sequence.
 * It also moves BOTH fleet counters (UJobBoard::FleetRevision, which the re-bid keys on, and FleetCompositionRevision,
 * which the offer verdict's bFuelServable does), so a change of membership cannot be made without either hearing of it.
 * ENFORCED BY: Check-Architecture rule 43 (fleet-one-door) - no other production file writes the containers or names the
 * Fleet ledger category; AirportOps.Model.Fleet.* (each origin and reason, its money, its event, its re-open).
 *
 * PATTERN: a HANDLE constructed per use over the board, the way FServiceVehicleLifecycle is over a vehicle, and a friend of
 * UJobBoard. NAMED DEVIATIONS from a free-standing service class, each forced: the vehicles, the jobs and the two counters
 * are the board's private state (saved, or bumped by every transition), so a second owner of them would need a copy or a
 * pointer that outlives the board's arrays; and the depot removal runs INSIDE the board's Step, so the door cannot live
 * in UFacilityPurchases (whose dependency runs the other way) without a back-pointer and a fallback for the fixtures that
 * have no purchase service - which would be a second door. UFacilityPurchases uses this one, for the player's buy and sell.
 *
 * MONEY IS DATED BY THE CALLER's Now: a purchase has the clock (UFacilityPurchases::Clock), a Step has its own, and a
 * door that guessed would date a credit differently from the Step that caused it.
 */
class AIRPORTOPS_API FServiceFleet
{
public:
	explicit FServiceFleet(UJobBoard& InBoard) : Board(InBoard) {}

	/**
	 * A vehicle of TypeCode joins Home's fleet: Idle at Home and full, its row read through UJobBoard::TypeFor. Returns
	 * the new id, or 0 (logged) for an unset Home, a None type or a type the catalogue has no row for (#430: such a
	 * vehicle used to be made with a zero chassis). Bought charges the type's price to Fleet; Seeded is free.
	 * Publishes FleetChanged{Bought or Seeded} and re-opens every refused job of its role.
	 * ENFORCED BY: AirportOps.Model.Fleet.PurchasedVehicleIsIdleAndFull, .PurchaseReopensRefusedJobs, .SeedingPublishesFleetChanged
	 */
	int32 Add(FName TypeCode, FEntityInstanceId Home, EFleetOrigin Origin, double Now);

	/**
	 * VehicleId leaves the fleet. False, nothing changed, when there is no such vehicle - or, for Sold, when it may not go
	 * (UJobBoard::CanRemoveVehicle: R5, idle at home with nothing promised). DepotRemoved has no such precondition: the
	 * caller (SyncFleet) has already released its jobs and retired its agent, and a vehicle mid-refill whose depot is
	 * gone must still go. Credits RefundOf the vehicle to Fleet (a zero value posts no line - and a SEEDED vehicle's is zero,
	 * #487), publishes FleetChanged{Sold or Withdrawn}. A Sold vehicle leaves SeededDepots alone, so a sold starter fleet
	 * stays sold; a DepotRemoved one forgets its depot (#487), so an undo then redo of the placement, which restores the
	 * depot's exact {Index, Generation}, seeds the starter fleet again instead of finding the id already seen.
	 * [[nodiscard]]: false means nothing left the fleet, and a caller that assumed otherwise would credit or forget a vehicle.
	 * ENFORCED BY: AirportOps.Model.Fleet.OnlyAnIdleVehicleLeaves, AirportOps.Model.Facility.DepotRemovalCreditsItsVehicles,
	 * AirportOps.Model.Fleet.DepotRemovalPublishesFleetChanged
	 */
	[[nodiscard]] bool Withdraw(int32 VehicleId, EFleetReason Reason, double Now);

	/** What buying TypeCode costs: its row's Price. THE ONE READ - the quote's label, the purchase's judgement and charge and
	 *  the ledger line cannot differ, because none of them reads the row's Price but through here (and ResaleOf).
	 *  ENFORCED BY: Check-Architecture rule 43 (fleet-one-door): no production .cpp outside ServiceFleet.cpp reads a
	 *  vehicle row's Price or ResaleValue */
	double PriceOf(FName TypeCode) const;

	/** What selling TypeCode, or losing it with its depot, credits: its row's ResaleValue - the card's "Sell" label and a
	 *  bulldozer's credit cannot drift apart. NOT WHAT A VEHICLE FETCHES: that is RefundOf, which is this for a bought one. */
	double ResaleOf(FName TypeCode) const;

	/**
	 * What THIS vehicle is worth on its way out: its row's resale if the player bought it, nothing if it was seeded (#487).
	 * THE ONE READ of "does it pay" - Withdraw credits it, and the depot card's "Sell" label and UFacilityPurchases::SellVehicle's
	 * result quote it, so a label cannot promise money the ledger never gets.
	 * ENFORCED BY: AirportOps.Model.Fleet.SeededVehicleFetchesNothing
	 */
	double RefundOf(const FServiceVehicle& Vehicle) const;

	/** The name a ledger line, a card label, a toast, the depot card's fleet rows, the vehicle card and the stranded alert
	 *  use for TypeCode: the catalogue row's DisplayName, else the code. THE ONE RESOLVER of that rule (it was typed three
	 *  times: here, UFacilityPurchases::VehicleName and the runtime's toast; and the card and the alert printed the raw
	 *  code, #430). STATIC ON A CONST BOARD so the const readers - UJobBoard::VehicleLine and DescribeVehicle, and
	 *  OpsAlerts, which holds a const board - ask the same function the door does, not a second copy of it.
	 *  ENFORCED BY: Check-Architecture rule 4's 'vehicle display-name fallback' row (DisplayName.IsEmpty() in
	 *  ServiceFleet.cpp alone); AirportOps.Fuel.Describe.DepotBacklog, AirportOps.Model.Alerts.VehicleStrandedRaises */
	static FText NameOf(const UJobBoard& Board, FName TypeCode);
	FText NameOf(FName TypeCode) const { return NameOf(Board, TypeCode); }

	/**
	 * THE VEHICLE CATALOGUE, RESOLVED (#430): one FServiceVehicleType per scenario row, its figures from Rows and its
	 * chassis from ResolveChassis(code) - UAirsideSettings::ResolveVehicle in production, handed in because this is
	 * Model/ and may not reach Content/ (the way UJobBoard::ResolveVehicles is). Replaces the board's whole catalogue and
	 * its StarterFleet, which is Starter filtered to the rows that survived. Returns how many rows it kept.
	 *
	 * A ROW WITH NO CHASSIS IS DROPPED, WITH A WARNING - a None code or a zero wheelbase from the resolver. Before this, a
	 * buyable type no stand letter was designed for became a zero-size vehicle that passed every fit gate
	 * (VehicleFit::NoLargerThan compares zeros), dispatched at the default speed and drew with the default mesh, and
	 * nothing warned. A starter code with no row is dropped with a Warning too.
	 *
	 * HERE, ON THE FLEET'S DOOR, because the catalogue is what the door's other readers read - PriceOf, ResaleOf and
	 * NameOf - and the join is where a scenario row's resale value is asked for (FFuelVehicleSpec::ResaleValue, which
	 * rule 43 keeps in this file). NO BUMP of either fleet counter: no vehicle joined or left. A catalogue is resolved at
	 * attach, before any vehicle exists, and again after every load (UOpsRuntime::ApplyScenarioFigures, #449), onto the
	 * vehicles the restore brought back - their kinds are the save's codes, and one a retune dropped reads TypeFor's no-row
	 * answer, as an unknown code always has.
	 * ENFORCED BY: AirportOps.Fleet.EveryBuyableTypeHasAChassis, AirportOps.Fleet.CatalogueDropsARowWithNoChassis
	 */
	int32 ResolveCatalogue(const TMap<FName, FFuelVehicleSpec>& Rows, const TArray<FName>& Starter,
		TFunctionRef<FVehicle(FName)> ResolveChassis);

private:
	// THE DOOR'S OWN SURFACE, callable by the board alone (#461 review): a load's clear and restore, the test adder and
	// the starter seeding are the BOARD's to call - from OnBeforeRestore, Serialize, AddVehicleForTest and
	// UJobBoard::SeedStarterFleets - and public on a handle that any holder of a UJobBoard can make (Fleet()) they were a
	// way in for everyone else. The Seeded origin of Add stays public because Add takes it as data; SeedStarterFleets,
	// which calls it, is private, and the board's own public SeedStarterFleets is the one way to ask for it.
	// The containers themselves stay UJobBoard's private members with a friend, not a struct of their own: splitting the
	// data out of the god-class is #427's, and this handle would then hold it.
	friend class UJobBoard;

	/**
	 * Every live fuel depot not seen before that has Trucks > 0 gets Trucks x UJobBoard::StarterFleet vehicles through Add
	 * (Seeded), and is marked seen so a vehicle that is out never gets a twin at home and a sold starter fleet stays sold.
	 * Returns how many vehicles it added. WAS the first half of SyncFleet's own loop, which built its vehicles by hand, and
	 * ran in every job board Step: it is the "FleetSeed" pass's now (UJobBoard::SeedStarterFleets), woken by the network
	 * change that announces a placed depot (#443).
	 * ENFORCED BY: AirportOps.Model.Fleet.SoldStarterFleetIsNotReseededAfterLoad, AirportOps.Fuel.RestoredFleetIsNotReseeded
	 */
	int32 SeedStarterFleets(const URoadNetwork& Network, double Now);

	/** The fleet is emptied and forgotten - a load's OnBeforeRestore: no vehicles, no depot seen. Moves both counters. */
	void Clear();

	/** The archive has just replaced the vehicles (UJobBoard::Serialize, after it normalised each one): every restored
	 *  vehicle's depot counts as seen, so a placeholder is not added beside them, and both counters move. */
	void Restored();

	/**
	 * A vehicle staged where the tests want it: in State, carrying Cargo, its Home counted as seen (a test that places a
	 * depot's vehicles by hand means those to be the fleet, and the seeding must not add the placeholder's beside them).
	 * No money, no event, no re-open: it is a fixture's hand, not a way into the fleet.
	 */
	FServiceVehicle& AddForTest(FName TypeCode, FEntityInstanceId Home, EServiceVehicleState State, double Cargo);

	/** The vehicle as Add and AddForTest both make it, with the next id and the Origin it came by; moves both counters. Good
	 *  until the next add or removal. THE ONE CREATION SITE (the seeded and the bought vehicle were two hand copies, and the
	 *  test adder a third). */
	FServiceVehicle& Create(FName TypeCode, EServiceRole Role, FEntityInstanceId Home, double Cargo, EFleetOrigin Origin);

	UJobBoard& Board;
};
