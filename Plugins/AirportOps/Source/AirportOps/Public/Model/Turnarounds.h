#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/ServiceJob.h"

class UGroundTraffic;
class UJobBoard;
class URoadNetwork;
class USimClock;
struct FAgentTransition;
struct FAirframe;

/**
 * Every parked aircraft's time on stand - its turnaround - and the decision that sends it away when that time is up
 * (issue #427: the third owner taken out of UJobBoard, after the vehicle catalogue, the fleet's door and movement).
 *
 * WHAT IT OWNS: the turnaround list; the three agent events that are an aircraft's (it parked at a stand, it left its
 * stand, it was sent off from somewhere it was never turned around); the departure pass (DepartTheReady ->
 * UGroundTraffic::DepartAgent); a turnaround's end (EndTurnaround, the publisher of FTurnaroundEndedEvent); and the
 * answer to "did this transition begin a turnaround" (BeganAt), which UFlightBoard reads as well.
 * WHAT IT DOES NOT OWN: the jobs. A turnaround NAMES its jobs (FTurnaround::JobIds - the user's ruling 8's per-flight
 * index), and the job list, the vehicles and the bidding stay UJobBoard's. So a turnaround that opens asks the board for
 * its job (UJobBoard::OpenJob), and one that closes hands its jobs back (UJobBoard::DropJobsOf), which moves the vehicles
 * out for them on.
 *
 * PATTERN: Extract Class, into a value-type COMPONENT that UJobBoard holds, with the board as the orchestrator - Step
 * calls DepartTheReady in its one sequence, and OnAgentPhase hands an aircraft's events here and a vehicle's to its own
 * handler. Every public name the board had stays on it, as a forwarder (TurnaroundFor, GetTurnarounds, DepartedLastStep,
 * HasRefusedDeparture, NextDeadline, FuelOutcomeOf, AddTurnaroundForTest).
 * ENFORCED BY: the build (every caller compiles unchanged), AirportOps.Model.Turnarounds.EachOwnerHearsItsOwnAgents,
 * AirportOps.Model.TurnaroundDeparts (DepartedLastStep), AirportOps.Present.PushGroundFreed.NetArmedAndCancelled
 * (HasRefusedDeparture arms the net)
 * NAMED DEVIATIONS, each forced:
 *  - A FRIEND OF UJobBoard, as FServiceFleet is: opening and closing a turnaround move the board's private RevisionCount
 *    (one counter for "a change no vehicle transition reports" - a second here would split Revision's contract between
 *    two owners, and RevisionCountForTest with it) and write its job list through OpenJob and DropJobsOf, which are
 *    private there so that only the board's own owners can call them. (It READS the jobs through the board's public
 *    FindJob and JobForAircraft, as ServiceText does.) A FRIEND SEES EVERYTHING, so what it may touch is held by lint
 *    instead: no write of the board's jobs, vehicles or job ids from this file, and no vehicle transition.
 *    ENFORCED BY: Check-Architecture rule 78(c)
 *  - THE BOARD IS PASSED PER CALL, NEVER HELD: this lives inside the board, and a stored back-pointer is exactly what a
 *    duplicated UObject copies wrongly (PIE duplicates the level, and a Transient pointer comes back aimed at the CDO's
 *    subobjects).
 *  - NOT A UObject AND NOT A USTRUCT: it holds no object reference and nothing of it is saved (see Items), so reflection
 *    would buy it nothing. The board's UPROPERTY(Transient) array it replaces was reflected only to say "not saved".
 */
