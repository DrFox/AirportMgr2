#include "Model/ServiceBid.h"

#include "Model/ServiceRolePolicy.h"

ServiceBid::FResult ServiceBid::Finish(const FInput& In, int32 MaxTrips)
{
	FResult Result;
	if (In.Type == nullptr || In.Policy == nullptr || !In.DriveSeconds)
	{
		Result.bReachable = false;
		return Result;
	}
	const IServiceRolePolicy& Policy = *In.Policy;
	const FServiceVehicleType& Type = *In.Type;

	double Time = In.FreeAt;
	double Cargo = In.CargoWhenFree;
	int32 Node = In.NodeWhenFree;

	/** Drive to To, or report the way missing. The one place a leg's time is added. */
	auto DriveTo = [&](int32 To) -> bool
	{
		const double Seconds = In.DriveSeconds(Node, To);
		if (Seconds < 0.0)
		{
			Result.bReachable = false;
			return false;
		}
		Time += Seconds;
		Node = To;
		return true;
	};

	/**
	 * One trip to Trip.Node, with the facility first if the policy says so - the SAME NextStep the
	 * live vehicle asks (UJobBoard::StartNext). Returns what it delivered, or a negative on no way.
	 */
	auto OneTrip = [&](const FTrip& Trip, double Owed) -> double
	{
		if (Policy.NextStep(Cargo, Type, Owed) == EServiceStep::ViaFacility)
		{
			// AT THE FACILITY ALREADY (an idle vehicle at home): the visit, with no drive to it.
			if (Node != In.FacilityNode && !DriveTo(In.FacilityNode))
			{
				return -1.0;
			}
			Time += Policy.FacilitySeconds(Cargo, Type, In.Pumps);
			Cargo = Policy.CargoAfterFacility(Cargo, Type);
			++Result.FacilityVisits;
		}
		if (!DriveTo(Trip.Node))
		{
			return -1.0;
		}
		const double Quantity = Policy.TripQuantity(Cargo, Type, Owed);
		Time += Policy.ServeSeconds(Type, Quantity);
		Cargo = Policy.CargoAfterServe(Cargo, Quantity);
		++Result.Trips;
		return Quantity;
	};

	// THE QUEUE AHEAD, one trip each - a queue entry is one commitment (user's ruling 5), and what a
	// trip leaves owed goes back to the board rather than staying with this vehicle.
	for (const FTrip& Queued : In.Queued)
	{
		if (OneTrip(Queued, Queued.Owed) < 0.0)
		{
			Result.Finish = Time;
			return Result;
		}
	}

	// THE JOB BID FOR, TO COMPLETION ALONE: this vehicle's own facility visits between its trips are
	// what makes a small tank's bid for a big job honest. "COMPLETE" IS THE POLICY'S DoneWithin, the figure the live
	// vehicle's NextStep and FinishServe judge by - a bid that priced a trip the vehicle then does not make is a
	// promise nobody keeps.
	double Owed = In.Appended.Owed;
	for (int32 Trip = 0; Trip < MaxTrips && Owed > Policy.DoneWithin(); ++Trip)
	{
		const double Delivered = OneTrip(In.Appended, Owed);
		if (Delivered < 0.0)
		{
			break;
		}
		// A TRIP THAT DELIVERS NOTHING (less than the policy would call a job done within) would repeat for ever - a
		// degenerate type the policy's floors should already have ruled out. Stop at the time reached rather than spin.
		if (Delivered < Policy.DoneWithin())
		{
			break;
		}
		Owed -= Delivered;
	}
	Result.Finish = Time;
	return Result;
}
