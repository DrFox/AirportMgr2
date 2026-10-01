#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Model/RoadHandles.h"

#include "StandAllocator.generated.h"

class UFlight;
class UGroundTraffic;
class URoadNetwork;

/**
 * Holding a stand for a flight so that nobody else takes it - and the tests of what a flight's hold is worth.
 *
 * IT HOLDS; IT DOES NOT CHOOSE. ArrivalPlanner picks the stand the aeroplane actually taxis to, from the nearest free
 * one it can REACH; the board hands that stand here. What this guarantees is that A stand exists for the flight from
 * the moment the player accepts it - which is the whole of "the player cannot over-commit". See UFlight::Stand for why
 * the held stand and the one parked on can differ and who reconciles them.
 *
 * EVERY HOLD IS A PLAN'S STAND (#431, #471): UFlightBoard::TryAccept holds the stand its accept's plan taxis to, and
 * UFlightBoard::Rehold - every re-hold: the queue's, a load's, a failed dispatch's, a hold an edit lost - holds the
 * stand a fresh plan taxis to. There was a Reserve here, the smallest admitted unheld stand, REACH-BLIND: it held a
 * stand nothing could taxi to while the flight waited NoFreeStand for the one it could, and the queue spec's ruling 2
 * ("every queued flight has somewhere to go") held by size only. It went with its last caller. Smallest-fit is still
 * the rule - ArrivalPlanner::ChooseStand's, which ranks the reachable stands by letter.
 * ENFORCED BY: Check-Architecture rule 4 ('UStandAllocator::Hold (a hold is the plan's stand)' - FlightBoard.cpp only)
 *
 * THE TABLE IS THE RECORD (#442): a hold is a claim in FTrafficOccupancy under the flight's negative holder id, and
 * UFlight::Stand is the board's saved copy of it. The two are re-made apart - Airside's rebuild re-makes the claims on
 * an edit, Reapply re-makes them from UFlight::Stand on a load - and ONE routine reconciles the copy to the record
 * after either: Reconcile below, through HoldIsLost.
 */
UCLASS()
class AIRPORTOPS_API UStandAllocator : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Hold THIS stand for the flight - the one an arrival plan chose (FArrivalPlan::StandNode), which UFlightBoard passes
	 * from TryAccept and Rehold - after the checks a stand must pass to be held at all: live, a stand, admitted
	 * (StandAdmission::Judge), and not held by another. Writes UFlight::Stand and returns true, or changes nothing.
	 */
	bool Hold(UGroundTraffic& Traffic, const URoadNetwork& Network, UFlight& Flight, FEntityInstanceId Stand);

	/** Give the hold back. Safe on a flight that never had one. */
	void Release(UGroundTraffic& Traffic, UFlight& Flight);

	/**
	 * True when Flight holds a stand that is no longer there to hold - deleted, or left with no pose node.
	 * ONE TEST, read by Reapply (which warns) and by the ops HeldStandLost alert (which tells the player),
	 * so the two cannot disagree about what "gone" means.
	 */
	static bool HeldStandIsGone(const UFlight& Flight, const URoadNetwork& Network);

	/**
	 * True when Flight names a LIVE stand that the occupancy table does not hold for it - its hold was refused when it
	 * was re-made (another holder had the stand) or dropped some other way. THE ONE TEST of "UFlight::Stand says one
	 * thing and the table another" (#442): Reconcile asks it after an edit and after a load alike,
	 * whichever routine re-made the claims. False for no stand, and for a gone one: that is HeldStandIsGone's case, kept
	 * named for the HeldStandLost alert.
	 * ENFORCED BY: AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable (an edit), AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand (a load)
	 */
	static bool HoldIsLost(const UFlight& Flight, const UGroundTraffic& Traffic, const URoadNetwork& Network);

	/**
	 * Re-make every hold from UFlight::Stand after a LOAD - in Held's order, so the earlier wins a stand two flights name.
	 *
	 * A load's network rebuild regenerated the guideline graph, and with it went every node claim
	 * (FTrafficOccupancy::ReleaseGuidelineClaims); the table is not saved, so UFlight::Stand is the only record left
	 * and the claims are re-made from it. The flight's saved truth is the stand ENTITY, whose handle survives a
	 * rebuild, so the pose node can be looked up again on the new graph.
	 *
	 * AN EDIT DOES NOT COME HERE: Airside's own rebuild re-makes the claims it dropped (UGroundTraffic::OnGraphRebuilt,
	 * since PR D's review I1) - this comment used to say that NOTHING re-made them and every edit cost every accepted
	 * flight its stand, which stopped being true then. A REFUSAL IS NOT HANDLED HERE EITHER (#442): it is warned and left,
	 * and Reconcile, which UFlightBoard::RestoreStandHolds runs straight after, gives it the one treatment an edit's refusal gets.
	 */
	void Reapply(UGroundTraffic& Traffic, const URoadNetwork& Network,
		const TArray<UFlight*>& Held);

	/**
	 * THE ONE CONFLICT RULE FOR A FLIGHT'S STAND (#442): bring each Accepted or Inbound flight's copy (UFlight::Stand) back
	 * to the record (the occupancy table), over InOrder in that order - the earlier wins a stand two of them want.
	 *  - A flight whose copy names a live stand the table does not hold for it (HoldIsLost - its hold was refused when it
	 *    was re-made, by Airside's rebuild on an edit or by Reapply on a load) GIVES IT UP and is re-held at once, whatever
	 *    its phase: the accept promised it a stand, and the next accept must not take the last one first.
	 *  - An Inbound flight with no stand, or a gone one, is re-held (the queue's rule since review I1): it is next to land.
	 * A gone stand on an Accepted flight is LEFT - the HeldStandLost alert's evidence (see Reapply) - until its ETA puts it
	 * in the queue. Rehold is the BOARD's (UFlightBoard::Rehold): the stand a fresh plan taxis the flight to - this class
	 * holds, a plan chooses. Returns how many flights gave a lost stand up, so the board can bump the revision its rows read.
	 *
	 * PATTERN: RECONCILIATION AGAINST A SYSTEM OF RECORD - the board observes the table, rather than Airside announcing which
	 * hold failed (#442's other option, a delegate per refusal). A refusal is one of several ways the copy and the record
	 * part - a dispatch's goal claim outranking a hold, a rebuild dropping a hold whose stand's pose moved, Reapply's
	 * refusal, whatever comes next - and asking the table catches each by one test, with no delegate across the plugin
	 * line. The edit IS still announced, once: FNetworkChangedEvent dirties the queue pass, which runs this (through
	 * UFlightBoard::ReconcileStandHolds) after its closed exit and before its paused one, so a paused edit is reconciled
	 * too. NOT #442's first option (Airside restores holds from a list the board hands it, UFlight::Stand a read of the
	 * table): UFlight::Stand has a second job - once parked it names the stand the aeroplane is on, which no hold records -
	 * and a gone stand, the HeldStandLost alert's evidence, is one the table can no longer hold at all.
	 * ENFORCED BY: AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable, AirportOps.Model.FlightSave.RequeueDoesNotTakeAnAcceptedStand,
	 * AirportOps.Model.ArrivalQueue.DeadStandReservesWhenOneFrees
	 */
	int32 Reconcile(const UGroundTraffic& Traffic, const URoadNetwork& Network, TConstArrayView<UFlight*> InOrder,
		TFunctionRef<bool(UFlight&)> Rehold);
};
