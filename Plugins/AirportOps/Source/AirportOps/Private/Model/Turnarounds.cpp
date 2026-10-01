#include "Model/Turnarounds.h"

#include "AirportOpsLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/Flight.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/ServiceRolePolicy.h"
#include "Model/ServiceText.h"
#include "Model/SimClock.h"

// MOVED FROM JobBoard.cpp (#427) with the three aircraft cases of OnAgentPhase, DropAircraft's turnaround half,
// EndTurnaround, LitresWanted, FuelOutcomeOf, IsBeingServed, HasRefusedDeparture, DepartTheReady and the turnaround
// half of NextDeadline - bodies, logs and comments as they were, the board's members reached through Board.

namespace JobBoardPhase
{
	AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
	/**
	 * Did this transition take the agent off where it stood UNDER ITS OWN POWER - sent somewhere - as against being
	 * removed (Retired, Cleared) or losing its road (Stranded)? THE ONE LIST, by cause (#436): OnAgentPhase typed it
	 * twice as a phase set, {Manoeuvring, Reversing, Taxiing, Departing}, once per branch that needed it. Every cause
	 * by name and no default, so a cause added to EAgentEvent is a BUILD ERROR here (C4062, raised around this
	 * function - see ExhaustiveSwitch.h), not a silent "no".
	 * ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (checked 2026-09-30 by a stray enumerator: the build failed here)
	 */
	bool LeftUnderItsOwnPower(EAgentEvent Cause)
	{
		switch (Cause)
		{
		case EAgentEvent::DepartOrdered:
		case EAgentEvent::Redirected:
		case EAgentEvent::ReOffered:
		case EAgentEvent::Rescued:
			return true;
		case EAgentEvent::None:
		case EAgentEvent::Vacated:
		case EAgentEvent::LinedUp:
		case EAgentEvent::Parked:
		case EAgentEvent::PushedBack:
		case EAgentEvent::Airborne:
		case EAgentEvent::Gone:
		case EAgentEvent::Stranded:
		case EAgentEvent::BackingIn:
		case EAgentEvent::BackedOut:
		case EAgentEvent::TouchedDown:
		case EAgentEvent::Dispatched:
		case EAgentEvent::Retired:
		case EAgentEvent::Cleared:
			return false;
		}
		return false;
	}
	AIRSIDE_EXHAUSTIVE_SWITCH_END
}

FEntityInstanceId FTurnarounds::BeganAt(const URoadNetwork& Network, const FAgentTransition& Transition)
{
	// THE PARKED CAUSE, and the node the EVENT parked it on - see the declaration. Not To == Parked: the cause is what
	// #436 made every consumer switch on, and the flight board's To was the one read of the pair left.
	return Transition.Cause == EAgentEvent::Parked ? StandAtNode(Network, Transition.GoalAtEvent) : FEntityInstanceId();
}

EFuelOutcome FTurnarounds::FuelOutcomeOf(double Delivered, double Wanted)
{
	if (Wanted <= 0.0 || Wanted - Delivered <= FFuelRolePolicy::FuelledWithinLitres)
	{
		return EFuelOutcome::Fuelled;
	}
	return Delivered <= 0.0 ? EFuelOutcome::Unfuelled : EFuelOutcome::PartFuelled;
}

const FTurnaround* FTurnarounds::For(int32 AircraftId) const
{
	return Items.FindByPredicate([AircraftId](const FTurnaround& Each) { return Each.AircraftId == AircraftId; });
}

FTurnaround* FTurnarounds::Find(int32 AircraftId)
{
	return Items.FindByPredicate([AircraftId](const FTurnaround& Each) { return Each.AircraftId == AircraftId; });
}

FTurnaround& FTurnarounds::Open(int32 AircraftId, FEntityInstanceId Stand, double TurnaroundEndsAt)
{
	FTurnaround& Turnaround = Items.AddDefaulted_GetRef();
	Turnaround.AircraftId = AircraftId;
	Turnaround.Stand = Stand;
	Turnaround.TurnaroundEndsAt = TurnaroundEndsAt;
	return Turnaround;
}

