#include "Model/FuelService.h"

#include "AirportOpsLog.h"
#include "Content/AirsideSettings.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Solve/GuidelineGeom.h"

namespace
{
	/**
	 * A truck's engine wind-down, in seconds. ZERO, and deliberately.
	 *
	 * ARoadNetworkActor::ShutdownPauseSeconds is an AIRCRAFT's post-arrival pause - the nine
	 * seconds a propeller takes to stop, which reads as a shutdown rather than a stall. A
	 * truck has nothing to wind down, and ten idle seconds at each end would be added to
	 * every trip for no visible reason.
	 */
	constexpr double TruckShutdownPause = 0.0;

	/** The live entity a guideline node is the POSE of, or unset. */
	FEntityInstanceId EntityAtPose(const URoadNetwork& Network, FGuidelineNodeId Node)
	{
		const int32 Index = Network.FindEntityIndexByPoseNode(Node);
		return Index != INDEX_NONE ? Network.EntityIdAt(Index) : FEntityInstanceId();
	}

	const TCHAR* RefusalText(EFuelRefusal Why)
	{
		switch (Why)
		{
		case EFuelRefusal::NoDepot:       return TEXT("no fuel depot");
		case EFuelRefusal::NoRoad:        return TEXT("depot not on a road");
		case EFuelRefusal::StandUnjoined: return TEXT("stand not on a road");
		case EFuelRefusal::NoRoute:       return TEXT("no road from depot");
		default:                          return TEXT("unserviceable");
		}
	}
}

FFuelDemand* UFuelService::FindByAircraft(int32 AircraftId)
{
	return Demands.FindByPredicate(
		[AircraftId](const FFuelDemand& Demand) { return Demand.AircraftId == AircraftId; });
}

const FFuelDemand* UFuelService::FindByAircraft(int32 AircraftId) const
{
	return Demands.FindByPredicate(
		[AircraftId](const FFuelDemand& Demand) { return Demand.AircraftId == AircraftId; });
}

FFuelDemand* UFuelService::FindByTruck(int32 TruckId)
{
	return TruckId == 0 ? nullptr : Demands.FindByPredicate(
		[TruckId](const FFuelDemand& Demand) { return Demand.TruckId == TruckId; });
}

FGuidelineNodeId UFuelService::FuelAnchorOf(const URoadNetwork& Network, FEntityInstanceId Stand)
{
	// BY ROLE, THEN BY ID. Role is a category - a stand may one day have two hydrants - so
	// this answers "where can fuel be worked" and takes the first; never by array position,
	// which is the invariant FResolvedAnchor exists to remove.
	for (const FName AnchorId : Network.GetAnchorIdsForRole(Stand, EServiceRole::Fuel))
	{
		if (const FResolvedAnchor* Resolved = Network.FindResolvedAnchor(Stand, AnchorId))
		{
			return Resolved->Node;
		}
	}
	return FGuidelineNodeId();
}

int32 UFuelService::TrucksOutFor(FEntityInstanceId Depot) const
{
	int32 Out = 0;
	for (const FFuelDemand& Demand : Demands)
	{
		Out += (Demand.TruckId != 0 && Demand.Depot == Depot) ? 1 : 0;
	}
	for (const TPair<int32, FEntityInstanceId>& Home : GoingHome)
	{
		// STILL OUT. A truck driving back is not available: dispatching it again from here
		// would be the same vehicle in two places, and RedirectAgent would silently turn it
		// round mid-journey.
		Out += Home.Value == Depot ? 1 : 0;
	}
	return Out;
}

