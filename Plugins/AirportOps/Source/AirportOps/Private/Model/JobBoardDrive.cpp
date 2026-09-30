#include "Model/JobBoard.h"

#include "AirportOpsLog.h"
#include "Model/ExhaustiveSwitch.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"

namespace JobBoardDrive
{
	/**
	 * A truck's engine wind-down, in seconds. ZERO, and deliberately.
	 *
	 * ARoadNetworkActor::ShutdownPauseSeconds is an AIRCRAFT's post-arrival pause - the nine seconds
	 * a propeller takes to stop, which reads as a shutdown rather than a stall. A truck has nothing to
	 * wind down, and ten idle seconds at each end would be added to every trip for no visible reason.
	 */
	constexpr double TruckShutdownPause = 0.0;

	/**
	 * THE ROUTE ITSELF, not its length, as one log line. A truck that reaches the hydrant the long
	 * way round the lane and one that turns straight in are both "a valid plan" and both log
	 * identically without this - and the difference is the whole of what the player watches.
	 *
	 * CAPPED. A route the length of the airport is a hundred points, and a log line nobody can read is
	 * the same as no log line. The HEAD is the half that matters: the journey out of the lane is what
	 * this exists to show.
	 */
	FString DescribePath(const FRoutePlan& Plan)
	{
		constexpr int32 MostPoints = 40;
		FString Path;
		for (int32 At = 0; At < FMath::Min(Plan.Polyline.Num(), MostPoints); ++At)
		{
			Path += FString::Printf(TEXT("(%.0f,%.0f) "), Plan.Polyline[At].X, Plan.Polyline[At].Y);
		}
		if (Plan.Polyline.Num() > MostPoints)
		{
			Path += FString::Printf(TEXT("... +%d more"), Plan.Polyline.Num() - MostPoints);
		}
		return Path;
	}
}

void UJobBoard::GoToFacility(FServiceVehicle& Vehicle, UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock)
{
	if (Vehicle.AgentId == 0)
	{
		// ALREADY THERE - it never left, or it was retired where it stood and put back at home.
		BeginFacility(Vehicle, Network, Clock);
		return;
	}
	const FGuidelineNodeId Home = HomePose(Network, Vehicle);
	if (Home.IsSet() && DriveVehicleTo(Vehicle, Home, /*bToFacility=*/true, Traffic, Network))
	{
		Lifecycle(Vehicle).HeadHome();
		return;
	}
	// DriveVehicleTo retired it where it stood (and said so, and put it Idle at home) - OR WAS NEVER ASKED,
	// because the depot is gone (final review #2, 2026-09-28): then the agent is still on the road, and
	// unhooking it without retiring would leave it there for the session with nothing that knows it. Retired
	// here in that case.
	// Idle at home is the honest state: the depot keeps its vehicle, which is the leak UFuelService's
	// GoingHome existed to prevent.
	// ENFORCED BY: AirportOps.Fuel.DepotGoneBeforeRecallLeavesNoAgent
	if (Vehicle.AgentId != 0)
	{
		if (Traffic.FindAgent(Vehicle.AgentId) != nullptr)
		{
			UE_LOG(LogAirportOps, Warning, TEXT("Fuel: truck %d has no depot to go home to; retired where it stands"), Vehicle.AgentId);
		}
		RetireAgentOf(Vehicle, Traffic);
	}
}

