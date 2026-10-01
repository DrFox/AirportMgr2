#pragma once

#include "CoreMinimal.h"
#include "Model/Flight.h"
#include "Model/FlightBilling.h"
#include "Model/FlightBoard.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"

/**
 * A transition as UGroundTraffic would announce it, for a test that hands one to a board or a delegate by hand (#436).
 *
 * THE CAUSE IS THE CALLER'S TO NAME, never guessed from the pair: the boards map Cause, and a helper that derived one
 * from (From, To) would be the pair-mapping #436 took out of production, moved into the tests where nothing checks it.
 * GoalAtEvent defaults unset - no stand - which is what every hand-made Parked before #436 meant, since those agents
 * were not in any traffic model and so parked at no stand.
 *
 * INLINE, in a header: the test module is a unity build, and one definition per .cpp would collide.
 */
inline FAgentTransition OpsTestTransition(int32 AgentId, EAgentPhase From, EAgentPhase To, EAgentEvent Cause,
	FGuidelineNodeId GoalAtEvent = FGuidelineNodeId())
{
	FAgentTransition Made;
	Made.AgentId = AgentId;
	Made.From = From;
	Made.To = To;
	Made.Cause = Cause;
	Made.GoalAtEvent = GoalAtEvent;
	return Made;
}

/**
 * The stand at the agent's goal NOW - a test asking about a live agent. Kept out of production on purpose (#436): the
 * boards ask of the EVENT's node (StandAtNode(GoalAtEvent)), and a live-goal read in ops is the stale read that issue
 * removed.
 */
inline FEntityInstanceId StandAtGoalForTest(const URoadNetwork& Network, const FRoadAgent& Agent)
{
	return StandAtNode(Network, Agent.GoalNode);
}

/**
 * THE BILLING REACTION, subscribed on a test's own bus as UOpsRuntime::WireBus subscribes it ("Billing", Sim,
 * FlightBilling::OnFlightPhaseChanged into Ledger, against Board - the ledger handed in, as the runtime hands its own since
 * #506's review; null bills nothing but still starts the parking clock) - for a board test that drives a flight's phase by hand and reads the
 * ledger or the parking clock (#442 item 4: billing is a reaction to the phase now, so a board whose phase changes reach no
 * bus posts nothing). Call between the bus's BeginWiring and EndWiring; the board must point at the bus to publish. THE
 * RULE'S HARNESS, NOT ITS WIRING: the runtime's own subscription is pinned by AirportOps.Present.Bus.BillingIsWired.
 */
inline void OpsTestSubscribeBilling(FOpsEventBus& Bus, UFlightBoard& Board, ULedger* Ledger)
{
	Bus.Subscribe<FFlightPhaseChangedEvent>(EOpsTier::Sim, TEXT("Billing"),
		[&Board, Ledger](const FFlightPhaseChangedEvent& E) { FlightBilling::OnFlightPhaseChanged(Ledger, Board, E); });
}

/**
 * A bare bus whose one subscriber is the billing reaction, with Board pointed at it - the fee tests' harness. The board
 * lets go of it when this goes, so nothing publishes into a bus that has left the stack.
 */
struct FOpsTestBilling
{
	FOpsEventBus Bus;
	UFlightBoard* Board = nullptr;

	FOpsTestBilling(UFlightBoard& InBoard, ULedger* Ledger)
		: Board(&InBoard)
	{
		Bus.BeginWiring();
		OpsTestSubscribeBilling(Bus, InBoard, Ledger);
		Bus.EndWiring();
		InBoard.Bus = &Bus;
	}
	~FOpsTestBilling()
	{
		if (Board != nullptr && Board->Bus == &Bus)
		{
			Board->Bus = nullptr;
		}
	}
	FOpsTestBilling(const FOpsTestBilling&) = delete;
	FOpsTestBilling& operator=(const FOpsTestBilling&) = delete;
};