FEntityInstanceId UFuelService::ChooseDepot(const URoadNetwork& Network,
	FGuidelineNodeId StandFuel, FRoutePlan& OutPlan, EFuelRefusal& OutWhy) const
{
	// THE ORDER OF THESE TESTS IS THE SPEC'S, and it is the order of the player's hand: no
	// depot at all is a building to place, a depot off the road is a road to draw, and only
	// then is it worth talking about the stand or the graph.
	OutWhy = EFuelRefusal::NoDepot;

	FEntityInstanceId Best;
	double BestLength = TNumericLimits<double>::Max();
	bool bAnyDepot = false;
	bool bAnyJoinedDepot = false;

	/**
	 * A joined depot with a fleet, all of it already out.
	 *
	 * SEPARATE FROM EVERY OTHER REASON because it is the only one that resolves ITSELF. The
	 * chain below reports facts about the airport, which change only when the player edits
	 * it; a truck comes home on its own.
	 */
	bool bAnyBusyDepot = false;

	// JOINED, NOT MERELY RESOLVED - and since the service loop, not merely INCIDENT either.
	//
	// This tested StandFuel.IsSet() alone, which is a fact about PLACEMENT and not about the
	// airport: URoadNetwork::PlaceEntity creates a node for every anchor whether or not a
	// lead-in ever reaches it, so the test was true for every Code C stand ever placed and
	// StandUnjoined could not fire at all. Counting incident edges fixed that, and then
	// stopped working for the same shape of reason the moment stands grew SERVICE LANES: a
	// hydrant is ALWAYS spurred to its own lane, so the count is true for a stand in the
	// middle of a field. The question was never "does this node have a line on it" but "does
	// that line go anywhere", which is a walk - see URoadNetwork::IsServiceNodeConnected.
	//
	// Both wrong answers reported NoRoute - "no road from depot" - which sends the player to
	// look at the depot when the road they need is at the stand. Observed in PIE 2026-09-07.
	const bool bStandJoined = StandFuel.IsSet() && Network.IsServiceNodeConnected(StandFuel);

	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Instance = Entities[Index];

		// A DEPOT IS AN ENTITY WHOSE POSE IS A SERVICE VEHICLE'S. Read off the instance,
		// where placement captured it: this layer may not dereference a UEntityDefinition.
		if (!Instance.bAlive || Instance.PoseRole != EServiceRole::Fuel)
		{
			continue;
		}
		bAnyDepot = true;

		const FGuidelineNode* Pose = Network.GetGuidelineNode(Instance.PoseNode);
		if (Pose == nullptr || Pose->Incident.Num() == 0)
		{
			// Placed, but with no road within its lead-in reach. FAnchorLink has already
			// warned about it in the census; this is the same fact reaching the player's
			// aircraft card.
			continue;
		}
		bAnyJoinedDepot = true;

		const FEntityInstanceId DepotId = Network.EntityIdAt(Index);

		// NO FLEET AT ALL IS NOT "BUSY", and the two were one condition until 2026-09-08.
		// A depot with no trucks never frees up, so treating it as busy would make a demand
		// wait for ever in silence. It falls through to the chain below and is reported.
		if (Instance.Trucks <= 0)
		{
			continue;
		}

		if (TrucksOutFor(DepotId) >= Instance.Trucks)
		{
			// BUSY, NOT BROKEN. A demand whose only depot is out on another job is Needed and
			// will be offered again next tick, not Unserviceable - which is TERMINAL until
			// the guideline revision changes, and a truck driving home changes no guideline.
			//
			// This branch always said so and could not deliver it: the else-chain below
			// assigned a refusal unconditionally whenever nothing was chosen, so busy came
			// out as NoRoute - "no road from depot" - and the aircraft was never fuelled
			// while the player was sent to look at a road that was already there. Seen in
			// PIE 2026-09-08. The flag is what carries this branch's intent to the chain.
			bAnyBusyDepot = true;
			continue;
		}

		if (!bStandJoined)
		{
			// Nothing to route TO. Skipped here as well as reported below, so a stand with an
			// unjoined hydrant does not cost a search per depot per tick.
			continue;
		}

		FRouteQuery Query;
		Query.Start = Instance.PoseNode;
		Query.Goal = StandFuel;
		Query.Class = ETraversalClass::GroundVehicle;

		// 0 IS UNLIMITED, and a road guideline carries no span limit either, so neither side
		// of that comparison means anything for a van.
		Query.Wingspan = 0.0;

		// NEVER ALONG A STRIP. A truck crossing a runway at a junction is unaffected - a
		// crossing is a turn path and a node, and turn paths carry no DerivedFrom - but
		// taxiing DOWN one is not something a fuel job may plan.
		Query.AvoidRunways = ERunwayAvoidance::All;

		// NO OCCUPANCY WEIGHT, deliberately. Which depot is nearest is a fact about the
		// airport's SHAPE, not about who happens to be on the road this instant; a
		// congestion-weighted length would make the chosen depot flicker between ticks and
		// the log unreadable. Congestion is the arbiter's job once the truck is under way.

		const FRoutePlan Plan = RouteSearch::Find(Network, Query);
		if (!Plan.IsValid() || Plan.Length >= BestLength)
		{
			continue;
		}

		BestLength = Plan.Length;
		OutPlan = Plan;
		Best = DepotId;
	}

	if (Best.IsSet())
	{
		OutWhy = EFuelRefusal::None;
	}
	else if (!bAnyDepot)
	{
		OutWhy = EFuelRefusal::NoDepot;
	}
	else if (!bAnyJoinedDepot)
	{
		OutWhy = EFuelRefusal::NoRoad;
	}
	else if (!bStandJoined)
	{
		OutWhy = EFuelRefusal::StandUnjoined;
	}
	else if (bAnyBusyDepot)
	{
		// NOTHING IS WRONG - WAIT. None keeps the demand Needed and re-offered every tick,
		// which is the queue: see the Needed case in Tick.
		//
		// AFTER the three facts above, deliberately. Those are things the player can go and
		// fix and should be told about even while a truck happens to be out; being busy is
		// not. And DEFERRING a genuine NoRoute costs nothing: the moment the truck is home
		// the depot is idle, this branch stops firing, and the real reason is reported.
		OutWhy = EFuelRefusal::None;
	}
	else
	{
		OutWhy = EFuelRefusal::NoRoute;
	}
	return Best;
}

