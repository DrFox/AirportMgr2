#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/ServiceVehicle.h"

class UJobBoard;
class URoadNetwork;

/**
 * How a vehicle came to be in a fleet: the two ways IN of FServiceFleet::Add. Plain enums, like EFleetChange (this
 * header has no .generated.h, and nothing reflected holds either).
 */
enum class EFleetOrigin : uint8
{
	/** The player paid for it (UFacilityPurchases::BuyVehicle): charged the type's price. */
	Bought,
	/** The starter fleet a placed depot begins with (Trucks > 0, SeedStarterFleets): free. */
	Seeded
};

/** Why a vehicle is leaving: the two ways OUT of FServiceFleet::Withdraw. */
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
	 * A vehicle of TypeCode joins Home's fleet: Idle at Home and full, its row resolved through UJobBoard::TypeFor. Returns
	 * the new id, or 0 (logged) for an unset Home or a None type. Bought charges the type's price to Fleet; Seeded is free.
	 * Publishes FleetChanged{Bought or Seeded} and re-opens every refused job of its role.
	 * ENFORCED BY: AirportOps.Model.Fleet.PurchasedVehicleIsIdleAndFull, .PurchaseReopensRefusedJobs, .SeedingPublishesFleetChanged
	 */
	int32 Add(FName TypeCode, FEntityInstanceId Home, EFleetOrigin Origin, double Now);

	/**
	 * VehicleId leaves the fleet. False, nothing changed, when there is no such vehicle - or, for Sold, when it may not go
	 * (UJobBoard::CanRemoveVehicle: R5, idle at home with nothing promised). DepotRemoved has no such precondition: the
	 * caller (SyncFleet) has already released its jobs and retired its agent, and a vehicle mid-refill whose depot is
	 * gone must still go. Credits the resale value to Fleet (a zero value posts no line), publishes FleetChanged{Sold or
	 * Withdrawn}. Leaves SeededDepots alone, so a sold starter fleet stays sold.
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
	 *  bulldozer's credit cannot drift apart. */
	double ResaleOf(FName TypeCode) const;

	/** The name a ledger line, a card label and a toast use for TypeCode: the row's DisplayName, else the code. THE ONE
	 *  RESOLVER of that rule (it was typed three times: here, UFacilityPurchases::VehicleName and the runtime's toast). */
	FText NameOf(FName TypeCode) const;

private:
	// THE DOOR'S OWN SURFACE, callable by the board alone (#461 review): a load's clear and restore, the test adder and
	// the starter seeding are the BOARD's to call - from OnBeforeRestore, Serialize, AddVehicleForTest and SyncFleet -
	// and public on a handle that any holder of a UJobBoard can make (Fleet()) they were a way in for everyone else.
	// The Seeded origin of Add stays public because Add takes it as data; SeedStarterFleets, which calls it, is private.
	// The containers themselves stay UJobBoard's private members with a friend, not a struct of their own: splitting the
	// data out of the god-class is #427's, and this handle would then hold it.
	friend class UJobBoard;

	/**
	 * Every live fuel depot not seen before that has Trucks > 0 gets Trucks x UJobBoard::FleetTypes() vehicles through Add
	 * (Seeded), and is marked seen so a vehicle that is out never gets a twin at home and a sold starter fleet stays sold.
	 * Returns how many vehicles it added. WAS the first half of SyncFleet's own loop, which built its vehicles by hand.
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
	 * depot's vehicles by hand means those to be the fleet, and SyncFleet must not add the placeholder's beside them).
	 * No money, no event, no re-open: it is a fixture's hand, not a way into the fleet.
	 */
	FServiceVehicle& AddForTest(FName TypeCode, FEntityInstanceId Home, EServiceVehicleState State, double Cargo);

	/** The vehicle as Add and AddForTest both make it, with the next id; moves both counters. Good until the next
	 *  add or removal. THE ONE CREATION SITE (the seeded and the bought vehicle were two hand copies, and the test adder
	 *  a third). */
	FServiceVehicle& Create(FName TypeCode, EServiceRole Role, FEntityInstanceId Home, double Cargo);

	UJobBoard& Board;
};