double FTurnarounds::LitresWanted(const UJobBoard& Board, int32 AgentId, const FAirframe& Airframe)
{
	return FMath::Max(Board.LitresOwedFor ? Board.LitresOwedFor(AgentId, Airframe) : UJobBoard::DefaultLitres(Airframe), 0.0);
}

void FTurnarounds::EndTurnaround(UJobBoard& Board, int32 AircraftId, FEntityInstanceId Stand, double Delivered, double Wanted,
	const USimClock& Clock)
{
	const EFuelOutcome Outcome = FuelOutcomeOf(Delivered, Wanted);
	// PART-FUELLED PAYS FOR WHAT IT GOT (review, 2026-09-28), HERE AT THE ONE SITE: a job that became
	// impossible after a trip, or one the player cut short with Depart, leaves with what it got and pays
	// for it. Never twice: FinishServe pays only a job it calls Done, which FuelOutcomeOf calls Fuelled.
	// What the shortfall costs the airline is the roster's to score, not the fee's.
	// ENFORCED BY: AirportOps.Fuel.PartFuelledPaysForWhatItGot, AirportOps.Fuel.ManualDepartEndsTurnaroundOnce
	if (Outcome == EFuelOutcome::PartFuelled)
	{
		Board.PostServiceFee(Clock.Now(), Delivered);
	}
	if (Board.Bus != nullptr)
	{
		Board.Bus->Publish(FTurnaroundEndedEvent{ AircraftId, Stand, Outcome, Delivered, Wanted });
	}
}

void FTurnarounds::Drop(UJobBoard& Board, int32 AircraftId, bool bDeparted, UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock)
{
	const FTurnaround* Turnaround = For(AircraftId);
	if (Turnaround == nullptr)
	{
		return;
	}
	const TArray<int32> JobIds = Turnaround->JobIds;
	const int32 StandIndex = Turnaround->Stand.Index;
	const FEntityInstanceId StandId = Turnaround->Stand;
	++Board.RevisionCount;   // See UJobBoard::Revision: the turnaround and its jobs are about to go.

	// THE TURNAROUND'S END, read BEFORE the jobs go (batch 3 review I1) - the one site, see the header.
	if (bDeparted)
	{
		const FServiceJob* Fuel = Board.JobForAircraft(AircraftId, EServiceRole::Fuel);
		const double Delivered = Fuel != nullptr ? Fuel->QuantityDelivered : 0.0;
		const double Wanted = Fuel != nullptr ? Fuel->QuantityDelivered + Fuel->QuantityOwed : 0.0;
		EndTurnaround(Board, AircraftId, StandId, Delivered, Wanted, Clock);
	}
	Items.RemoveAll([AircraftId](const FTurnaround& Each) { return Each.AircraftId == AircraftId; });

	// THEN ITS JOBS, and the vehicles out for them - the job board's, in the order DropAircraft ran them.
	Board.DropJobsOf(AircraftId, StandIndex, JobIds, Traffic, Network, Clock);
}