void UFuelService::SendTruckHome(UGroundTraffic& Traffic, const URoadNetwork& Network,
	int32 TruckId, FEntityInstanceId Depot)
{
	const FRoadAgent* Truck = Traffic.FindAgent(TruckId);
	const FEntityInstance* Home = Network.GetEntity(Depot);
	if (Truck == nullptr)
	{
		GoingHome.Remove(TruckId);
		return;
	}

	FRoutePlan Plan;
	if (Home != nullptr)
	{
		FRouteQuery Query;
		Query.Start = Truck->GoalNode;
		Query.Goal = Home->PoseNode;
		Query.Class = ETraversalClass::GroundVehicle;
		Query.AvoidRunways = ERunwayAvoidance::All;
		Plan = RouteSearch::Find(Network, Query);
	}

	// REDIRECT, NOT DISPATCH: the truck keeps its id and its view, and RedirectAgent accepts
	// a Parked agent - which is exactly the handover its own header describes this service
	// composing.
	if (Plan.IsValid() && Traffic.RedirectAgent(TruckId, &Network, Plan))
	{
		GoingHome.Add(TruckId, Depot);
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d heading home to depot %d"),
			TruckId, Depot.Index);
		return;
	}

	// NO WAY BACK. Retired where it stands, and said out loud: a truck parked at a hydrant
	// for the rest of the session holds that node against every later job, which is worse
	// than one that vanishes with a line explaining itself.
	UE_LOG(LogAirportOps, Warning,
		TEXT("Fuel: truck %d has no route home to depot %d; retired where it stands"),
		TruckId, Depot.Index);
	Traffic.RetireAgent(TruckId);
	GoingHome.Remove(TruckId);
}

