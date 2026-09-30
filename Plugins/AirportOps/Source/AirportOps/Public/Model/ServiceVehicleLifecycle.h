#pragma once

#include "CoreMinimal.h"
#include "Model/ServiceVehicle.h"

/**
 * THE ONE WRITER of an FServiceVehicle's State, AgentId and CurrentJob (issue #428).
 *
 * Ruling 1 of the service-vehicle spec is "the vehicle owns its state". For two stages it did not: the vehicle
 * was a passive USTRUCT and fifteen `State =` writes, nine `AgentId =` writes and six spellings of "leave the
 * road" were scattered over whichever UJobBoard function happened to be running. The enum's contract - which
 * states have an agent, which have a job - was checked nowhere, and the illegal "Serving with no job" it allowed
 * was caught by a per-Step backstop that kept the whole board pass running every frame. UJobBoard now decides
 * WHICH transition, and this decides what a transition IS: what it moves, what it clears, what it bumps.
 *
 * PATTERN: a state machine as NAMED TRANSITIONS over an enum (EServiceVehicleState), with the transition table
 * in one file. NAMED DEVIATIONS from the textbook State pattern, each forced:
 *  - NO CLASS PER STATE: the vehicle is a UHT USTRUCT held in a TArray, so it cannot carry a polymorphic state
 *    object, and the states differ in what they PERMIT, not in behaviour.
 *  - A HANDLE CONSTRUCTED PER USE, not methods on the struct: a transition must bump the board's FleetRevision,
 *    and a USTRUCT in a TArray cannot safely hold a pointer to its owner (the array reallocates, and a copy
 *    would point at the original's board). The handle is two references and lives for one statement.
 *  - THE TRANSITIONS ASSERT, THEY DO NOT REFUSE: a call from the wrong state is a defect to be SEEN (ensureAlways
 *    names the transition and the state it found) and then carried through, not a reason to leave a vehicle
 *    half-moved - the board would then have a vehicle no transition describes, which is the disease.
 *  - IT KNOWS NOTHING OF TRAFFIC OR JOBS: retiring an agent and re-opening a job are the board's, because they
 *    need Airside and the job list. The board calls LeaveRoad AFTER deciding to retire, and before it retires
 *    (see UJobBoard::RetireAgentOf for why that order).
 *
 * THE INVARIANT, per state (the table on EServiceVehicleState): Violation reports a row that does not hold, and
 * every transition ends by asserting it. Deciding is the one transient state: it never survives a Step.
 *
 * ENFORCED BY: Check-Architecture rule 38 (vehicle-lifecycle-one-writer) - no other production file writes the
 * three fields; AirportOps.Model.Lifecycle.* (each transition's move, bump and invariant) and FFuelFixture's
 * per-Step check (every vehicle a fuel test drives).
 */
class AIRPORTOPS_API FServiceVehicleLifecycle
{
public:
	/** FleetRevision is the board's counter: every transition that changes the vehicle moves it once. */
	FServiceVehicleLifecycle(FServiceVehicle& InVehicle, uint32& InFleetRevision)
		: Vehicle(InVehicle), FleetRevision(InFleetRevision)
	{
	}

	/**
	 * A vehicle as it is born: Idle at Home, no agent, no job, an empty queue, carrying Cargo. THE ONE CREATION
	 * SITE (the seeded and the bought vehicle were two hand copies that could drift, and AddVehicleForTest a
	 * third); UJobBoard::NewVehicle gives it its id, its role and its full tank.
	 */
	static FServiceVehicle Create(int32 Id, FName TypeCode, EServiceRole Role, FEntityInstanceId Home, double Cargo);

	// ----- The transitions. Each names the state it may be called from, asserts it, and ends by asserting the
	// ----- invariant of the state it enters. Each that changes the vehicle moves FleetRevision once.

	/** A fresh agent leaves the depot for it. Idle -> Deciding: on the road, no job or trip chosen yet. */
	void Dispatched(int32 AgentId);

	/** The decision lands on a job. Deciding -> ToJob, holding JobId, driving the agent it already has. */
	void SetOff(int32 JobId);

	/** Arrived at the stand; the trip's pumping ends at EndsAt (USimClock game seconds). ToJob -> Serving. */
	void BeginServe(double EndsAt);

	/**
	 * The serve is over, the job's accounting is the board's. Serving -> Deciding: still parked at the stand,
	 * with its agent, and StartNext decides where next - after the re-bid, which prices it as standing here.
	 */
	void EndServe();

	/**
	 * Its job is taken from under it - the aircraft left, a recall, a stranding. ToJob or Serving -> Deciding.
	 * NOTHING HELD IS NO TRANSITION: a vehicle with no current job is left as it is and nothing is bumped, so
	 * the board can call it for every vehicle it releases.
	 */
	void ReleaseCurrentJob();

	/**
	 * Its purpose is home. Deciding or ToFacility -> ToFacility. The agent is driving there - or, stranded,
	 * waiting for the player to send it: ToFacility is what the vehicle is FOR, not that it is moving.
	 */
	void HeadHome();

	/**
	 * Off the road: the agent is gone, retired or lost. Deciding or ToFacility -> Idle, no agent. THE ONE SPELLING
	 * of "leave the road" (six, with six different tails, before #428): the board retires the agent itself.
	 */
	void LeaveRoad();

	/** At the facility, refilling or unloading until EndsAt. Idle -> AtFacility (no agent). */
	void BeginFacility(double EndsAt);

	/** The refill is done, or there is nothing to do at home. AtFacility -> Idle; already Idle is left alone. */
	void BecomeIdle();

	/**
	 * A load: whatever it was doing, and its whole queue, are things a load clears (agents, and the jobs of
	 * aircraft that are not restored). Any state -> Idle at home; keeps Cargo and Home, which the save keeps.
	 */
	void ResetForRestore();

	// ----- Queries: pure, so a test asserts them without a board.

	/** Whether State carries an agent / a job - the two columns of the table. */
	static bool HasAgent(EServiceVehicleState State);
	static bool HasJob(EServiceVehicleState State);

	/** In a timed step: Serving or AtFacility, which StepEndsAt then dates. THE ONE SPELLING of that predicate. */
	static bool IsTimed(const FServiceVehicle& Vehicle);

	/**
	 * Empty when the vehicle's (State, AgentId, CurrentJob) is a row of the table, else the row that fails - the
	 * message an ensure and a fixture both print. bSettled is the between-Steps form: it also refuses Deciding,
	 * which exists only inside one.
	 */
	static FString Violation(const FServiceVehicle& Vehicle, bool bSettled = false);

	/**
	 * Puts a vehicle in State BYPASSING every rule above - for the ForTest adders and a test staging a vehicle
	 * where no transition reaches, to see what the board does with it. Production has no business here.
	 */
	static void SeedStateForTest(FServiceVehicle& Vehicle, EServiceVehicleState State);

private:
	/** Assert Vehicle is in one of From for Transition; false, after ensuring, when it is not. */
	bool ExpectFrom(std::initializer_list<EServiceVehicleState> From, const TCHAR* Transition) const;

	/** Enter To: set it, move FleetRevision, and assert the row of the table for it. */
	void Enter(EServiceVehicleState To, const TCHAR* Transition);

	FServiceVehicle& Vehicle;
	uint32& FleetRevision;
};
