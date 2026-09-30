#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AgentRescue.generated.h"

class UFlightBoard;
class UGroundTraffic;
class UJobBoard;
class URoadNetwork;
class USimClock;
struct FRoadAgent;

/** The inspector's Unstick menu, in escalating order. Spec 2026-09-29-unstick-agent. */
UENUM()
enum class EUnstickAction : uint8
{
	/** A new route to where it was going: from where it is held, or back onto pavement if stranded. */
	Replan,
	/** A vehicle drops its jobs and goes to its depot; an aircraft seeks a free stand. */
	SendHome,
	/** Gone. A vehicle is Idle at home, its jobs back on the board; an aircraft's flight is Cancelled. */
	Despawn
};

/** Whether an Unstick may run (CanUnstick) or did (Unstick), and when not, the sentence why. */
struct FUnstickVerdict
{
	bool bAllowed = false;
	FText Why;

	static FUnstickVerdict Yes() { return FUnstickVerdict{true, FText::GetEmpty()}; }
	static FUnstickVerdict No(const FText& Why) { return FUnstickVerdict{false, Why}; }
};

/**
 * The player's escape hatch for an agent that has got stuck - spec 2026-09-29-unstick-agent.
 *
 * IN AirportOps, NOT Airside, because two of the three actions are about what an agent is FOR: a
 * vehicle's jobs (UJobBoard) and an aircraft's flight (UFlightBoard). The movement underneath is all
 * UGroundTraffic's - ReplanAroundBlocker, ReplanFromNextNode, RescueStranded, ReofferStand, RescueToStand,
 * RetireAgent - and nothing here moves an agent itself, or reads its route to decide how (#429): each of
 * those answers with an outcome, and what is left here is the SENTENCE the player reads for each.
 * ENFORCED BY: Check-Architecture rule 53 (route-internals: no Follower.Plan, ReplanAt, GetBlockedStep in
 * AirportOps). A subobject of UOpsRuntime, which grows by forwarding: a pointer and a line.
 *
 * WHETHER AN AGENT LOOKS STUCK is not here either since #429: it is FRoadAgent::IsStuck, the one
 * definition, which the inspector asks at its own highlight threshold.
 *
 * TWO BODIES, THREE ACTIONS, ONE TABLE (the spec's) - so two private functions switch on the action,
 * not a policy class per body: IServiceRolePolicy earns its hierarchy by carrying per-role data, and
 * this would be six one-line classes carrying none.
 *
 * CanUnstick and Unstick share one decision (Decide) so the menu cannot offer an entry the action
 * then refuses for a reason the menu never showed.
 */
UCLASS()
class AIRPORTOPS_API UAgentRescue : public UObject
{
	GENERATED_BODY()

public:
	/** Set once by UOpsRuntime; a test sets its own. Either may be null (no vehicles, no flights). */
	UPROPERTY() TObjectPtr<UJobBoard> JobBoard = nullptr;
	UPROPERTY() TObjectPtr<UFlightBoard> FlightBoard = nullptr;

	/** Whether Action would run on AgentId now, and why not. Changes nothing. */
	FUnstickVerdict CanUnstick(const UGroundTraffic& Traffic, int32 AgentId, EUnstickAction Action) const;

	/**
	 * Runs Action on AgentId: bAllowed when it happened, Why when it did not. Logs one line either way -
	 * "Unstick: agent N (<body>, <phase>) <action> -> done|refused: <why>" - the evidence a repro reads.
	 */
	FUnstickVerdict Unstick(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		int32 AgentId, EUnstickAction Action);

private:
	/** The shared decision, by phase and body alone - see the class comment. Changes nothing. */
	FUnstickVerdict Decide(const FRoadAgent& Agent, EUnstickAction Action) const;

	FUnstickVerdict Replan(UGroundTraffic& Traffic, const URoadNetwork& Network, const FRoadAgent& Agent);
	FUnstickVerdict SendVehicleHome(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		int32 AgentId);
	FUnstickVerdict FindStand(UGroundTraffic& Traffic, const URoadNetwork& Network, const FRoadAgent& Agent);
	FUnstickVerdict Despawn(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
		const FRoadAgent& Agent);
};
