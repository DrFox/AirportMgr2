#include "Model/StandAllocator.h"

#include "AirportOpsLog.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/StandAdmission.h"
#include "Model/TrafficOccupancy.h"

// NO Reserve (#471): the smallest admitted unheld stand, chosen here with no reach check, was every re-hold's choice
// until each re-hold went through a plan (UFlightBoard::Rehold) - see this class's header. Its ranking (IcaoCode::
// StandRank, strict less-than so the first-found stand won a letter tie) went with it: ranking is ChooseStand's, which
// ranks the same way over the stands it can reach. What it ASKED of a stand stayed - Hold below asks it of every stand.

bool UStandAllocator::Hold(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, FEntityInstanceId Stand)
{
	// A FLIGHT WITH NO AIRFRAME is a fixture nobody filled in. Refuse rather than hold the smallest stand on the field
	// for it - the refusal Reserve opened with, kept for every hold.
	const double Wingspan = Flight.Airframe.Wingspan;
	const FEntityInstance* Chosen = Stand.IsSet() ? Network.GetEntity(Stand) : nullptr;
	// IsStandCandidate, THE SAME FILTER ChooseStand applies - never a hand-spelled one. Reserve's walk once read bAlive +
	// PoseNode alone, with no kind check, and StandAdmits' "0 span admits anything" then handed a fuel depot (whose
	// DesignWingspan is 0) to any airliner as the smallest "stand" on the field (final review C1).
	// ADMISSION IS StandAdmission::Judge - the ONE rule ArrivalPlanner::ChooseStand also calls (surface, then size via
	// IcaoCode::StandAdmits, then service), so a stand held here for a flight is never one ChooseStand would have refused
	// it at touchdown - not for its letter, and not for grass under a jet either.
	// ENFORCED BY: Check-Architecture rule 4 row 'IcaoCode::StandAdmits' (only StandAdmission may ask size alone),
	// AirportOps.Model.StandAllocator.SkipsAGrassStandForATarmacFlight
	// HELD IS ASKED OF THE TABLE, not of any book-keeping here: a stand with an aeroplane ON it is held by that agent, and
	// a stand promised to another flight is held by that flight's holder id. One question covers both, which is the point
	// of one table.
	if (Wingspan <= 0.0 || Chosen == nullptr || !Chosen->IsStandCandidate()
		|| !StandAdmission::Judge(Network, *Chosen, Flight.Airframe).IsAdmitted()
		|| Traffic.IsStandHeld(Chosen->PoseNode, Flight.HolderId())
		|| !Traffic.HoldStand(Flight.HolderId(), Chosen->PoseNode))
	{
		return false;
	}

	Flight.Stand = Stand;
	UE_LOG(LogAirportOps, Log,
		TEXT("Flight %d holds stand %d: a %.0f uu stand for a %.0f uu span"),
		Flight.Id, Stand.Index, Chosen->DesignWingspan, Wingspan);
	return true;
}

void UStandAllocator::Release(UGroundTraffic& Traffic, UFlight& Flight)
{
	Traffic.ReleaseHold(Flight.HolderId());
}

void UStandAllocator::Reapply(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const TArray<UFlight*>& Held)
{
	for (UFlight* Flight : Held)
	{
		if (Flight == nullptr || !Flight->Stand.IsSet())
		{
			continue;
		}

		if (HeldStandIsGone(*Flight, Network))
		{
			// The stand was deleted under an accepted flight. Finding it another one belongs
			// to the sequencer's divert path, which does not exist yet - but the silence is
			// what must not happen: an aeroplane is coming for a stand nobody is holding.
			// KEPT, NOT CLEARED (a deviation from review ruling I1, which said clear it): the dead
			// Stand IS the HeldStandLost alert's evidence (UOpsAlerts reads HeldStandIsGone), and
			// every stand deletion reaches here through a rebuild - clearing it would silence that
			// alert in play. UFlightBoard::ReconcileStandHolds re-holds an Inbound flight a live one
			// instead, which overwrites it and clears the alert by the same test.
			// ENFORCED BY: AirportOps.Model.Alerts.HeldStandLostRaisesAndClears, AirportOps.Model.FlightSave.RequeueOffADeadStandReserves
			UE_LOG(LogAirportOps, Warning,
				TEXT("Flight %d held stand %d, which is gone from the graph"),
				Flight->Id, Flight->Stand.Index);
			continue;
		}

		// HONOURED, not dropped (review I1): a refusal means another holder has the stand - a
		// flight a load re-queued (#404) names the stand it was accepted onto, which another
		// flight may have been accepted onto since. Its Stand is then a promise it does not
		// hold - SAID HERE, AND LEFT (#442): this used to clear it on the spot, while an edit's
		// refusal (Airside's rebuild) only warned and kept it, so one conflict had two outcomes
		// by which door it came through. Both now leave the board's copy as it is, and
		// UFlightBoard::ReconcileStandHolds - which RestoreStandHolds runs straight after this -
		// finds it with HoldIsLost, gives it up and re-holds, as it does after an edit.
		// ENFORCED BY: AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand
		if (!Traffic.HoldStand(Flight->HolderId(), Network.GetEntity(Flight->Stand)->PoseNode))
		{
			UE_LOG(LogAirportOps, Warning,
				TEXT("Flight %d could not hold stand %d again - another holder has it"),
				Flight->Id, Flight->Stand.Index);
		}
	}
}

bool UStandAllocator::HeldStandIsGone(const UFlight& Flight, const URoadNetwork& Network)
{
	if (!Flight.Stand.IsSet())
	{
		return false;
	}
	const FEntityInstance* Stand = Network.GetEntity(Flight.Stand);
	return Stand == nullptr || !Stand->PoseNode.IsSet();
}

bool UStandAllocator::HoldIsLost(const UFlight& Flight, const UGroundTraffic& Traffic, const URoadNetwork& Network)
{
	if (!Flight.Stand.IsSet() || HeldStandIsGone(Flight, Network))
	{
		return false;
	}
	// THE FLIGHT'S OWN CLAIM, asked by holder - not IsStandHeld, which answers "is it held by anyone but me" and so reads
	// a stand somebody ELSE now has exactly as it reads a stand nobody has: the two are the same loss here.
	return Traffic.GetOccupancy().FindClaim(Flight.HolderId(),
		FTrafficResource::OfNode(Network.GetEntity(Flight.Stand)->PoseNode)) == nullptr;
}