class AIRPORTOPS_API FTurnarounds
{
public:
	/**
	 * THE ONE DERIVATION OF "A TURNAROUND BEGAN" (#427): the stand this transition parked the agent AT - a Parked cause
	 * whose GoalAtEvent is a stand's pose (StandAtNode) - or unset. BOTH BOARDS READ IT: UJobBoard opens a turnaround
	 * there (OnAircraftPhase), and UFlightBoard enters Turnaround and records the stand (UFlightBoard::OnAgentPhase,
	 * through FlightPhaseFromTransition's bParkedAtStand). They used to ask StandAtNode each for itself - the flight board
	 * on To == Parked, the job board on the Parked cause - two derivations of one fact, agreeing because the traffic model
	 * happens to report every Parked phase with the Parked cause.
	 *
	 * THE EVENT'S NODE, NOT THE LIVE AGENT'S (#436): it is heard a drain late, and an aircraft a same-frame re-offer has
	 * sent on to a stand has a live goal that IS a stand, while the node it parked on - the fallback junction - is not.
	 * WHAT EACH BOARD ADDS IS ITS OWN, not a second derivation: the job board opens a turnaround only for an aeroplane (an
	 * FAirframe carries the turnaround's figures) that has none open; the flight board keeps a flight already past its
	 * stand in Turnaround wherever it parks (FlightPhaseFromTransition's Parked case).
	 * ENFORCED BY: Check-Architecture rule 78 (turnaround-began-once: StandAtNode is called from this alone in production),
	 * AirportOps.Model.Turnarounds.BothBoardsBeginAtTheOneStand
	 */
	static FEntityInstanceId BeganAt(const URoadNetwork& Network, const FAgentTransition& Transition);

	/**
	 * How a departing aircraft left, from the FIGURES, not the job's state (batch 3 review I1): a job still
	 * being served when the player pressed Depart is part-fuelled, though it never went Unserviceable.
	 * Wanted <= 0 or delivered within FFuelRolePolicy::FuelledWithinLitres of it: Fuelled; nothing delivered:
	 * Unfuelled; otherwise PartFuelled. THE POLICY'S FIGURE, the one FinishServe calls a job Done by (DoneWithin), so
	 * the outcome and the fee use the same number - or a Done job 0.4 L short would read part-fuelled and be paid for
	 * twice (#443: it was a constant here beside two literals in the policy and two in the bid).
	 * HERE SINCE #427, with the departure it scores; UJobBoard::FuelOutcomeOf forwards to it.
	 */
	static EFuelOutcome FuelOutcomeOf(double Delivered, double Wanted);

	/** Every open turnaround. */
	const TArray<FTurnaround>& All() const { return Items; }

	/** The aircraft's turnaround, or null. */
	const FTurnaround* For(int32 AircraftId) const;

	/**
	 * An aircraft's phase change - the three cases of UJobBoard::OnAgentPhase that were a turnaround's (#427), in the order
	 * it took them: the aircraft LEFT ITS STAND with a turnaround open (Drop); it was SENT OFF FROM WHERE IT WAS NEVER
	 * TURNED AROUND (EndTurnaround, Unfuelled); it PARKED AT A STAND (a turnaround opens, and its fuel job with it). Any
	 * other change is none of a turnaround's business. Board is the board that holds this, for the jobs.
	 *
	 * MAPS THE TRANSITION'S CAUSE, not (From, To) plus the live agent (#436): it is heard a drain late, so what
	 * decides is what was true when the change was made - DepartOrdered, not bDepartureArmed read now; GoalAtEvent,
	 * not the GoalNode the agent may have been redirected to since.
	 */
	void OnAircraftPhase(UJobBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		const FAgentTransition& Transition);

	/**
	 * Send every aircraft whose turnaround has run out and whose jobs are finished - THE DEPARTURE DECISION.
	 *
	 * Its own function and not a step in the job loop, because it is a pass OVER the turnarounds
	 * rather than a transition of one: departing an aircraft removes its turnaround and jobs, so this
	 * cannot run inside a loop that walks them. See its body. UJobBoard::Step runs it, in its one sequence.
	 * It begins by forgetting the last pass's departures (DepartedLastPass): Step runs it once per Step, so "the last
	 * pass" is "the last Step", which is what UOpsRuntime reads it as.
	 */
	void DepartTheReady(UJobBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock);

	/**
	 * The aircraft the last DepartTheReady got away - UJobBoard::DepartedLastStep's answer. The ops runtime reads it
	 * after a run the safety net alone asked for, to name the departure no event covered.
	 */
	const TArray<int32>& DepartedLastPass() const { return LastStepDeparted; }

	/**
	 * Is any turnaround due, not being served, and refused its departure by the last try? What keeps the ops
	 * safety net armed for departures: such an aircraft is retried only by an event now (see UJobBoard::Step), and the net
	 * is for the one whose event never came. DERIVED from the turnarounds and their jobs, asked after every Step.
	 */
	bool HasRefusedDeparture(const UJobBoard& Board, double Now) const;

	/** The earliest turnaround deadline after Now, Max double for none - the turnarounds' half of UJobBoard::NextDeadline. */
	double NextDeadline(double Now) const;

