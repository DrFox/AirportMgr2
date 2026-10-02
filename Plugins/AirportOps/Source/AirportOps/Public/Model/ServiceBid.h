#pragma once

#include "CoreMinimal.h"

struct FServiceVehicleType;
class IServiceRolePolicy;

/**
 * When a vehicle would FINISH a job appended to its queue. Spec 2026-09-28-service-vehicle-lifecycle
 * §2.4, user's rulings 6 and the queue addendum.
 *
 * WHY A FINISH TIME, and not "who is free" or "who arrives first": both of those pick an idle utility
 * tow for a 4000 L job over a bowser five minutes from free, and the tow then takes four trips and
 * three refills to do what the bowser does in one. The measure that picks the bowser is when the
 * WHOLE job is done.
 *
 * WHY THE QUEUE IS SIMULATED rather than summed: a vehicle's cargo carries through its queue, so a
 * third job can cost a refill that neither of the first two did. The user's example: a tow with one
 * small job ahead can still win; with two ahead it loses, because the tank runs out partway.
 *
 * PURE - no world, no network, no clock. The board builds FInput from live state (where the current
 * step leaves the vehicle, route lengths over cruise speed) and this answers. Everything a role does
 * differently comes through the policy, the same functions the live lifecycle calls.
 */
namespace ServiceBid
{
	/** One trip to a stand. Node is opaque here - whatever DriveSeconds understands. */
	struct FTrip
	{
		int32 Node = 0;
		double Owed = 0.0;
	};

	struct FInput
	{
		/** When the vehicle's current step ends, what it will carry then, and where it will be. */
		double FreeAt = 0.0;
		double CargoWhenFree = 0.0;
		int32 NodeWhenFree = 0;

		/** Its facility (for fuel, the home depot's pose) and that facility's pump count. */
		int32 FacilityNode = 0;
		int32 Pumps = 1;

		/** What its facility can give right now (fuel: the airport's stock). A SNAPSHOT: two vehicles bidding at once
		 *  are both priced against the whole stock, and the one that refills second may get less than it was priced for.
		 *  Accepted - the bid is a ranking, re-run every decision, and the live draw (UJobBoard::BeginFacility) is exact.
		 *  Unbounded by default: a role whose facility never runs out, and every board with no supply. */
		double FacilityAvailable = TNumericLimits<double>::Max();

		/** Jobs already on its queue, ONE TRIP EACH: a queue entry is a single commitment, and what a
		 *  trip leaves owed goes back to the board (user's ruling 5). */
		TArray<FTrip> Queued;

		/** The job being bid for, served to completion ALONE - its own facility visits included. */
		FTrip Appended;

		const FServiceVehicleType* Type = nullptr;
		const IServiceRolePolicy* Policy = nullptr;

		/** GAME seconds from one node to another; negative when there is no way. */
		TFunction<double(int32 From, int32 To)> DriveSeconds;
	};

	struct FResult
	{
		double Finish = 0.0;
		int32 FacilityVisits = 0;
		int32 Trips = 0;
		bool bReachable = true;
	};

	/**
	 * The simulation. MaxTrips bounds a degenerate type (a tank the policy floors to nothing) - a bid
	 * that cannot finish reports the time it reached, with bReachable true, rather than spinning.
	 */
	AIRPORTOPS_API FResult Finish(const FInput& In, int32 MaxTrips = 64);
}
