#include "Model/ServiceText.h"

#include "Model/ExhaustiveSwitch.h"
#include "Model/GameTimeText.h"
#include "Model/JobBoard.h"
#include "Model/OpsNames.h"
#include "Model/ServiceFleet.h"

// MOVED FROM JobBoard.cpp (#427): JobBoardText::StateName, RefusalText, VehicleLine, VehicleDoing, DescribeVehicle,
// DescribeDepot and DescribeAgent - bodies and comments as they were, the board's members reached through Board's
// public reads.

namespace ServiceText
{
	const TCHAR* StateName(EServiceVehicleState State)
	{
		switch (State)
		{
		case EServiceVehicleState::Idle:       return TEXT("Idle");
		case EServiceVehicleState::ToJob:      return TEXT("ToJob");
		case EServiceVehicleState::Serving:    return TEXT("Serving");
		case EServiceVehicleState::ToFacility: return TEXT("ToFacility");
		case EServiceVehicleState::AtFacility: return TEXT("AtFacility");
		case EServiceVehicleState::Deciding:   return TEXT("Deciding");
		default:                               return TEXT("?");
		}
	}

	// A MISSING CASE BELOW IS A BUILD ERROR - see ExhaustiveSwitch.h: a refusal added to EServiceRefusal must say what the player is
	// told. It had a `default:` that answered "unserviceable" for every value it did not name, so a new reason silently read as
	// the bare word and the player was sent nowhere - the thing this enum exists to avoid. ENFORCED BY: the build, checked by
	// adding a stray enumerator to EServiceRefusal and watching it fail here (2026-09-30).
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	const TCHAR* RefusalText(EServiceRefusal Why)
	{
		switch (Why)
		{
		// NOT REFUSED: there is no reason to give, and the bare word is what this has always answered for it.
		case EServiceRefusal::None:          return TEXT("unserviceable");
		case EServiceRefusal::NoDepot:       return TEXT("no fuel depot");
		case EServiceRefusal::NoRoad:        return TEXT("depot not on a road");
		// THE ENTRANCES, NAMED, since 2026-09-16 - the message the stand-routing spec promised and the
		// only one of the four that can say something more useful than the bare fact. A stand's lane
		// declares its entrances and a road reaches them or does not, so "not on a road" was true and sent
		// the player looking at the stand's own sides, where there is nothing to draw: the road has to
		// reach an ENTRANCE, which on a Code C is a corner of the nose or tail crossing. How many there are
		// is per-definition, so the count is not in the text - a number that has to agree with an asset is
		// a number that drifts.
		case EServiceRefusal::StandUnjoined: return TEXT("no road within reach of the stand's entrances");
		case EServiceRefusal::NoRoute:       return TEXT("no road from depot");
		case EServiceRefusal::TooNarrow:     return TEXT("no road wide enough for the fuel vehicle");
		case EServiceRefusal::NoPump:        return TEXT("depot has no pump");
		// THE VEHICLE, NOT THE ROAD OR THE STAND: the fix is a depot with a smaller vehicle, which is why
		// this does not share TooNarrow's text.
		case EServiceRefusal::VehicleTooLarge: return TEXT("the depot's vehicle is too large for this stand");
		// THE FIX, NAMED: the depot card has the buy button (facility-upgrades spec §4).
		case EServiceRefusal::NoVehicles:    return TEXT("depot has no vehicles - buy one");
		// NOT "buy one": the card lists the vehicle, and it is the listed one that cannot be used (#478) - sell it, buy a kind
		// the scenario has.
		case EServiceRefusal::UnknownVehicleKind: return TEXT("the depot's vehicles are of a kind this scenario no longer has - sell them and buy new");
		}
		// A VALUE OUTSIDE THE ENUM (a corrupt save's Why): not one of the cases above, and still worded.
		return TEXT("unserviceable");
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END

	FString VehicleDoing(const UJobBoard& Board, const FServiceVehicle& Vehicle, const URoadNetwork* Network)
	{
		const FServiceJob* Job = Board.FindJob(Vehicle.CurrentJob);
		// THE STAND BY ITS NUMBER, as the stand card and the sign painted at its turn-off say it (OpsNames::StandLabel) - not the entity index,
		// which a delete recycles and which starts at 0 (#447). No job, no stand: INDEX_NONE, as before.
		const FString Stand = Job != nullptr ? OpsNames::StandLabel(Network, Job->Stand) : FString::FromInt(INDEX_NONE);
		// THE DEPOT BY ITS NUMBER TOO, as the depot card's title says it (OpsNames::DepotLabel, #490) - not Home.Index, which a bulldoze recycles into a different depot.
		// The "Fuel:" / "Fleet:" log lines keep printing Home.Index: a diagnostic names the SLOT, and a grep of a log wants the same figure the code holds.
		const FString Depot = OpsNames::DepotLabel(Network, Vehicle.Home);
		switch (Vehicle.State)
		{
		case EServiceVehicleState::ToJob:      return FString::Printf(TEXT("to stand %s"), *Stand);
		case EServiceVehicleState::Serving:    return FString::Printf(TEXT("fuelling at stand %s"), *Stand);
		case EServiceVehicleState::ToFacility: return FString::Printf(TEXT("to depot %s"), *Depot);
		case EServiceVehicleState::AtFacility: return FString::Printf(TEXT("refilling at depot %s"), *Depot);
		case EServiceVehicleState::Deciding:   return FString(TEXT("deciding where next"));
		default:                               return FString::Printf(TEXT("at depot %s"), *Depot);
		}
	}

	FString VehicleLine(const UJobBoard& Board, const FServiceVehicle& Vehicle, const URoadNetwork* Network)
	{
		const FString Dot = TEXT(" · ");
		// THE KIND'S NAME, the one the shop sold it under (#430) - not its code: the card listed "FUEL #7" beside a "Bowser".
		return FString::Printf(TEXT("%s #%d"), *FServiceFleet::NameOf(Board, Vehicle.TypeCode).ToString(), Vehicle.Id) + Dot + VehicleDoing(Board, Vehicle, Network)
			+ Dot + FText::AsNumber(FMath::RoundToInt(Vehicle.Cargo)).ToString() + TEXT(" L");
	}

	FString DescribeVehicle(const UJobBoard& Board, const FServiceVehicle& Vehicle, const URoadNetwork* Network)
	{
		const FString Dot = TEXT(" · ");
		const FString Cargo = FText::AsNumber(FMath::RoundToInt(Vehicle.Cargo)).ToString() + TEXT(" L");
		const FString Queued = Vehicle.Queue.Num() > 0 ? Dot + FString::Printf(TEXT("%d queued"), Vehicle.Queue.Num()) : FString();
		return FServiceFleet::NameOf(Board, Vehicle.TypeCode).ToString() + Dot + VehicleDoing(Board, Vehicle, Network) + Dot + Cargo + Queued;
	}

	FDepotBacklog DescribeDepot(const UJobBoard& Board, FEntityInstanceId Depot, double Now, const URoadNetwork* Network)
	{
		const FString Dot = TEXT(" · ");
		auto Litres = [](double L) { return FText::AsNumber(FMath::RoundToInt(L)).ToString() + TEXT(" L"); };
		// A SPAN IN THE CLOCK'S OWN WORDS (GameTimeText::Duration, #447): "+1 h 35 min", as the aircraft card says it, where this printed "+95 min"
		// beside it. Still WHOLE MINUTES, rounded: with the inspector passing the minute's start as Now (see the header), its card
		// redraws at most once a game minute.
		auto Span = [](double Seconds) { return GameTimeText::Duration(Seconds).ToString(); };

		FDepotBacklog Out;
		TArray<FString> Lines;
		for (const FServiceVehicle& Vehicle : Board.GetVehicles())
		{
			if (Vehicle.Home != Depot)
			{
				continue;
			}
			Lines.Add(VehicleLine(Board, Vehicle, Network));

			// ITS JOBS IN THE ORDER IT WILL DO THEM: the one it is on, then its queue.
			TArray<int32> Order;
			if (Vehicle.CurrentJob != 0)
			{
				Order.Add(Vehicle.CurrentJob);
			}
			Order.Append(Vehicle.Queue);
			for (const int32 JobId : Order)
			{
				const FServiceJob* Job = Board.FindJob(JobId);
				if (Job == nullptr)
				{
					continue;
				}
				++Out.Jobs;
				Out.ClearsAt = FMath::Max(Out.ClearsAt, Job->PromisedFinish);
				FString Line = FString::Printf(TEXT("  stand %s"), *OpsNames::StandLabel(Network, Job->Stand)) + Dot + Litres(Job->QuantityOwed)
					+ Dot + TEXT("+") + Span(Job->PromisedFinish - Now);

				// LATE is the promise landing after the aircraft's turnaround: the one number that says the
				// backlog is costing the airport, not merely keeping the depot busy.
				const FTurnaround* Turnaround = Board.TurnaroundFor(Job->AircraftId);
				if (Turnaround != nullptr && Job->PromisedFinish > Turnaround->TurnaroundEndsAt)
				{
					++Out.LateJobs;
					Line += Dot + TEXT("late ") + Span(Job->PromisedFinish - Turnaround->TurnaroundEndsAt);
				}
				Lines.Add(Line);
			}
		}
		Out.Detail = FString::Join(Lines, TEXT("\n"));

		if (Out.Jobs == 0)
		{
			// NO VEHICLE AT ALL IS THE FIX THE CARD NAMES (facility-upgrades spec section 4): every job sits on a vehicle, so a depot with
			// none has no jobs, and "No jobs" would read as a healthy idle depot. The widget used to lay this over the summary in its own
			// wording (#447); the board owns both sentences now - RefusalText's "depot has no vehicles - buy one" is the same fact as a clause.
			Out.Summary = Board.VehiclesAt(Depot) == 0 ? FString(TEXT("No vehicles \u2014 buy one")) : FString(TEXT("No jobs"));
			return Out;
		}
		Out.Summary = FString::Printf(TEXT("%d job%s"), Out.Jobs, Out.Jobs == 1 ? TEXT("") : TEXT("s"))
			+ Dot + TEXT("clears in ") + Span(Out.ClearsAt - Now)
			+ (Out.LateJobs > 0 ? Dot + FString::Printf(TEXT("%d late"), Out.LateJobs) : FString());
		return Out;
	}

	FString DescribeAgent(const UJobBoard& Board, int32 AgentId, double Now, bool& bOutMovesWithClock, const URoadNetwork* Network)
	{
		bOutMovesWithClock = false;
		if (const FServiceVehicle* Vehicle = Board.VehicleForAgent(AgentId))
		{
			return DescribeVehicle(Board, *Vehicle, Network);
		}
		const FServiceJob* Job = Board.JobForAircraft(AgentId, EServiceRole::Fuel);
		const FString Dot = TEXT(" · ");
		if (Job == nullptr)
		{
			// A TURNAROUND WITH NO FUEL JOB is an aircraft that wanted none - it still says so.
			return Board.TurnaroundFor(AgentId) != nullptr ? TEXT("Fuel") + Dot + TEXT("none needed") : FString();
		}

		// THE CARD'S FUEL LINE (2026-09-28): the load, what is left, where the job has got to. Numbers
		// through FText::AsNumber so they group ("2,900") as the rest of the UI's do.
		auto Litres = [](double L) { return FText::AsNumber(FMath::RoundToInt(L)).ToString(); };
		// NO ZERO-LITRE BRANCH: a job is only ever opened for Litres > 0 (FTurnarounds' Litres <= 0 return comes before OpenJob), and
		// delivering moves litres from Owed to Delivered, so Total stays positive; the "none needed" an aircraft that wanted nothing
		// reads is the Job == nullptr line above. A Total <= 0 branch here was unreachable (#462, T8).
		// ENFORCED BY: AirportOps.Fuel.NoLitresNoTruck (a zero-litre aircraft has a turnaround and no job, and the card says so)
		const double Total = Job->QuantityOwed + Job->QuantityDelivered;
		const FString Head = FString::Printf(TEXT("Fuel %s L"), *Litres(Total));

		if (Job->State == EServiceJobState::Unserviceable)
		{
			return Head + Dot + RefusalText(Job->Why);
		}
		if (Job->State == EServiceJobState::Done)
		{
			return Head + Dot + (Job->Trips > 1 ? FString::Printf(TEXT("done in %d trips"), Job->Trips) : FString(TEXT("done")));
		}

		// LIVE WHILE PUMPING: QuantityOwed only moves when a trip ends, so the part of this trip's load
		// already pumped is the elapsed fraction of its pumping time - on the game clock, so a pause freezes
		// it. Rounded to 10 L so the card is not rebuilt every frame.
		double Left = Job->QuantityOwed;
		if (Job->State == EServiceJobState::Serving && Job->TripEndsAt > Job->TripStartedAt)
		{
			// THE ONE ANSWER THAT MOVES WITH THE CLOCK - the out-flag, which lets a caller keep
			// every other answer until Revision moves.
			bOutMovesWithClock = true;
			const double Fraction = FMath::Clamp((Now - Job->TripStartedAt) / (Job->TripEndsAt - Job->TripStartedAt), 0.0, 1.0);
			Left -= Job->TripQuantity * Fraction;
		}
		Left = FMath::RoundToDouble(FMath::Max(Left, 0.0) / 10.0) * 10.0;

		const TCHAR* Stage = Job->State == EServiceJobState::Serving ? TEXT("fuelling")
			: Job->State == EServiceJobState::Underway ? TEXT("truck en route")
			: TEXT("waiting for a truck");
		// TRIPS ONLY WHEN THERE IS MORE THAN ONE: this one plus what the rest will take in the tank that is
		// coming (or came).
		const int32 TotalTrips = Job->TankLitres > 0.0
			? Job->Trips + FMath::CeilToInt(Job->QuantityOwed / Job->TankLitres) : 0;
		const FString Trips = TotalTrips > 1
			? FString::Printf(TEXT(" (trip %d of %d)"), Job->Trips + 1, TotalTrips) : FString();
		return Head + Dot + FString::Printf(TEXT("%s L left"), *Litres(Left)) + Dot + Stage + Trips;
	}
}