void FTurnarounds::OnAircraftPhase(UJobBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, const FAgentTransition& Transition)
{
	const int32 AgentId = Transition.AgentId;
	const EAgentPhase From = Transition.From;
	const EAgentPhase To = Transition.To;
	const bool bLeftUnderItsOwnPower = JobBoardPhase::LeftUnderItsOwnPower(Transition.Cause);

	// AN AIRCRAFT LEAVING ITS STAND, first: it departs, or is retired, or is deleted under the player's
	// hand. Its turnaround and jobs go, and any vehicle out for it moves on - its next job, or home.
	if (From == EAgentPhase::Parked && To != EAgentPhase::Parked && For(AgentId) != nullptr)
	{
		// A DEPARTURE, named by its cause rather than "not Gone": a retire and a road lost under it (Retired, Cleared,
		// Stranded) are not the aircraft leaving under its own power - see LeftUnderItsOwnPower.
		Drop(Board, AgentId, bLeftUnderItsOwnPower, Traffic, Network, Clock);
		return;
	}

	// A DEPARTURE THAT WAS NEVER TURNED AROUND (whole-stack review M4, ruling 2026-09-30): an aircraft parked on the
	// fallback junction - no stand, so no turnaround and no fuel job - that the inspector's Depart sends off. It
	// leaves Unfuelled, owed what its flight was offered at: LitresOwedFor, the same source a turnaround's fuel job
	// takes its load from (the Parked branch below), so the two cannot be owed different amounts. SENT OFF
	// FOR A RUNWAY, not merely moving: a Parked -> Taxiing that is the re-offer taking it to a stand (FallbackParkStaysTaxiIn)
	// is not a departure, and its turnaround at the stand will end it properly. THE CAUSE SAYS WHICH (#436) -
	// DepartOrdered, UGroundTraffic::DepartAgent's own - where this used to ask the live agent, a step late, whether
	// it was still armed for a departure. The agent is still asked for its AIRFRAME, which is identity, not state:
	// the litres owed are an aeroplane's figure, and only an agent started with an FAirframe carries one.
	// ENFORCED BY: AirportOps.Model.Bus.DepartFromFallbackReadsTaxiOut, AirportOps.Model.Bus.FallbackParkStaysTaxiIn
	if (From == EAgentPhase::Parked && bLeftUnderItsOwnPower)
	{
		const FRoadAgent* Leaving = Traffic.FindAgent(AgentId);
		const FAirframe* Airframe = Leaving != nullptr ? Leaving->AsAircraft() : nullptr;
		if (Airframe != nullptr && Transition.Cause == EAgentEvent::DepartOrdered)
		{
			const double Wanted = LitresWanted(Board, AgentId, *Airframe);
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d departed without a turnaround - %s, %.0f L owed"),
				AgentId, *UEnum::GetValueAsString(FuelOutcomeOf(0.0, Wanted)), Wanted);
			EndTurnaround(Board, AgentId, FEntityInstanceId(), 0.0, Wanted, Clock);
		}
		return;
	}

	if (Transition.Cause != EAgentEvent::Parked)
	{
		return;
	}

	const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
	if (Agent == nullptr)
	{
		return;
	}
	// A PARKED EVENT IS A FACT ABOUT THE PAST: the ops bus delivers it on the next ops step (spec
	// 2026-09-29 §1), and the agent may have moved on since. UGroundTraffic::ReofferStands redirects an
	// aircraft parked on a fallback junction to a stand that freed in the same frame, and its GoalNode is
	// then the NEW stand - acting on it would open a turnaround for an aircraft still taxiing in. Its own
	// Parked event for the real stand follows. SINCE #436 THE AIRCRAFT CASE IS THE EVENT'S OWN: GoalAtEvent is the
	// node it parked ON, the fallback junction, which is no stand - so no turnaround opens, and the live agent is no
	// longer asked whether it is still parked (see below).
	// ENFORCED BY: AirportOps.Present.Bus.StaleParkedOpensNoTurnaround
	//
	// AN AIRCRAFT THAT HAS PARKED. The node it parked on must be a STAND's pose - an aircraft parked on a taxiway
	// junction (the stand-death fallback) is at no stand and demands nothing, which falls out of this
	// same lookup rather than needing a rule of its own - BeganAt, the one derivation UFlightBoard reads too, so the
	// flight enters Turnaround exactly where a turnaround opens (review M8; #427 made it one function where it was two
	// StandAtNode calls). Asked of the EVENT's node (#436), so a Parked heard after a same-frame re-offer finds the
	// junction it parked on, not the stand it has been sent to since.
	// AsAircraft AS WELL AS Class: the turnaround below is an aeroplane's figure, and only an agent started with an
	// FAirframe carries one - the live agent is read for that, its identity, and for nothing that moves.
	const FAirframe* Aircraft = Agent->AsAircraft();
	if (Agent->Class != ETraversalClass::Aircraft || Aircraft == nullptr || For(AgentId) != nullptr)
	{
		return;
	}
	const FEntityInstanceId Stand = BeganAt(Network, Transition);
	if (!Stand.IsSet())
	{
		return;
	}

	++Board.RevisionCount;   // See UJobBoard::Revision: a turnaround (and below, usually a job) opens.

	// THE CLOCK STARTS WHEN THE WHEELS STOP, not when the fuelling finishes. A turnaround is the time on
	// stand, and the services happen INSIDE it - which is what lets baggage and catering be added later
	// without lengthening anything. The figure rides on the AGENT, in its airframe bundle, because this
	// class may not include Entities/ and so cannot ask the aircraft's type. See
	// FAirframe::TurnaroundSeconds.
	FTurnaround& Turnaround = Open(AgentId, Stand, Clock.Now() + Aircraft->TurnaroundSeconds);

	// THE LOAD, from the flight's own offer (LitresOwedFor), or the one fallback.
	const double Litres = LitresWanted(Board, AgentId, *Aircraft);
	if (Litres <= 0.0)
	{
		// WANTS NOTHING, BUT STILL TURNS ROUND: a turnaround with no job, which DepartTheReady sends at
		// its deadline. UFuelService faked this as a Done demand for zero litres, because its departure
		// pass walked demands; the turnaround is the thing that departs now.
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d parked at stand %d; wants no fuel, away in %.0f game s"),
			AgentId, Stand.Index, Aircraft->TurnaroundSeconds);
		return;
	}

	// ITS JOB IS THE BOARD'S to make (OpenJob): the job list is the board's, the turnaround only names what is in it.
	Turnaround.JobIds.Add(Board.OpenJob(AgentId, EServiceRole::Fuel, Stand, Litres).Id);
	UE_LOG(LogAirportOps, Log,
		TEXT("Fuel: aircraft %d parked at stand %d; needs %.0f L, away in %.0f game s at the earliest"),
		AgentId, Stand.Index, Litres, Aircraft->TurnaroundSeconds);
}

