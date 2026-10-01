#pragma once

#include "CoreMinimal.h"
#include "Model/RoadHandles.h"
#include "Model/ServiceJob.h"
#include "Model/ServiceVehicle.h"

class UJobBoard;
class URoadNetwork;

/**
 * How far behind one depot is: the depot card's two texts and the counts behind them. See
 * ServiceText::DescribeDepot (UJobBoard::DescribeDepot forwards to it).
 */
struct FDepotBacklog
{
	/** "3 jobs · clears in 38 min · 1 late", "No jobs", or - a depot with no vehicle at all - "No vehicles — buy one". */
	FString Summary;

	/** One line per vehicle, then its jobs in the order it will do them. Empty with no vehicles. */
	FString Detail;

	int32 Jobs = 0;
	int32 LateJobs = 0;

	/** The latest promised finish among its jobs, USimClock game seconds; 0 with none. */
	double ClearsAt = 0.0;
};

/**
 * EVERY VEHICLE, JOB AND DEPOT TEXT THE JOB BOARD SHOWS (issue #427, the fifth of its extractions): the refusal the player
 * is told, the aircraft card's fuel line, the vehicle card, the depot card's backlog and fleet rows, and the vehicle state's
 * log name. They were UJobBoard members, so every next role's wording (baggage, stairs) would have entered the scheduler's
 * class beside its bidding. A NAMESPACE OF FUNCTIONS OVER A const UJobBoard&, not a class: text has no state of its own,
 * and it reads the board through its public reads alone (its jobs, vehicles and turnarounds), so it can never change it.
 * UJobBoard keeps every name it had - RefusalText, DescribeAgent, DescribeDepot, VehicleLine - as a forwarder here.
 * ENFORCED BY: AirportOps.Model.ServiceText.BoardForwardsEveryLine (each forwarder against what it forwards to)
 *
 * WHAT STAYS OUT: the stand's NUMBER (OpsNames::StandLabel) and the clock's words (GameTimeText), each one file's already
 * (#447), and the "Fuel:" log lines, which travel with the code that logs them.
 *
 * NETWORK NAMES THE STANDS (#447): a vehicle's line says "to stand 4" by the number painted at the stand, which only the
 * network's entity holds. REQUIRED, NO DEFAULT, wherever a stand is named: it was optional at first, and the depot card's
 * fleet rows left it off and said "to stand 0" beside the backlog's "to stand 4" (#447 review). A world-free test of the
 * board passes nullptr, which names them by index.
 */
namespace ServiceText
{
	/** A vehicle state's name, for the "Vehicle 3 FUEL: Idle -> ToJob" log lines. "?" for a value outside the enum. */
	AIRPORTOPS_API const TCHAR* StateName(EServiceVehicleState State);

	/** What the player is told for Why - on the aircraft card and in the log. The wording IS the
	 *  contract: tests assert it, because it is what sends the player to the right thing to fix. */
	AIRPORTOPS_API const TCHAR* RefusalText(EServiceRefusal Why);

	/** "to stand 3", "at depot 1" - the one phrase the vehicle card and the depot card share, so the
	 *  two cannot describe the same vehicle two ways. The stand and the depot are named by their numbers through Network (OpsNames::StandLabel, OpsNames::DepotLabel, #490);
	 *  null, for a graph-free test, names them by index. */
	AIRPORTOPS_API FString VehicleDoing(const UJobBoard& Board, const FServiceVehicle& Vehicle, const URoadNetwork* Network);

	/** "Bowser #7 · at depot 1 · 10,000 L" - the one line the depot card's backlog and its fleet rows share. The kind's
	 *  NAME, through FServiceFleet::NameOf (#430): the card listed "FUEL #7" beside the "Bowser" it sold. Its stand is named by number through
	 *  Network (OpsNames::StandLabel), which BOTH callers must pass - null (a graph-free test) names it by index. */
	AIRPORTOPS_API FString VehicleLine(const UJobBoard& Board, const FServiceVehicle& Vehicle, const URoadNetwork* Network);

	/** The vehicle card's line - its kind's NAME (FServiceFleet::NameOf, #430), what it is doing, what it carries, what
	 *  is queued. */
	AIRPORTOPS_API FString DescribeVehicle(const UJobBoard& Board, const FServiceVehicle& Vehicle, const URoadNetwork* Network);

	/**
	 * The one line the inspector's card shows for this agent, or empty when the board has nothing to
	 * say about it: an aircraft's fuel line, or - for a service vehicle's agent - the vehicle's own
	 * line (DescribeVehicle). ONE SEAM FOR BOTH, because the inspector already asks it for whatever
	 * agent is selected.
	 *
	 * A STRING BUILT HERE rather than an enum the panel switches on: it is presentation of two
	 * orthogonal model facts (the state, and for Unserviceable the reason), and nothing branches on
	 * it - the same argument InspectFacts::StatusOf makes for its own line.
	 *
	 * AND WHETHER ITS ANSWER MOVES WITH THE CLOCK (bOutMovesWithClock): true only while the fuel line counts litres down
	 * through a trip being pumped (its "LIVE WHILE PUMPING" rule). Anything else it says changes only with the
	 * board, so a caller may keep it until UJobBoard::Revision moves - the inspector does (ops batch 3 PR E).
	 * ENFORCED BY: AirportOps.Fuel.LineSaysWhenItMovesWithTheClock (live only while serving);
	 * AirportMgr.Inspector.Cache.FuelLineLiveWhilePumping (the card follows it while live)
	 */
	AIRPORTOPS_API FString DescribeAgent(const UJobBoard& Board, int32 AgentId, double Now, bool& bOutMovesWithClock,
		const URoadNetwork* Network);

	/**
	 * How far behind Depot is (user, 2026-09-28: "see how far behind your depot is in jobs"): every job
	 * on its vehicles - under way, being served, or queued - when the last of them is promised to
	 * finish, and how many of those promises land after their aircraft's turnaround ends, which is the
	 * backlog actually costing the airport. Read off each job's PromisedFinish, which the re-bid pass
	 * refreshes on every trigger (RebidQueued), so the card needs no bookkeeping of its own. Minutes are
	 * whole - but rounded from Now, so they move at half-minute offsets of it, not at the game minute. The
	 * inspector therefore passes the START of the game minute as Now (ops batch 3 PR E), which makes its
	 * card a function of the minute it keys on, and redraws it at most once a game minute.
	 *
	 * THE STATUS IS THE BOARD'S (#447): a depot with no vehicle says "No vehicles - buy one" in Summary, where the card laid that over
	 * the summary in its own wording beside RefusalText's. Network names the stands as the card does (OpsNames::StandLabel); a test of the
	 * board with no graph passes nullptr and gets the index. Required, no default - see the namespace comment.
	 * ENFORCED BY: AirportOps.Fuel.Describe.DepotBacklog (the no-vehicle and no-job cases),
	 * AirportOps.Model.StandLabel.AlertBacklogAndCardSayTheSameNumber
	 */
	AIRPORTOPS_API FDepotBacklog DescribeDepot(const UJobBoard& Board, FEntityInstanceId Depot, double Now, const URoadNetwork* Network);
}