	/**
	 * A turnaround for AircraftId with nothing in it but its deadline and stand - the first half of a real open (the
	 * Parked case of OnAircraftPhase, which then asks the board for its job) and the whole of UJobBoard::AddTurnaroundForTest.
	 * Moves no revision: each caller moves the board's at the change it makes.
	 */
	FTurnaround& Open(int32 AircraftId, FEntityInstanceId Stand, double TurnaroundEndsAt);

	/** Every turnaround forgotten - a load (UJobBoard::OnBeforeRestore: they name agents, and a load clears every agent). */
	void Clear() { Items.Reset(); }

private:
	FTurnaround* Find(int32 AircraftId);

	/** True while any of the turnaround's jobs is neither Done nor Unserviceable. One rule, read by
	 *  DepartTheReady and by HasRefusedDeparture - so the two cannot disagree. */
	bool IsBeingServed(const UJobBoard& Board, const FTurnaround& Turnaround) const;

	/**
	 * The aircraft left its stand: its turnaround goes, then its jobs (UJobBoard::DropJobsOf, which moves the vehicles out
	 * for them on). WAS UJobBoard::DropAircraft, split at the line between the two owners (#427) and run in its order.
	 *
	 * bDeparted - it left for a departing phase (pushed back, taxied), not Gone or Stranded - makes this
	 * where a TURNED-AROUND aircraft's turnaround ends (batch 3 review I1): it calls EndTurnaround, the one
	 * publisher of FTurnaroundEndedEvent and poster of the part-fuelled fee, whoever sent it - DepartTheReady,
	 * or the inspector's Depart calling UGroundTraffic::DepartAgent directly, which never passes through
	 * DepartTheReady. EndTurnaround's other caller is OnAircraftPhase, for a departure never turned around. A retire
	 * (Unstick's despawn) is not a departure: PR B scores it as a cancelled flight.
	 * ENFORCED BY: AirportOps.Fuel.ManualDepartEndsTurnaroundOnce, AirportOps.Fuel.RetiredAircraftEndsNoTurnaround
	 */
	void Drop(UJobBoard& Board, int32 AircraftId, bool bDeparted, UGroundTraffic& Traffic, const URoadNetwork& Network,
		const USimClock& Clock);

	/**
	 * A turnaround's end, announced - THE ONE PUBLISHER of FTurnaroundEndedEvent (and poster of the part-fuelled fee).
	 * Two callers, one per way an aircraft can leave: Drop for one that had a turnaround, and OnAircraftPhase for
	 * one that departed WITHOUT ever being turned around (whole-stack review M4) - which can never both be true of one
	 * departure, since OnAircraftPhase takes the second branch only when For finds nothing.
	 * ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut ("one TurnaroundEnded"), AirportOps.Fuel.ManualDepartEndsTurnaroundOnce
	 */
	void EndTurnaround(UJobBoard& Board, int32 AircraftId, FEntityInstanceId Stand, double Delivered, double Wanted,
		const USimClock& Clock);

	/**
	 * The litres AgentId's flight is owed: the board's LitresOwedFor (the offer's FuelLitres), else UJobBoard::DefaultLitres.
	 * ONE READ for both sites that ask - a turnaround's fuel job and a departure never turned around (whole-stack re-review
	 * m1) - so the two cannot be owed different amounts.
	 * ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut ("owed what the flight was owed")
	 */
	static double LitresWanted(const UJobBoard& Board, int32 AgentId, const FAirframe& Airframe);

	/**
	 * NOT SAVED, and not reflected at all since #427 (it was UJobBoard's UPROPERTY(Transient) - PR #137 review, issue #105
	 * item 8): turnarounds NAME AGENTS, and agents are never saved - see OpsRuntimeTest's own "agents do not survive a
	 * load" assertion. Nor do the aircraft they are for come back: a load re-arms Accepted and Inbound flights only, so a
	 * turnaround restored from a save would be one for an aircraft that is not there. Saved, they would be serialized
	 * straight into the Fuel blob and restored right over what UJobBoard::OnBeforeRestore cleared - the leak it exists to fix.
	 */
	TArray<FTurnaround> Items;

	/** See DepartedLastPass. One pass's answer, not saved. */
	TArray<int32> LastStepDeparted;
};