bool FTurnarounds::IsBeingServed(const UJobBoard& Board, const FTurnaround& Turnaround) const
{
	// STILL BEING SERVED, so the deadline does not apply - see DepartTheReady. Open and Queued count: the
	// airport may be about to gain the depot the job waits for, and a queued vehicle is coming.
	return Turnaround.JobIds.ContainsByPredicate([&Board](int32 JobId)
		{
			const FServiceJob* Job = Board.FindJob(JobId);
			return Job != nullptr && Job->State != EServiceJobState::Done && Job->State != EServiceJobState::Unserviceable;
		});
}

double FTurnarounds::NextDeadline(double Now) const
{
	double Next = TNumericLimits<double>::Max();
	for (const FTurnaround& Turnaround : Items)
	{
		if (Turnaround.TurnaroundEndsAt > Now)
		{
			Next = FMath::Min(Next, Turnaround.TurnaroundEndsAt);
		}
	}
	return Next;
}

bool FTurnarounds::HasRefusedDeparture(const UJobBoard& Board, double Now) const
{
	return Items.ContainsByPredicate([this, &Board, Now](const FTurnaround& Turnaround)
		{
			return Now >= Turnaround.TurnaroundEndsAt && !IsBeingServed(Board, Turnaround)
				&& Turnaround.LastDepartureRefusal != EDepartureRefusal::None;
		});
}