void UFuelService::OnAgentPhase(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock, int32 AgentId, EAgentPhase From, EAgentPhase To)
{
	// AN AIRCRAFT LEAVING ITS STAND, first: it departs, or is retired, or is deleted under
	// the player's hand. Any open demand for it is dropped and any truck out for it turns
	// round - a truck left at a hydrant nobody is using would hold that node for ever, and
	// its depot would be a truck short for the rest of the session.
	if (From == EAgentPhase::Parked && To != EAgentPhase::Parked)
	{
		if (FFuelDemand* Demand = FindByAircraft(AgentId))
		{
			const int32 TruckId = Demand->TruckId;
			const FEntityInstanceId Depot = Demand->Depot;
			UE_LOG(LogAirportOps, Log,
				TEXT("Fuel: aircraft %d left stand %d; demand dropped (was %d), truck %d recalled"),
				AgentId, Demand->Stand.Index, static_cast<int32>(Demand->State), TruckId);

			Demands.RemoveAll(
				[AgentId](const FFuelDemand& Each) { return Each.AircraftId == AgentId; });

			if (TruckId != 0)
			{
				SendTruckHome(Traffic, Network, TruckId, Depot);
			}
			return;
		}
	}

	if (To != EAgentPhase::Parked)
	{
		return;
	}

	const FRoadAgent* Agent = Traffic.FindAgent(AgentId);
	if (Agent == nullptr)
	{
		return;
	}

	// A TRUCK THAT HAS REACHED ITS DEPOT. Retired here and nowhere else: it does not fly
	// away, so nothing in the traffic model would ever remove it (UGroundTraffic::RetireAgent
	// exists for exactly this). Retiring frees the depot's count, because TrucksOutFor counts
	// GoingHome.
	if (const FEntityInstanceId* Home = GoingHome.Find(AgentId))
	{
		const FEntityInstance* Depot = Network.GetEntity(*Home);
		if (Depot != nullptr && Agent->GoalNode == Depot->PoseNode)
		{
			UE_LOG(LogAirportOps, Log, TEXT("Fuel: truck %d home at depot %d; retired"),
				AgentId, Home->Index);
			GoingHome.Remove(AgentId);
			Traffic.RetireAgent(AgentId);
		}
		return;
	}

	// A TRUCK THAT HAS REACHED THE HYDRANT.
	if (FFuelDemand* Demand = FindByTruck(AgentId))
	{
		if (Demand->State == EFuelDemandState::TruckEnRoute
			&& Agent->GoalNode == FuelAnchorOf(Network, Demand->Stand))
		{
			Demand->State = EFuelDemandState::Fuelling;
			Demand->DwellEndsAt = Traffic.GetSimSeconds() + DwellSeconds;
			UE_LOG(LogAirportOps, Log,
				TEXT("Fuel: truck %d at stand %d for aircraft %d; fuelling for %.0f s"),
				AgentId, Demand->Stand.Index, Demand->AircraftId, DwellSeconds);
		}
		return;
	}

	// AN AIRCRAFT THAT HAS PARKED. Its goal must be a STAND's pose - an aircraft parked on a
	// taxiway junction (the stand-death fallback) is at no stand and demands nothing, which
	// falls out of this same lookup rather than needing a rule of its own.
	if (Agent->Class != ETraversalClass::Aircraft || FindByAircraft(AgentId) != nullptr)
	{
		return;
	}

	const FEntityInstanceId Stand = EntityAtPose(Network, Agent->GoalNode);
	const FEntityInstance* Instance = Network.GetEntity(Stand);
	if (Instance == nullptr || Instance->PoseRole != EServiceRole::Aircraft)
	{
		return;
	}

	FFuelDemand Demand;
	Demand.AircraftId = AgentId;
	Demand.Stand = Stand;
	Demand.State = EFuelDemandState::Needed;

	// THE CLOCK STARTS WHEN THE WHEELS STOP, not when the fuelling finishes. A turnaround is
	// the time on stand, and the services happen INSIDE it - which is what lets baggage and
	// catering be added later without lengthening anything.
	//
	// The figure rides on the AGENT, in its airframe bundle, because this class may not
	// include Entities/ and so cannot ask the aircraft's type - the same reason the pose role
	// is read off FEntityInstance above. See FAirframe::TurnaroundSeconds.
	Demand.TurnaroundEndsAt = Clock.Now() + Agent->Airframe.TurnaroundSeconds;
	Demands.Add(Demand);

	UE_LOG(LogAirportOps, Log,
		TEXT("Fuel: aircraft %d parked at stand %d; needs fuel, away in %.0f game s"),
		AgentId, Stand.Index, Agent->Airframe.TurnaroundSeconds);
}