// ENFORCED BY: AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN (C4062 as an error over this function: a send outcome added to
// ESendOutcome is a build error here until it says what it means for a vehicle)
AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN
bool UJobBoard::DriveVehicleTo(FServiceVehicle& Vehicle, FGuidelineNodeId Goal, bool bToFacility,
	UGroundTraffic& Traffic, const URoadNetwork& Network)
{
	const FServiceVehicleType Type = TypeFor(Vehicle.TypeCode);
	const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
	const int32 TruckId = Vehicle.AgentId;

	// NO AGENT: IT IS AT HOME, and leaves by a fresh dispatch from its depot's pose - the route the
	// bid judged it on (DepotRoute's cache), so the truck drives the very route the choice was made on.
	if (TruckId == 0)
	{
		if (bToFacility || Home == nullptr)
		{
			return false;
		}
		const FRoutePlan Plan = DepotRoute(Network, Home->PoseNode, Goal, Type.Vehicle);
		if (!Plan.IsValid())
		{
			return false;
		}

		// ShutdownPause 0 - see TruckShutdownPause. A TOW IS DISPATCHED WHOLE by this same call:
		// FRoadAgent::StartDrive lays one axle per tow link, and the agent arms FTowReverseRun on a
		// reverse leg because its vehicle has a trailer - nothing here has to know which kind it sent.
		const int32 NewId = Traffic.DispatchAgent(&Network, Plan, Type.Vehicle, ETraversalClass::GroundVehicle,
			JobBoardDrive::TruckShutdownPause);
		if (NewId == 0)
		{
			// The model refused a plan the search called valid. Nothing about the AIRPORT is wrong, so
			// a rebuild is not what would fix it: the caller keeps the job queued and the next tick is a
			// free retry. SAID ONCE PER VEHICLE until a dispatch of it works (final review #5): the retry
			// runs every tick, and a line a tick buries every other line in the file.
			if (!DispatchRefusedWarned.Contains(Vehicle.Id))
			{
				DispatchRefusedWarned.Add(Vehicle.Id);
				UE_LOG(LogAirportOps, Warning,
					TEXT("Fuel: vehicle %d - depot %d had a route but the dispatch was refused; retrying each tick"),
					Vehicle.Id, Vehicle.Home.Index);
			}
			return false;
		}
		DispatchRefusedWarned.Remove(Vehicle.Id);

		// At LOG, not Verbose: a dispatch happens once per trip, not per tick, so this costs one line a
		// trip - and a line the player has to switch on is a line that is not there in the session
		// that needed it. AFTER THE DISPATCH, so a refused one does not print its route every retry.
		UE_LOG(LogAirportOps, Log,
			TEXT("Fuel route: vehicle %d, depot %d to guideline node %d, %.0f uu over %d point(s): %s"),
			Vehicle.Id, Vehicle.Home.Index, Goal.Index,
			GuidelineGeom::PolylineLength(Plan.Polyline), Plan.Polyline.Num(), *JobBoardDrive::DescribePath(Plan));
		// IDLE -> DECIDING: it has its agent, and the caller (StartNext) sets it off for the job it chose.
		Lifecycle(Vehicle).Dispatched(NewId);
		UE_LOG(LogAirportOps, Log,
			TEXT("Fuel: depot %d sends truck %d (vehicle %d, %s) to guideline node %d (%.0f uu)"),
			Vehicle.Home.Index, NewId, Vehicle.Id, *Vehicle.TypeCode.ToString(), Goal.Index, Plan.Length);
		return true;
	}

	const FRoadAgent* Truck = Traffic.FindAgent(TruckId);
	if (Truck == nullptr)
	{
		// GONE. It no longer counts as out anywhere: it is back at home, Idle, and the caller carries on from there.
		Lifecycle(Vehicle).LeaveRoad();
		return false;
	}

	// THE AGENT'S OWN VEHICLE, copied before anything below can move the agent array: the one that is
	// actually out. Not re-read from the catalogue - a truck already dispatched is that truck until it
	// is home, whatever the table says now. Its phase too, for the one line that names it.
	const FVehicle OwnVehicle = Truck->AsVehicle() != nullptr ? *Truck->AsVehicle() : Type.Vehicle;
	const EAgentPhase WasPhase = Truck->Phase;
	const TCHAR* Where = bToFacility ? TEXT("home") : TEXT("its next job");

	// THE QUERY FOR THE NEXT LEG - one statement for every place a vehicle turns (the service point it is
	// parked at, a node ahead of it on the road); Airside sets where each search starts, and a tow's seed.
	// THROUGH FRouteQuery::For, the one writer of AvoidRunways off the resolved policy (#312).
	FRouteQuery Leg = FRouteQuery::For(ERouteErrand::VehicleToJob, FGuidelineNodeId(), Goal, 0.0, ETraversalClass::GroundVehicle);

	// A DRIVE, NOT A COMPARISON, so unlike the bid's route this one takes the table: the truck is
	// committed to the leg and should go round the queue that is there rather than into the back of
	// it. The flicker argument covers a WINNER being re-picked every tick, which a committed route
	// is not.
	Leg.WithRules(Traffic.Rules, Traffic.GetOccupancy(), TruckId);
	Leg.WithVehicle(OwnVehicle);

	// HOW IT GETS THERE IS AIRSIDE'S (issue #429): UGroundTraffic::SendAgentTo turns it on the road at the first node
	// ahead that holds, lets it finish a leg or a reverse, redirects it from a stand, or rescues it if stranded - by
	// its phase, from its own route, which this file used to read to choose. WHAT IT IS FOR stays here, below.
	//
	// HOME EVEN IF IT DOES NOT FIT (review of 2026-09-24). The way out was chosen for a road this truck
	// fits; the way back can meet a tighter corner - the near-side turn is the tight one - and retiring
	// the truck at the stand costs the airport a truck on every such job. So it drives home ungated,
	// and the log names the edge, which is the player's cue to widen that road - once per recall, not
	// once per node tried. ONLY HOME: a job leg the vehicle does not fit is not one to drive anyway - the
	// job goes back to the board, where the bid will choose a vehicle that does, or say which road is too
	// narrow. That an ungated route is never one that FOLDS a tow is Airside's rule (VehicleFit::MayDriveUngated).
	// ENFORCED BY: AirportOps.Ops.FuelTruckGetsHomeWhenTooNarrow
	const FSendAgentResult Sent = Traffic.SendAgentTo(TruckId, Goal, Leg, Network,
		bToFacility ? ENarrowRoad::DriveAnyway : ENarrowRoad::Refuse);
	if (Sent.bNarrow)
	{
		UE_LOG(LogAirportOps, Warning,
			TEXT("Fuel: truck %d does not fit the road home to depot %d (%s, guideline edge %d); driving it anyway"),
			TruckId, Vehicle.Home.Index, *Sent.NarrowWhy, Sent.NarrowEdge.Index);
	}

	switch (Sent.Outcome)
	{
	case ESendOutcome::AlreadyGoing:
		// ALREADY GOING THERE (a job re-bid back onto the leg it is on): nothing to turn.
		return true;

	case ESendOutcome::Turned:
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d turns for %s at guideline node %d, on the road"),
			TruckId, Where, Sent.TurnAt.Index);
		return true;

	case ESendOutcome::FinishesLeg:
		// STILL ITS LEG: it arrives at the old goal, and OnVehicleArrived sees a goal that is not the one
		// its state is heading for and sends it on from there, parked.
		UE_LOG(LogAirportOps, Log,
			TEXT("Fuel: truck %d finishes its leg to the service point and turns for %s there"), TruckId, Where);
		return true;

	case ESendOutcome::FinishesMotion:
		// REVERSING: it FINISHES THE LEG - backing off one stand at the start of a chained leg, into a bay at the end of
		// one - and OnVehicleArrived sends it on from the service point it parks at, exactly as the last-leg recall does.
		// ENFORCED BY: AirportOps.Fuel.TowRecalledWhileReversingGetsHome
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d finishes its reverse and turns for %s where it parks"), TruckId, Where);
		return true;

	case ESendOutcome::Rescued:
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: stranded truck %d rescued, heading for %s"), TruckId, Where);
		return true;

	case ESendOutcome::Redirected:
		// REDIRECT, NOT DISPATCH: the truck keeps its id and its view, and RedirectAgent accepts a Parked
		// agent - which is exactly the handover its own header describes this service composing.
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d heading for %s (guideline node %d)"), TruckId, Where, Goal.Index);
		return true;

	case ESendOutcome::NoPavement:
		// STRANDED WITH NO PAVEMENT TO REJOIN (2026-09-29): a job leg is refused, and the way home falls to the parked
		// case's own fallback, retired where it stands.
		// ENFORCED BY: AirportOps.Model.AgentRescue.StrandedVehicleReleasesJobs
		if (!bToFacility)
		{
			return false;
		}
		UE_LOG(LogAirportOps, Warning,
			TEXT("Fuel: stranded truck %d has no pavement to rejoin toward depot %d; retired where it stands, vehicle %d back at its depot"),
			TruckId, Vehicle.Home.Index, Vehicle.Id);
		RetireAgentOf(Vehicle, Traffic);
		return false;

	case ESendOutcome::NoRoute:
		// BUT NEVER A ROUTE THAT FOLDS ITS TRAILER (review of 9441ccf1): Airside judged the ungated road home whole and
		// would not drive it - said here, with the depot the player knows it by.
		// ENFORCED BY: AirportOps.Ops.FuelTowNeverDrivenHomeIntoAFold
		if (!Sent.FoldWhy.IsEmpty())
		{
			UE_LOG(LogAirportOps, Warning,
				TEXT("Fuel: truck %d's ungated road home to depot %d does not hold its tow either (%s); not driven"),
				TruckId, Vehicle.Home.Index, *Sent.FoldWhy);
		}
		if (!bToFacility)
		{
			// NO WAY TO THAT JOB FROM HERE. The caller puts the job back on the board and sends the
			// vehicle home, which is the fallback that has one.
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d has no route from its stand to guideline node %d"),
				TruckId, Goal.Index);
			return false;
		}
		// NO WAY BACK. Retired where it stands, and said out loud: a truck parked at a hydrant for the rest
		// of the session holds that node against every later job, which is worse than one that vanishes
		// with a line explaining itself. THE VEHICLE IS NOT LOST: the caller puts it back Idle at home.
		UE_LOG(LogAirportOps, Warning,
			TEXT("Fuel: truck %d has no route home to depot %d; retired where it stands, vehicle %d back at its depot"),
			TruckId, Vehicle.Home.Index, Vehicle.Id);
		RetireAgentOf(Vehicle, Traffic);
		return false;

	case ESendOutcome::NotSendable:
		// ON A RUNWAY OR GONE - a phase a service vehicle is not in (it never lands or takes off, and a gone agent is
		// not found above). Refused and SAID rather than promised: true would leave the vehicle heading for a goal
		// nothing is taking it to.
		UE_LOG(LogAirportOps, Warning, TEXT("Fuel: truck %d is in no state to be sent anywhere (%s); not sent for %s"),
			TruckId, *UEnum::GetValueAsString(WasPhase), Where);
		return false;
	}
	return false;
}
AIRSIDE_EXHAUSTIVE_SWITCH_END
