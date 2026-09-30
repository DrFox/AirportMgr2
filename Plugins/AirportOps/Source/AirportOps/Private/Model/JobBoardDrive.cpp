#include "Model/JobBoard.h"

#include "AirportOpsLog.h"
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

bool UJobBoard::MayDriveUngated(const FRoutePlan& Plan, const FVehicle& Vehicle, const URoadNetwork& Network,
	FString* OutWhy, const FTowSeed* Seed)
{
	if (!Vehicle.HasTrailer())
	{
		return true;
	}
	// A REVERSE IT CANNOT BACK is refused with the fold (2026-09-27): the tow would stall at the
	// service point holding the node, which is the jack-knife's cost under another name.
	const FFitVerdict Whole = VehicleFit::JudgePlan(Plan, Vehicle, Network, Seed);
	if (Whole.Refusal != EFitRefusal::TrailerFolds && Whole.Refusal != EFitRefusal::ReverseUnsolvable)
	{
		return true;
	}
	if (OutWhy != nullptr)
	{
		*OutWhy = Whole.Describe();
	}
	return false;
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
	// is home, whatever the table says now.
	const FVehicle OwnVehicle = Truck->AsVehicle() != nullptr ? *Truck->AsVehicle() : Type.Vehicle;
	const TCHAR* Where = bToFacility ? TEXT("home") : TEXT("its next job");

	// THE QUERY FOR THE NEXT LEG, from Start: one statement for the two places a vehicle turns - the
	// service point it is parked at, and a node ahead of it on the road.
	auto LegQuery = [&Traffic, TruckId, &OwnVehicle](FGuidelineNodeId Start, FGuidelineNodeId LegGoal)
	{
		// THROUGH FRouteQuery::For, the one writer of AvoidRunways off the resolved policy (#312).
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::VehicleToJob, Start, LegGoal, 0.0, ETraversalClass::GroundVehicle);

		// A DRIVE, NOT A COMPARISON, so unlike the bid's route this one takes the table: the truck is
		// committed to the leg and should go round the queue that is there rather than into the back of
		// it. The flicker argument covers a WINNER being re-picked every tick, which a committed route
		// is not.
		Query.WithCongestion(Traffic.GetOccupancy(), TruckId, Traffic.Rules.CongestionWeight);
		Query.WithVehicle(OwnVehicle);
		Query.RunwayPenalty = Traffic.Rules.RunwayPenalty;
		return Query;
	};

	// A VEHICLE STILL ON THE ROAD (final review, 2026-09-27): its aircraft left, or was deleted, before
	// it got there - or it is being sent on to a different job. The search used to start from its GOAL
	// regardless - the service point, where the route away opens with the bay's reverse leg - and
	// RedirectAgent then started the truck on that route's first point: a rigid truck jumped to the
	// hydrant, and the tow's reverse was solved from a cab still out on the road (ReverseUnsolvable,
	// "retired where it stands").
	//
	// SO IT TURNS WHERE IT IS: the route is searched from the node at the END of the step it is driving
	// - ReplanAt's splice point, ahead of it - and spliced on through RerouteAgent, which keeps it
	// moving and judges a tow's whole new route from the live chain. The tail is searched UNSEEDED, as
	// FPlanReResolver::SpliceReplan's is: it starts where the chain is not yet. When that turn does not
	// hold - a tow arriving at a junction cannot always take a hard turn back (measured 2026-09-27: a 90
	// degree fold at the far road's junction) - the next node on is tried, and so on up the route: each
	// is one search, once per recall, at most one per step left - 15 steps depot to hydrant on the fuel
	// fixture's A and C stands (measured 2026-09-27, AirportOps.Fuel.*RecalledMidRouteGetsHome's
	// "recalled on step" line); the tow took 5.
	//
	// ON ITS LAST STEP, OR WHEN NO TURN HOLDS, IT FINISHES THE LEG and turns from the service point,
	// the normal cycle's own path (OnVehicleArrived, on its arrival): the last step ends AT the service
	// point, so a turn from its end is the route onward from there anyway, and only the chain that
	// arrives can solve its reverse. Better the rest of the way out than a retirement.
	// ENFORCED BY: AirportOps.Fuel.TowRecalledMidRouteGetsHome,
	// AirportOps.Fuel.TruckRecalledMidRouteGetsHome, AirportOps.Fuel.TowRecalledOnItsLastLegGetsHome
	if (Truck->Phase == EAgentPhase::Taxiing && Truck->Follower.Plan.IsValid())
	{
		// COPIED, because a successful RerouteAgent replaces the plan this would otherwise alias.
		const FRoutePlan Out = Truck->Follower.Plan;
		const FGuidelineNodeId OutGoal = Truck->GoalNode;
		if (OutGoal == Goal)
		{
			// ALREADY GOING THERE (a job re-bid back onto the leg it is on): nothing to turn.
			return true;
		}
		const int32 FirstKeep = UGroundTraffic::CurrentStep(Out, Truck->Follower.Travelled) + 1;
		bool bSaidNarrow = false;
		for (int32 KeepSteps = FirstKeep; KeepSteps < Out.Steps.Num(); ++KeepSteps)
		{
			const FGuidelineNodeId TurnAt = UGroundTraffic::StepFromNode(Out, KeepSteps);
			if (TurnAt == OutGoal)
			{
				break;
			}
			FRouteQuery Query = LegQuery(TurnAt, Goal);
			FRoutePlan Tail = RouteSearch::Find(Network, Query);
			// HOME EVEN IF IT DOES NOT FIT, for the parked path's reason below, and SAID the same way -
			// once per recall, not once per node tried; RerouteAgent's own whole-route judge is what
			// keeps a fold off the road. ONLY HOME: a job leg the vehicle does not fit is not one to
			// drive anyway - the job goes back to the board, where the bid will choose a vehicle that
			// does, or say which road is too narrow.
			if (Tail.Result == ERouteResult::TooNarrow && bToFacility)
			{
				if (!bSaidNarrow)
				{
					bSaidNarrow = true;
					UE_LOG(LogAirportOps, Warning,
						TEXT("Fuel: truck %d does not fit the road home to depot %d (%s, guideline edge %d); driving it anyway"),
						TruckId, Vehicle.Home.Index, *Tail.RejectedBy.Describe(), Tail.RejectedEdge.Index);
				}
				Query.Vehicle = nullptr;
				Tail = RouteSearch::Find(Network, Query);
			}
			if (Tail.IsValid() && Traffic.RerouteAgent(TruckId, &Network, KeepSteps, Tail))
			{
				UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d turns for %s at guideline node %d, on the road"),
					TruckId, Where, TurnAt.Index);
				return true;
			}
		}
		// STILL ITS LEG: it arrives at the old goal, and OnVehicleArrived sees a goal that is not the one
		// its state is heading for and sends it on from there, parked.
		UE_LOG(LogAirportOps, Log,
			TEXT("Fuel: truck %d finishes its leg to the service point and turns for %s there"), TruckId, Where);
		return true;
	}

	// REVERSING (or any other motion that is not the road and not parked): RedirectAgent refuses it, so
	// the parked path below would retire the vehicle where it stood (final review #3, 2026-09-28; the
	// old SendTruckHome had the same Taxiing-only test). It FINISHES THE LEG - backing off one stand at
	// the start of a chained leg, into a bay at the end of one - and OnVehicleArrived sends it on from
	// the service point it parks at, exactly as the last-leg recall above does.
	// ENFORCED BY: AirportOps.Fuel.TowRecalledWhileReversingGetsHome
	//
	// BUT NOT STRANDED (2026-09-29): a stranded vehicle has no leg to finish and would "turn where it
	// parks" never. It is rescued onto pavement toward Goal (UGroundTraffic::RescueStranded), and when
	// there is none, a job leg is refused and the way home falls to the parked branch's own fallback,
	// retired where it stands.
	// ENFORCED BY: AirportOps.Model.AgentRescue.StrandedVehicleReleasesJobs
	if (Truck->Phase == EAgentPhase::Stranded)
	{
		if (Traffic.RescueStranded(TruckId, Network, Goal))
		{
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: stranded truck %d rescued, heading for %s"), TruckId, Where);
			return true;
		}
		if (!bToFacility)
		{
			return false;
		}
		UE_LOG(LogAirportOps, Warning,
			TEXT("Fuel: stranded truck %d has no pavement to rejoin toward depot %d; retired where it stands, vehicle %d back at its depot"),
			TruckId, Vehicle.Home.Index, Vehicle.Id);
		RetireAgentOf(Vehicle, Traffic);
		return false;
	}
	if (Truck->Phase != EAgentPhase::Parked)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d finishes its reverse and turns for %s where it parks"), TruckId, Where);
		return true;
	}

	FRouteQuery Query = LegQuery(Truck->GoalNode, Goal);

	// JUDGED FROM THE LIVE CHAIN AND CAB (2026-09-27): the route away from a stand OPENS with the bay's
	// reverse leg, and VehicleFit::JudgePlan solves that reverse from where the tow is parked - its
	// axles, its heading and its cab (FTowSeed::Origin), the pose FTowReverseRun will arm from - so a
	// route the router admits is one the tow can back along. Unseeded, JudgePlan refuses a
	// reverse-first plan outright. THE SAME FOR A JOB LEG as for the way home: a vehicle chained stand
	// to stand backs off the first stand exactly as it would to go home.
	// ENFORCED BY: AirportOps.Fuel.TowServesCodeB (the tow gets home)
	if (OwnVehicle.HasTrailer() && Truck->TowAxles.Num() == OwnVehicle.Tow.Num())
	{
		const FVector2D Steered = Truck->LastMotion.Position
			+ FVector2D(FMath::Cos(Truck->LastMotion.Heading), FMath::Sin(Truck->LastMotion.Heading)) * OwnVehicle.Chassis.SteerAxleX;
		const FGuidelineNode* StartNode = Network.GetGuidelineNode(Query.Start);
		FTowSeed& Seed = Query.TowSeed.Emplace();
		Seed.Axles = Truck->TowAxles;
		Seed.Heading = Truck->LastMotion.Heading;
		Seed.Speed = 0.0;
		// HOW FAR ALONG THE ROUTE THE STEERED AXLE ALREADY IS - zero at a service point, where it
		// parked on the node the route starts from; RigYard's reading of the same thing.
		Seed.Travelled = StartNode != nullptr ? FVector2D::Distance(StartNode->Position, Steered) : 0.0;
		Seed.Origin = Truck->LastMotion.Position;
	}
	FRoutePlan Plan = RouteSearch::Find(Network, Query);

	// HOME EVEN IF IT DOES NOT FIT (review of 2026-09-24). The way out was chosen for a road this truck
	// fits; the way back can meet a tighter corner - the near-side turn is the tight one - and retiring
	// the truck at the stand costs the airport a truck on every such job. So it drives home ungated,
	// and the log names the edge, which is the player's cue to widen that road.
	// ENFORCED BY: AirportOps.Ops.FuelTruckGetsHomeWhenTooNarrow
	//
	// BUT NEVER A ROUTE THAT FOLDS ITS TRAILER (review of 9441ccf1). A body a little wide for a corner
	// scuffs a kerb; a trailer past square is a jack-knife, and the agent stops dead where it folds,
	// holding the road. So a TOW's ungated route is judged whole (VehicleFit::JudgePlan, the router's
	// own check) and, if it folds, not driven: the truck is retired below, with the fold named.
	// ENFORCED BY: AirportOps.Ops.FuelTowNeverDrivenHomeIntoAFold
	//
	// ONLY HOME, as on the road above: a job leg it does not fit goes back to the board.
	if (Plan.Result == ERouteResult::TooNarrow && bToFacility)
	{
		UE_LOG(LogAirportOps, Warning,
			TEXT("Fuel: truck %d does not fit the road home to depot %d (%s, guideline edge %d); driving it anyway"),
			TruckId, Vehicle.Home.Index, *Plan.RejectedBy.Describe(), Plan.RejectedEdge.Index);
		Query.Vehicle = nullptr;
		Plan = RouteSearch::Find(Network, Query);
		FString Why;
		if (Plan.IsValid() && !MayDriveUngated(Plan, OwnVehicle, Network, &Why, Query.TowSeed.GetPtrOrNull()))
		{
			UE_LOG(LogAirportOps, Warning,
				TEXT("Fuel: truck %d's ungated road home to depot %d does not hold its tow either (%s); not driven"),
				TruckId, Vehicle.Home.Index, *Why);
			Plan = FRoutePlan();
		}
	}

	// REDIRECT, NOT DISPATCH: the truck keeps its id and its view, and RedirectAgent accepts a Parked
	// agent - which is exactly the handover its own header describes this service composing.
	if (Plan.IsValid() && Traffic.RedirectAgent(TruckId, &Network, Plan))
	{
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d heading for %s (guideline node %d)"), TruckId, Where, Goal.Index);
		return true;
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
}