void UFuelService::Tick(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock)
{
	const double Now = Traffic.GetSimSeconds();
	const uint32 Revision = Network.GetGuidelineRevision();

	for (FFuelDemand& Demand : Demands)
	{
		switch (Demand.State)
		{
		case EFuelDemandState::Unserviceable:
		{
			// THE PLAYER MAY HAVE DRAWN THE ROAD. Re-offered only when the graph has
			// actually changed, which the revision reports without walking it - otherwise a
			// demand nothing can serve is retried thirty times a second, logging as it goes.
			if (Revision != LastRefusedRevision)
			{
				Demand.State = EFuelDemandState::Needed;
				Demand.Why = EFuelRefusal::None;
				UE_LOG(LogAirportOps, Log,
					TEXT("Fuel: the airport changed; aircraft %d asks again"), Demand.AircraftId);
			}
			break;
		}

		case EFuelDemandState::Needed:
		{
			FRoutePlan Plan;
			EFuelRefusal Why = EFuelRefusal::None;
			const FEntityInstanceId Depot = ChooseDepot(Network,
				FuelAnchorOf(Network, Demand.Stand), Plan, Why);

			if (!Depot.IsSet())
			{
				if (Why == EFuelRefusal::None)
				{
					// Every depot is simply busy. Still Needed - it will be offered again
					// next tick - and not logged, because it happens every tick until one
					// frees and would drown the line that matters.
					break;
				}

				Demand.State = EFuelDemandState::Unserviceable;
				Demand.Why = Why;
				LastRefusedRevision = Revision;

				// THE COUNTS THAT DECIDED IT, in the line itself. A bare reason sent the player
				// to look at the wrong end of the airport once already (PIE 2026-09-07); these
				// three numbers say which end without a second repro.
				int32 Depots = 0, DepotsOnRoad = 0;
				for (const FEntityInstance& Each : Network.GetEntities())
				{
					if (!Each.bAlive || Each.PoseRole != EServiceRole::Fuel) { continue; }
					++Depots;
					const FGuidelineNode* Pose = Network.GetGuidelineNode(Each.PoseNode);
					DepotsOnRoad += (Pose != nullptr && Pose->Incident.Num() > 0) ? 1 : 0;
				}
				const FGuidelineNodeId Hydrant = FuelAnchorOf(Network, Demand.Stand);

				UE_LOG(LogAirportOps, Warning,
					TEXT("Fuel: aircraft %d at stand %d cannot be served: %s. %d depot(s), %d on a "
						 "road; the stand's hydrant %s. Check the 'Anchor links:' line."),
					Demand.AircraftId, Demand.Stand.Index, RefusalText(Why), Depots, DepotsOnRoad,
					!Hydrant.IsSet() ? TEXT("has no node at all")
						: Network.IsServiceNodeConnected(Hydrant) ? TEXT("is on a road")
						: TEXT("is NOT on a road"));
				break;
			}

			// THE ROUTE ITSELF, not its length. A truck that reaches the hydrant the long way
			// round the lane and one that turns straight in are both "a valid plan" and both
			// log identically without this - and the difference is the whole of what the
			// player watches.
			//
			// At LOG, not Verbose: a fuel dispatch happens once per turnaround, not per tick,
			// so this costs one line an aircraft - and a line the player has to switch on is
			// a line that is not there in the session that needed it.
			{
				// CAPPED. A route the length of the airport is a hundred points, and a log
				// line nobody can read is the same as no log line. The HEAD is the half that
				// matters: the journey out of the lane is what this exists to show.
				constexpr int32 MostPoints = 40;
				FString Path;
				for (int32 At = 0; At < FMath::Min(Plan.Polyline.Num(), MostPoints); ++At)
				{
					Path += FString::Printf(TEXT("(%.0f,%.0f) "),
						Plan.Polyline[At].X, Plan.Polyline[At].Y);
				}
				if (Plan.Polyline.Num() > MostPoints)
				{
					Path += FString::Printf(TEXT("... +%d more"), Plan.Polyline.Num() - MostPoints);
				}
				UE_LOG(LogAirportOps, Log,
					TEXT("Fuel route: aircraft %d, depot %d to stand %d, %.0f uu over %d point(s): %s"),
					Demand.AircraftId, Depot.Index, Demand.Stand.Index,
					GuidelineGeom::PolylineLength(Plan.Polyline), Plan.Polyline.Num(), *Path);
			}

			// ShutdownPause 0 - see TruckShutdownPause. The AIRFRAME is the vehicle default,
			// resolved in the one place a truck's figures live.
			const int32 TruckId = Traffic.DispatchAgent(&Network, Plan,
				UAirsideSettings::ResolveDefaultVehicle(), ETraversalClass::GroundVehicle,
				TruckShutdownPause);
			if (TruckId == 0)
			{
				// The model refused a plan the search called valid. Left Needed rather than
				// marked Unserviceable: nothing about the AIRPORT is wrong, so a rebuild is
				// not what would fix it, and the next tick is a free retry.
				UE_LOG(LogAirportOps, Warning,
					TEXT("Fuel: aircraft %d - depot %d had a route but the dispatch was refused"),
					Demand.AircraftId, Depot.Index);
				break;
			}

			Demand.State = EFuelDemandState::TruckEnRoute;
			Demand.TruckId = TruckId;
			Demand.Depot = Depot;
			UE_LOG(LogAirportOps, Log,
				TEXT("Fuel: depot %d sends truck %d to stand %d for aircraft %d (%.0f uu)"),
				Depot.Index, TruckId, Demand.Stand.Index, Demand.AircraftId, Plan.Length);
			break;
		}

		case EFuelDemandState::Fuelling:
		{
			if (Now < Demand.DwellEndsAt)
			{
				break;
			}

			Demand.State = EFuelDemandState::Done;
			UE_LOG(LogAirportOps, Log,
				TEXT("Fuel: aircraft %d fuelled at stand %d by truck %d"),
				Demand.AircraftId, Demand.Stand.Index, Demand.TruckId);

			// CLEARED BEFORE THE TRIP HOME, so the truck belongs to GoingHome and to nothing
			// else - see that member for why a home-bound truck cannot stay on a demand.
			const int32 TruckId = Demand.TruckId;
			Demand.TruckId = 0;
			if (TruckId != 0)
			{
				SendTruckHome(Traffic, Network, TruckId, Demand.Depot);
			}
			break;
		}

		default:
			break;
		}
	}

	DepartTheReady(Traffic, Network, Clock);
}

