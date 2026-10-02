#pragma once

#include "CoreMinimal.h"
#include "Model/TaxiReservations.h"

/**
 * ORDER ENFORCEMENT (spec 2026-10-02 §1; Hönig et al. 2019's action dependency graph): from the reservation table,
 * per resource, the sequence of aircraft due through it - and whether one of them may go now.
 *
 * ONLY ORDER, NEVER TIME (spec §2): an aircraft early on its plan waits for whoever is booked through ahead of it, and
 * one late makes everyone behind it wait - lateness propagates as waiting, and every wait points at a window booked
 * EARLIER on one timeline, so the waits cannot form a cycle.
 *
 * A pure function of the table and "who has entered what" - the claim pass's one more refusal ("not my turn: waiting
 * for X") is UTaxiPlanning's call of it. World-free, for a test.
 */
struct AIRSIDE_API FPassingOrder
{
	/**
	 * Who Holder waits for before it may enter Resource, or 0: every window on Resource booked ahead of Holder's
	 * first one there has been RELEASED (its holder's tail has cleared it) - or, for a window going the SAME WAY along
	 * an edge (FTaxiReservations::MayShare's FIFO), its holder has ENTERED it, since the two may follow each other
	 * down it. 0 too when Holder has no window on Resource: an unplanned aircraft is not this order's to hold.
	 */
	static int32 WaitingFor(const FTaxiReservations& Table, int32 Holder, const FTaxiResource& Resource,
		TFunctionRef<bool(int32 Other)> HasEntered);
};