void FTurnarounds::DepartTheReady(UJobBoard& Board, UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	// THE LAST PASS'S ANSWER GOES FIRST, where the list is written: it is this pass's answer alone. It was cleared on
	// Step's first line, which reached this pass with nothing between that wrote or read the list.
	// ENFORCED BY: AirportOps.Model.TurnaroundDeparts (the Step that sends it names it, the next names nobody)
	LastStepDeparted.Reset();

	// GATHERED FIRST, DEPARTED AFTER. UGroundTraffic::DepartAgent broadcasts the phase change
	// synchronously, and OnAgentPhase drops the aircraft's turnaround when it hears it - which since the
	// ops bus is a drain later (UOpsRuntime::OnAgentPhase only publishes), not inside this loop. Kept
	// anyway, and it costs one small array: a synchronous listener is legal (UGroundTraffic's re-entrancy
	// contract), and a loop over Items that one could mutate is a crash waiting on a wiring change.
	// Ids, not pointers, for the same reason.
	TArray<int32> Ready;
	for (const FTurnaround& Turnaround : Items)
	{
		if (Clock.Now() < Turnaround.TurnaroundEndsAt)
		{
			continue;
		}
		// STILL BEING SERVED, so the deadline does not apply. A vehicle that is on its way or at the
		// hydrant is finishing a job the aircraft asked for, and cutting it off would strand the vehicle
		// at a stand nobody is at. Open and Queued are here too: the airport may be about to gain the
		// depot the job is waiting for, and a queued vehicle is coming.
		if (!IsBeingServed(Board, Turnaround))
		{
			Ready.Add(Turnaround.AircraftId);
		}
	}

	for (const int32 AircraftId : Ready)
	{
		const FTurnaround* Turnaround = For(AircraftId);
		if (Turnaround == nullptr)
		{
			continue;
		}
		// READ BEFORE THE DEPARTURE, which drops the turnaround and its jobs.
		const FServiceJob* Fuel = Board.JobForAircraft(AircraftId, EServiceRole::Fuel);
		const bool bUnfuelled = Fuel != nullptr && Fuel->State == EServiceJobState::Unserviceable;
		const EServiceRefusal Why = Fuel != nullptr ? Fuel->Why : EServiceRefusal::None;
		const double Delivered = Fuel != nullptr ? Fuel->QuantityDelivered : 0.0;
		const double Wanted = Fuel != nullptr ? Fuel->QuantityDelivered + Fuel->QuantityOwed : 0.0;
		const int32 Stand = Turnaround->Stand.Index;

		// NOTHING IS PUBLISHED OR PAID HERE (batch 3 review I1): a departure DepartAgent accepts changes the
		// aircraft's phase, OnAgentPhase drops the turnaround, and Drop - which the inspector's manual Depart
		// reaches too - calls EndTurnaround, the one publisher of FTurnaroundEndedEvent and poster of the
		// part-fuelled fee (its other caller is OnAircraftPhase, for a departure never turned around). A refusal
		// changes no phase, so it ends nothing, however often it is retried.
		// ENFORCED BY: AirportOps.Fuel.RefusedDepartureEndsNoTurnaround
		const EDepartureRefusal Refusal = Traffic.DepartAgent(AircraftId, Network);
		if (Refusal != EDepartureRefusal::None)
		{
			// LOGGED ON A CHANGE OF REASON, not every retry. A taxiway the player has left busy, or a
			// stand with no arm to push onto, refuses this for as long as they leave it, and the safety
			// net retries every 30 s besides the events (UJobBoard::Step's header). Re-found because DepartAgent
			// may have moved the array.
			if (FTurnaround* Still = Find(AircraftId); Still != nullptr && Still->LastDepartureRefusal != Refusal)
			{
				Still->LastDepartureRefusal = Refusal;
				UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d is ready to leave stand %d but cannot: %s"),
					AircraftId, Stand, *UEnum::GetValueAsString(Refusal));
			}
			continue;
		}

		// SAID WHEN IT LEAVES WITHOUT FUEL. The 'cannot be served' warning fired when the job went
		// Unserviceable and named what was missing; this says the airport lost the turnaround rather
		// than the stand, which is the consequence the player sees.
		//
		// PART-FUELLED is not UNFUELLED (review, 2026-09-28): a job that became impossible after a trip
		// or two - a depot deleted, a road cut - leaves with what it got, and PAYS for it. Fuel sold is
		// fuel paid for - PAID IN Drop since batch 3, when the phase change DepartAgent announced
		// reaches OnAgentPhase; this line only says so in the log.
		// ENFORCED BY: AirportOps.Fuel.PartFuelledPaysForWhatItGot
		LastStepDeparted.Add(AircraftId);
		const bool bPartFuelled = bUnfuelled && Delivered > 0.0;
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: aircraft %d departs stand %d%s"), AircraftId, Stand,
			bPartFuelled
				? *FString::Printf(TEXT(" PART-FUELLED %.0f of %.0f L - %s"), Delivered, Wanted, ServiceText::RefusalText(Why))
				: bUnfuelled
					? *FString::Printf(TEXT(" UNFUELLED - %s"), ServiceText::RefusalText(Why))
					: TEXT(" after its turnaround"));

	}
}