void UFuelService::DepartTheReady(UGroundTraffic& Traffic, const URoadNetwork& Network,
	const USimClock& Clock)
{
	// GATHERED FIRST, DEPARTED AFTER. UGroundTraffic::DepartAgent broadcasts the phase change
	// synchronously, OnAgentPhase is on the other end of that broadcast, and it drops the
	// aircraft's demand - so departing inside a loop over Demands would mutate the array
	// being walked. Ids, not pointers, for the same reason.
	TArray<int32> Ready;
	for (const FFuelDemand& Demand : Demands)
	{
		if (Clock.Now() < Demand.TurnaroundEndsAt)
		{
			continue;
		}

		// STILL BEING SERVED, so the deadline does not apply. A truck that is on its way or
		// at the hydrant is finishing a job the aircraft asked for, and cutting it off would
		// strand the truck at a stand nobody is at - see OnAgentPhase's recall, which exists
		// precisely because that is expensive. Needed is here too: the airport may be about
		// to gain the depot the demand is waiting for.
		if (Demand.State != EFuelDemandState::Done
			&& Demand.State != EFuelDemandState::Unserviceable)
		{
			continue;
		}
		Ready.Add(Demand.AircraftId);
	}

	for (const int32 AircraftId : Ready)
	{
		// RE-FOUND EACH TIME: an earlier departure in this same loop removed its own demand,
		// and may in principle have disturbed another's.
		FFuelDemand* Demand = FindByAircraft(AircraftId);
		if (Demand == nullptr)
		{
			continue;
		}

		const bool bUnfuelled = Demand->State == EFuelDemandState::Unserviceable;
		const EFuelRefusal Why = Demand->Why;
		const int32 Stand = Demand->Stand.Index;

		const EDepartureRefusal Refusal = Traffic.DepartAgent(AircraftId, Network);
		if (Refusal != EDepartureRefusal::None)
		{
			// LOGGED ON A CHANGE OF REASON, not every tick. A runway the player has left
			// occupied refuses this for as long as they leave it, and a line a tick would
			// bury every other line in the file - the busy-depot branch above is quiet for
			// the same reason. Demand re-found because DepartAgent may have moved the array.
			if (FFuelDemand* Still = FindByAircraft(AircraftId);
				Still != nullptr && Still->LastDepartureRefusal != Refusal)
			{
				Still->LastDepartureRefusal = Refusal;
				UE_LOG(LogAirportOps, Log,
					TEXT("Fuel: aircraft %d is ready to leave stand %d but cannot: %s"),
					AircraftId, Stand, *UEnum::GetValueAsString(Refusal));
			}
			continue;
		}

		// SAID WHEN IT LEAVES WITHOUT FUEL. The 'cannot be served' warning fired when the
		// demand went Unserviceable and named what was missing; this says the airport lost
		// the turnaround rather than the stand, which is the consequence the player sees.
		UE_LOG(LogAirportOps, Log,
			TEXT("Fuel: aircraft %d departs stand %d%s"), AircraftId, Stand,
			bUnfuelled
				? *FString::Printf(TEXT(" UNFUELLED - %s"), RefusalText(Why))
				: TEXT(" after its turnaround"));
	}
}

FString UFuelService::DescribeAgent(int32 AgentId) const
{
	const FFuelDemand* Demand = FindByAircraft(AgentId);
	if (Demand == nullptr)
	{
		return FString();
	}

	switch (Demand->State)
	{
	case EFuelDemandState::Needed:       return TEXT("needed");
	case EFuelDemandState::TruckEnRoute: return TEXT("truck en route");
	case EFuelDemandState::Fuelling:     return TEXT("fuelling");
	case EFuelDemandState::Done:         return TEXT("done");
	case EFuelDemandState::Unserviceable: return RefusalText(Demand->Why);
	default:                             return FString();
	}
}
