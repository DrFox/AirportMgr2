#include "Model/JobBoard.h"

#include "AirportOpsLog.h"
#include "Model/FuelSupply.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadTraffic.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAdmission.h"
#include "Model/VehicleFit.h"

FRoutePlan UJobBoard::DepotRoute(const URoadNetwork& Network, FGuidelineNodeId DepotPose, FGuidelineNodeId Goal,
	const FVehicle& Vehicle, bool* bOutTooNarrow, FGuidelineEdgeId* OutNarrowAt) const
{
	// DATED WITH THE GRAPH (#301), same rule as ARigTestCourse::PlanBetween's identical cache: clears
	// itself when Network or its guideline revision moved since the last call, so a stale route or
	// edge-fit answer is never served. Cheap to call every time nothing changed.
	RouteCache.EnsureFresh(Network);

	// CACHED PER (depot pose, stand anchor, vehicle figures) (#301): the SAME pair is asked at every
	// job the stand ever has and by every bid for it, and a refusal is as much a fact about the graph
	// and the vehicle as a route is.
	FRoutePlan Plan;
	if (const FCachedRoutePlan* Hit = RouteCache.Lookup(DepotPose, Goal, Vehicle))
	{
		Plan = Hit->Plan;
	}
	else
	{
		// THROUGH FRouteQuery::For, the one writer of AvoidRunways off the resolved policy (#312).
		// WINGSPAN 0 IS UNLIMITED, and a road guideline carries no span limit either, so neither side
		// of that comparison means anything for a van.
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::CandidateComparison, DepotPose, Goal, 0.0, ETraversalClass::GroundVehicle);

		// NEVER ALONG A STRIP. A truck crossing a runway at a junction is unaffected - a crossing is a
		// turn path and a node, and turn paths carry no DerivedFrom - but taxiing DOWN one is not
		// something a service job may plan. The rule lives in FRoutePolicy::For(CandidateComparison);
		// this paragraph is its justification.

		// NO OCCUPANCY WEIGHT, deliberately. Which vehicle is nearest is a fact about the airport's
		// SHAPE, not about who happens to be on the road this instant; a congestion-weighted length
		// would make the winning bid flicker between ticks and the log unreadable. Congestion is the
		// arbiter's job once the truck is under way. EOccupancyUse::Never on this errand's row IS that
		// rule, and the search REFUSES a table passed alongside it rather than quietly ignoring one.

		// THE VEHICLE THAT WILL DRIVE IT, so a vehicle only bids over road it fits (spec 2026-09-23
		// §6). A depot reachable only over too-narrow road is remembered, so the refusal can say so
		// rather than "no road". AND EACH EDGE'S FIT, cached: most of a Find's cost is tracing the
		// vehicle round every curve the search relaxes, which every depot's own Find asks about the
		// same shared roads.
		Query.WithVehicle(Vehicle);
		Query.FitCache = &RouteCache.FitCacheFor(Vehicle);
		Plan = RouteSearch::Find(Network, Query);
		RouteCache.Store(DepotPose, Goal, Vehicle, Plan, FString());
	}
	if (Plan.Result == ERouteResult::TooNarrow)
	{
		if (bOutTooNarrow != nullptr) { *bOutTooNarrow = true; }
		if (OutNarrowAt != nullptr) { *OutNarrowAt = Plan.RejectedEdge; }
	}
	return Plan;
}

TArray<UJobBoard::FCandidate> UJobBoard::Judge(const URoadNetwork& Network, EServiceRole Role, FEntityInstanceId Stand,
	const TArray<FCandidate>& Candidates, FJudgement& Out) const
{
	const IServiceRolePolicy* Policy = PolicyFor(Role);
	TArray<FCandidate> Eligible;

	// THE DEPOT COUNTS FIRST, off the entities and not the candidates: a depot with no vehicle at all
	// is still a depot to the refusal chain, which must not report "no depot" about one that is there.
	for (const FEntityInstance& Instance : Network.GetEntities())
	{
		// A DEPOT IS AN ENTITY WHOSE POSE IS A SERVICE VEHICLE'S. Read off the instance, where
		// placement captured it: this layer may not dereference a UEntityDefinition.
		if (!Instance.bAlive || Instance.PoseRole != Role)
		{
			continue;
		}
		++Out.Depots;
		// Placed, but with no road within its lead-in reach: FAnchorLink has already warned about it
		// in the census; this is the same fact reaching the player's aircraft card.
		Out.DepotsOnRoad += Network.IsDepotJoined(Instance) ? 1 : 0;
	}

	// THE FLEET COUNTS TOO, off the board's own vehicles and not the candidates: a stranded vehicle is
	// left out of the bidding (AssignOpenJobs) but is still a vehicle, and its job must keep saying what
	// it said before - only a depot with NOTHING in it is NoVehicles.
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
		Out.FleetOnRoad += (Vehicle.Role == Role && Home != nullptr && Home->bAlive && Network.IsDepotJoined(*Home)) ? 1 : 0;
	}

	// JOINED, NOT MERELY RESOLVED - and since the service loop, not merely INCIDENT either.
	//
	// This tested the anchor's IsSet() alone, which is a fact about PLACEMENT and not about the
	// airport: URoadNetwork::PlaceEntity creates a node for every anchor whether or not a lead-in ever
	// reaches it, so the test was true for every Code C stand ever placed and StandUnjoined could not
	// fire at all. Counting incident edges fixed that, and then stopped working for the same shape of
	// reason the moment stands grew SERVICE LANES: a hydrant is ALWAYS spurred to its own lane, so the
	// count is true for a stand in the middle of a field. The question was never "does this node have a
	// line on it" but "does that line go anywhere", which is a walk - see
	// URoadNetwork::IsServiceNodeConnected.
	//
	// Both wrong answers reported NoRoute - "no road from depot" - which sends the player to look at
	// the depot when the road they need is at the stand. Observed in PIE 2026-09-07.
	Out.Hydrant = ServiceAnchorOf(Network, Stand, Role);
	Out.bStandJoined = Out.Hydrant.IsSet() && Network.IsServiceNodeConnected(Out.Hydrant);

	// A STAND DELETED UNDER ITS JOB is asked as Code C's, like an outline reading as no letter - the
	// aircraft's own leaving drops the job a moment later.
	const FEntityInstance* StandInstance = Network.GetEntity(Stand);
	const FVehicle Design = StandInstance != nullptr ? DesignVehicleFor(*StandInstance) : VehiclesFor(EIcaoCode::C);
	Out.DesignType = Design.TypeCode;

	for (const FCandidate& Candidate : Candidates)
	{
		const FEntityInstance* Depot = Network.GetEntity(Candidate.Depot);
		if (Depot == nullptr || !Depot->bAlive || !Network.IsDepotJoined(*Depot))
		{
			continue;
		}

		// NO PUMP, NO FUELLING - checked HERE, before a vehicle bids, rather than at the hydrant where
		// the pumping is computed. A truck sent from a pumpless depot would drive the whole way and
		// then have nothing to do, which reads on screen as the service hanging rather than as a depot
		// the player has not finished building. Only a MODULAR depot can be pumpless - see
		// HasWorkingPump.
		if (Policy != nullptr && Policy->NeedsPumpAtHome() && !HasWorkingPump(Candidate.Depot, *Depot))
		{
			Out.bAnyPumpless = true;
			continue;
		}

		// NO LARGER THAN THE STAND WAS BUILT FOR (spec 2026-09-26 section 2): the stand's lane legs are
		// proven drivable by its definition's design vehicle and anything VehicleFit::NoLargerThan it,
		// and nothing else. PER VEHICLE - a typed fleet makes the vehicle a fact about this candidate,
		// and a depot with a bowser AND a tow is too large for an A stand only through its bowser.
		const FServiceVehicleType Type = TypeFor(Candidate.TypeCode);
		// A KIND WITH NO CATALOGUE ROW IS NO CANDIDATE (#430): its chassis is empty, and an empty chassis is NoLargerThan
		// every stand (it compares zeros) and routes at the default speed - the zero-size vehicle a buyable type with no
		// stand letter used to become. TypeFor has warned; FServiceFleet::Add never makes one, so only a vehicle restored
		// under a scenario that dropped its kind, or a test's hand, gets here.
		// ENFORCED BY: AirportOps.Fleet.UnknownKindServesNothing, AirportOps.Fleet.CatalogueDropsARowWithNoChassis (Add)
		//
		// COUNTED, NOT ONLY SKIPPED (#478): the skip set no flag, so a depot whose vehicles were ALL of unknown kinds fell
		// through RefusalOf to NoRoute - "no road from depot", about a depot on a road that reaches the stand. The count says
		// "nothing here could be sized", which is not a road problem and not an empty depot: EServiceRefusal::UnknownVehicleKind.
		// ENFORCED BY: AirportOps.Fuel.UnknownKindSaysSo
		if (Type.TypeCode.IsNone())
		{
			++Out.UnknownKinds;
			continue;
		}
		++Out.KnownKinds;
		if (!VehicleFit::NoLargerThan(Type.Vehicle, Design))
		{
			Out.bAnyTooLarge = true;
			Out.TooLargeType = Type.TypeCode;
			continue;
		}

		if (!Out.bStandJoined)
		{
			// Nothing to route TO. Skipped here as well as reported, so a stand with an unjoined anchor
			// does not cost a search per candidate per bid.
			continue;
		}

		bool bNarrow = false;
		FGuidelineEdgeId NarrowAt;
		const FRoutePlan Plan = DepotRoute(Network, Depot->PoseNode, Out.Hydrant, Type.Vehicle, &bNarrow, &NarrowAt);
		if (bNarrow)
		{
			Out.bAnyTooNarrow = true;
			Out.NarrowAt = NarrowAt;
		}
		if (Plan.IsValid())
		{
			Eligible.Add(Candidate);
		}
	}
	return Eligible;
}

EServiceRefusal UJobBoard::RefusalOf(const FJudgement& Out)
{
	// THE ORDER OF THESE TESTS IS THE SPEC'S, and it is the order of the player's hand: no depot at all
	// is a building to place, a depot off the road is a road to draw, and only then is it worth talking
	// about the stand or the graph.
	if (Out.Depots == 0)
	{
		return EServiceRefusal::NoDepot;
	}
	if (Out.DepotsOnRoad == 0)
	{
		return EServiceRefusal::NoRoad;
	}
	if (Out.FleetOnRoad == 0)
	{
		// A DEPOT ON A ROAD WITH NOTHING IN IT: the next thing in the player's hand is the buy button on
		// its card, before anything about the stand or the graph.
		return EServiceRefusal::NoVehicles;
	}
	if (!Out.bStandJoined)
	{
		return EServiceRefusal::StandUnjoined;
	}
	if (Out.bAnyPumpless)
	{
		// BEFORE NoRoute, because this is a thing the player can go and fix. Falling through to
		// NoRoute would have told them "no road from depot" about a depot sitting on a road.
		return EServiceRefusal::NoPump;
	}
	if (Out.UnknownKinds > 0 && Out.KnownKinds == 0)
	{
		// EVERY VEHICLE JUDGED HAD NO CATALOGUE ROW (#478): not one was sized, so nothing below - too large, too narrow, no
		// route - was ever asked of a vehicle, and falling to NoRoute would blame a road nobody tried. A depot with ONE known
		// vehicle among the unknown ones does not come here: its own verdict (too large, no route...) is the honest one.
		// AFTER NoPump, which is flagged before a vehicle's kind is looked at and is the nearer fix.
		// ENFORCED BY: AirportOps.Fuel.UnknownKindSaysSo
		return EServiceRefusal::UnknownVehicleKind;
	}
	if (Out.bAnyTooLarge)
	{
		// AFTER NoPump: a depot with no pump would not be sending anything at all, so the pump is the
		// nearer fix.
		return EServiceRefusal::VehicleTooLarge;
	}
	if (Out.bAnyTooNarrow)
	{
		return EServiceRefusal::TooNarrow;
	}
	if (Out.bNoStock)
	{
		// LAST BUT NoRoute (spec 2026-10-02 §7): every vehicle that could reach the stand would arrive with nothing. After
		// every road and vehicle refusal - a dry airport with no road is a road problem first, and stock is the one fix
		// that comes by the market rather than by building.
		return EServiceRefusal::NoFuelStock;
	}
	return EServiceRefusal::NoRoute;
}

double UJobBoard::DriveSeconds(const URoadNetwork& Network, FGuidelineNodeId From, FGuidelineNodeId To,
	const FServiceVehicleType& Type) const
{
	if (From == To)
	{
		return 0.0;
	}
	if (LegLengthsNetwork != &Network || LegLengthsRevision != Network.GetGuidelineRevision())
	{
		LegLengths.Reset();
		LegLengthsNetwork = &Network;
		LegLengthsRevision = Network.GetGuidelineRevision();
	}

	const FLegKey Key{ From, To, Type.TypeCode };
	double Length = -1.0;
	if (const double* Hit = LegLengths.Find(Key))
	{
		Length = *Hit;
	}
	else
	{
		// FROM A DEPOT, THE DEPOT ROUTE the eligibility already found - one cache, so a bid from an
		// idle vehicle costs no search the judgement did not already make. Anything else - stand to
		// stand, stand to depot - is searched here, gated first and ungated if the gated search
		// refuses: a leg off a service point opens with its reverse, which an UNSEEDED search may
		// refuse where the live, seeded one will not. The ungated length is a fair estimate of it.
		RouteCache.EnsureFresh(Network);
		FRoutePlan Plan = RouteCache.Lookup(From, To, Type.Vehicle) != nullptr
			? RouteCache.Lookup(From, To, Type.Vehicle)->Plan : FRoutePlan();
		if (!Plan.IsValid())
		{
			FRouteQuery Query = FRouteQuery::For(ERouteErrand::CandidateComparison, From, To, 0.0, ETraversalClass::GroundVehicle);
			Query.WithVehicle(Type.Vehicle);
			Plan = RouteSearch::Find(Network, Query);
			if (!Plan.IsValid())
			{
				Query.Vehicle = nullptr;
				Plan = RouteSearch::Find(Network, Query);
			}
		}
		Length = Plan.IsValid() ? Plan.Length : -1.0;
		LegLengths.Add(Key, Length);
	}
	if (Length < 0.0)
	{
		return -1.0;
	}
	// CRUISE SPEED ONLY: acceleration, corners and the service reverse make the real drive longer, and
	// they do so for every bidder alike - the bid ranks correctly without being a promise to the second.
	return Length / FMath::Max(Type.Vehicle.Chassis.Ground.Taxi.SpeedCap, 1.0);
}

// THE REFILL, LIVE AND PRICED, IN ONE FILE (2026-10-02, spec §7): BeginFacility draws the airport's stock and BidFor
// below prices the same limit through the same two policy calls. MOVED HERE FROM JobBoard.cpp with that change, which
// also keeps JobBoard.cpp inside its Check-Architecture rule 77 figure - the stock is a new responsibility, and it
// entered beside the bid that must agree with it rather than growing the orchestrator.

double UJobBoard::FuelAvailable() const
{
	return FuelSupply != nullptr ? FuelSupply->Available() : TNumericLimits<double>::Max();
}

int32 UJobBoard::ReopenStockRefusals()
{
	// THROUGH ReopenRefusedJob, the graph-change re-offer's own door: Open with the reason KEPT (#445's "refused, asking
	// again"), so the alert stands through the frame the bid is pending, and the bid that follows clears it or re-refuses.
	int32 Reopened = 0;
	for (FServiceJob& Job : Jobs)
	{
		if (Job.State == EServiceJobState::Unserviceable && Job.Why == EServiceRefusal::NoFuelStock)
		{
			ReopenRefusedJob(Job);
			++Reopened;
		}
	}
	if (Reopened > 0)
	{
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: %d job(s) re-opened by a delivery"), Reopened);
	}
	return Reopened;
}

double UJobBoard::FuelCapacityLitres(const URoadNetwork& Network, double LitresPerTank) const
{
	// THE SAME LIVE-DEPOT WALK UFacilityPurchases makes for its seated modules (FacilityPurchases.cpp, the excess pass), so
	// a tank counts here exactly when the plot seats it - an owned tank the plot cannot hold holds no fuel.
	int32 Tanks = 0;
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Entity = Entities[Index];
		if (Entity.bAlive && Entity.IsDepot())
		{
			Tanks += CapabilityOf(Network.EntityIdAt(Index), Entity).Tanks();
		}
	}
	return Tanks * FMath::Max(LitresPerTank, 0.0);
}

void UJobBoard::BeginFacility(FServiceVehicle& Vehicle, const URoadNetwork& Network, const USimClock& Clock)
{
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
	// DRAWN NOW, not when the pumping ends: two trucks home together must not both be promised the last 500 L. What is
	// granted is held on the vehicle (RefillLitres, saved) and added when the refill ends (Step's AtFacility branch).
	// FUEL'S STOCK, drawn for whatever role this is: the board is fuel's until a second role is scheduled (see JobBoard.h),
	// and a second role's facility would need its own supply here.
	const double Missing = Policy != nullptr ? FMath::Max(FFuelRolePolicy::CapacityOf(TypeFor(Vehicle.TypeCode)) - Vehicle.Cargo, 0.0) : 0.0;
	Vehicle.RefillLitres = FuelSupply != nullptr ? FuelSupply->Draw(Missing) : Missing;
	// A DRY REFILL FOR AN EMPTY TRUCK RELEASES ITS QUEUE (spec 2026-10-02 §7): ServiceBid's dry rule, live, on the same two
	// figures - nothing on board and nothing granted, each below DoneWithin. Without it the truck went Idle in no time and
	// StartNext sent it here again, every Step, its jobs held for ever. RELEASED, NOT REFUSED HERE: the jobs go back to the
	// board, whose bid refuses them NoFuelStock - or gives them to a truck that still carries fuel (partial service beats
	// none). A truck with cargo is not released: NextStep sends it straight there with what it has.
	// ENFORCED BY: AirportOps.Fuel.DryRefillReleasesItsQueue
	if (Policy != nullptr && Vehicle.Queue.Num() > 0 && Vehicle.Cargo < Policy->DoneWithin() && Vehicle.RefillLitres < Policy->DoneWithin())
	{
		for (const int32 JobId : Vehicle.Queue)
		{
			if (const FServiceJob* Job = FindJob(JobId))
			{
				UE_LOG(LogAirportOps, Log, TEXT("Fuel: depot dry - vehicle %d gives up job %d after %.0f L"), Vehicle.Id, Job->Id, Job->QuantityDelivered);
			}
		}
		ReleaseJobsOf(Vehicle);
	}
	const double Seconds = Policy != nullptr && Home != nullptr
		? Policy->FacilitySeconds(Vehicle.Cargo, TypeFor(Vehicle.TypeCode), PumpsAt(Vehicle.Home, *Home), Vehicle.RefillLitres) : 0.0;
	Lifecycle(Vehicle).BeginFacility(Clock.Now() + Seconds);
	if (Seconds > 0.0)
	{
		// REFILL BEFORE IT IS FREE (spec 2026-09-28-fuel-litres): what it pumped out, at the depot's
		// pumps. Nothing pumped (a recall mid-leg, or a dry airport) makes this zero, and it is free on the next tick.
		UE_LOG(LogAirportOps, Log, TEXT("Fuel: depot %d refilling vehicle %d (%.0f L, %.1f game min, stock left %.0f L)"),
			Vehicle.Home.Index, Vehicle.Id, Vehicle.RefillLitres, Seconds / 60.0, FuelSupply != nullptr ? FuelSupply->Available() : -1.0);
	}
}

ServiceBid::FResult UJobBoard::BidFor(const FServiceVehicle& Vehicle, const FServiceJob& Job, const UGroundTraffic& Traffic,
	const URoadNetwork& Network, const USimClock& Clock, int32 QueueAhead) const
{
	++BidCallCountForTest;
	const IServiceRolePolicy* Policy = PolicyFor(Vehicle.Role);
	const FServiceVehicleType Type = TypeFor(Vehicle.TypeCode);
	const FEntityInstance* Home = Network.GetEntity(Vehicle.Home);
	if (Policy == nullptr || Home == nullptr)
	{
		ServiceBid::FResult Unreachable;
		Unreachable.bReachable = false;
		return Unreachable;
	}

	// ONE CLOCK: the bid is in GAME seconds. Serves and refills already are; a drive is MOVEMENT time,
	// which runs at the speed multiplier while game time runs at the multiplier times the day
	// compression - so movement seconds times game-per-real at the hour is the drive in game seconds.
	// Taken once per bid; the hour a bid spans barely moves it.
	const double GamePerMovement = Clock.GameSecondsOfMovement(1.0);   // game seconds one movement second is worth at this hour

	// NODES AS SMALL INTS for the pure simulation, which knows nothing of the graph.
	TArray<FGuidelineNodeId> Nodes;
	auto NodeIndex = [&Nodes](FGuidelineNodeId Node) { return Nodes.AddUnique(Node); };

	ServiceBid::FInput In;
	In.Type = &Type;
	In.Policy = Policy;
	In.Pumps = PumpsAt(Vehicle.Home, *Home);
	In.FacilityAvailable = FuelAvailable();   // A SNAPSHOT - see FInput::FacilityAvailable for why that is accepted
	In.FacilityNode = NodeIndex(Home->PoseNode);
	In.DriveSeconds = [this, &Nodes, &Network, &Type, GamePerMovement](int32 From, int32 To)
	{
		if (DriveSecondsOverride)
		{
			return DriveSecondsOverride(Nodes[From], Nodes[To], Type.TypeCode);
		}
		const double Movement = DriveSeconds(Network, Nodes[From], Nodes[To], Type);
		return Movement < 0.0 ? -1.0 : Movement * GamePerMovement;
	};

	// WHERE AND WHEN THE CURRENT STEP LEAVES IT, and with what on board.
	const double Now = Clock.Now();
	const FRoadAgent* Agent = Vehicle.AgentId != 0 ? Traffic.FindAgent(Vehicle.AgentId) : nullptr;
	// WHAT IS LEFT OF THE LEG IT IS DRIVING, at cruise - Airside's answer (UGroundTraffic::RemainingDriveSeconds, #429),
	// which used to be read here off the follower's plan and distance. Movement seconds, converted to the bid's one clock.
	// ENFORCED BY: AirportOps.Service.Bid.OutVehiclePricesItsRemainingDrive
	auto RemainingDrive = [&]() -> double
	{
		return Agent != nullptr ? Traffic.RemainingDriveSeconds(Vehicle.AgentId) * GamePerMovement : 0.0;
	};
	const FServiceJob* Current = Vehicle.CurrentJob != 0 ? FindJob(Vehicle.CurrentJob) : nullptr;

	In.FreeAt = Now;
	In.CargoWhenFree = Vehicle.Cargo;
	In.NodeWhenFree = In.FacilityNode;
	switch (Vehicle.State)
	{
	case EServiceVehicleState::AtFacility:
		In.FreeAt = FMath::Max(Vehicle.StepEndsAt, Now);
		// THE LITRES ALREADY GRANTED, not the stock: BeginFacility drew them, and the stock no longer holds them.
		In.CargoWhenFree = Policy->CargoAfterFacility(Vehicle.Cargo, Type, Vehicle.RefillLitres);
		break;
	case EServiceVehicleState::ToFacility:
		In.FreeAt = Now + RemainingDrive() + Policy->FacilitySeconds(Vehicle.Cargo, Type, In.Pumps, In.FacilityAvailable);
		In.CargoWhenFree = Policy->CargoAfterFacility(Vehicle.Cargo, Type, In.FacilityAvailable);
		// THAT REFILL SPENDS THE SNAPSHOT TOO, so the simulation's own refills price only what it leaves - the rule
		// ServiceBid::Finish applies between its trips, applied to the refill this vehicle is already driving to.
		In.FacilityAvailable = FMath::Max(In.FacilityAvailable - FMath::Max(In.CargoWhenFree - Vehicle.Cargo, 0.0), 0.0);
		break;
	case EServiceVehicleState::ToJob:
		if (Current != nullptr)
		{
			const double Trip = Policy->TripQuantity(Vehicle.Cargo, Type, Current->QuantityOwed);
			In.FreeAt = Now + RemainingDrive() + Policy->ServeSeconds(Type, Trip);
			In.CargoWhenFree = Policy->CargoAfterServe(Vehicle.Cargo, Trip);
			In.NodeWhenFree = NodeIndex(ServiceAnchorOf(Network, Current->Stand, Vehicle.Role));
		}
		break;
	case EServiceVehicleState::Serving:
		if (Current != nullptr)
		{
			In.FreeAt = FMath::Max(Current->TripEndsAt, Now);
			In.CargoWhenFree = Policy->CargoAfterServe(Vehicle.Cargo, Current->TripQuantity);
		}
		// PARKED AT A STAND whether or not the trip is still running.
		if (Agent != nullptr)
		{
			In.NodeWhenFree = NodeIndex(Agent->GoalNode);
		}
		break;
	case EServiceVehicleState::Deciding:
		// A VEHICLE WHOSE SERVE JUST ENDED (or whose job was taken from under it) is still where it stands,
		// deciding where next: free NOW, with the cargo it has, at the node its agent is parked on. Priced as
		// ToFacility it would be "home and refilled first", and the vehicle that just pumped could never win its own
		// remainder back at the price StartNext will actually give it. This is what the illegal "Serving with no
		// job" was priced as by falling through the Serving case with no Current - and the state that replaces it.
		// ENFORCED BY: AirportOps.Service.Bid.DecidingVehiclePricesWhereItStands (deleting this case turns it red;
		// the one-bowser ChainsStandToStand / ShortTank tests stay green without it, a lone candidate winning at any price)
		if (Agent != nullptr)
		{
			In.NodeWhenFree = NodeIndex(Agent->GoalNode);
		}
		break;
	default:
		break;
	}

	const int32 Ahead = QueueAhead == INDEX_NONE ? Vehicle.Queue.Num() : FMath::Min(QueueAhead, Vehicle.Queue.Num());
	for (int32 At = 0; At < Ahead; ++At)
	{
		if (const FServiceJob* Queued = FindJob(Vehicle.Queue[At]))
		{
			In.Queued.Add({ NodeIndex(ServiceAnchorOf(Network, Queued->Stand, Vehicle.Role)), Queued->QuantityOwed });
		}
	}
	In.Appended = { NodeIndex(ServiceAnchorOf(Network, Job.Stand, Vehicle.Role)), Job.QuantityOwed };
	return ServiceBid::Finish(In);
}

ServiceBid::FResult UJobBoard::BidForTest(const UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock,
	int32 VehicleId, int32 JobId) const
{
	const FServiceVehicle* Vehicle = FindVehicle(VehicleId);
	const FServiceJob* Job = FindJob(JobId);
	if (Vehicle == nullptr || Job == nullptr)
	{
		return ServiceBid::FResult();
	}
	// THE JUDGEMENT FIRST, as AssignOpenJobs makes it: its route is the one the bid's first leg reads.
	FJudgement Judged;
	Judge(Network, Job->Role, Job->Stand, { FCandidate{ Vehicle->Home, Vehicle->TypeCode, Vehicle->Id } }, Judged);
	return BidFor(*Vehicle, *Job, Traffic, Network, Clock);
}

void UJobBoard::Assign(FServiceVehicle& Vehicle, FServiceJob& Job, double PromisedFinish)
{
	++RevisionCount;   // See Revision: a busy vehicle's queue grew, which no lifecycle transition reports.
	Vehicle.Queue.Add(Job.Id);
	Job.State = EServiceJobState::Queued;
	Job.VehicleId = Vehicle.Id;
	Job.PromisedFinish = PromisedFinish;
	Job.Why = EServiceRefusal::None;
}

TArray<UJobBoard::FCandidate> UJobBoard::CandidatesFor(EServiceRole Role, const UGroundTraffic& Traffic, int32 Except) const
{
	TArray<FCandidate> Candidates;
	for (const FServiceVehicle& Vehicle : Vehicles)
	{
		if (Vehicle.Role != Role || Vehicle.Id == Except)
		{
			continue;
		}
		// NOT A STRANDED ONE: it prices itself as "home soon" (ToFacility with no plan left, so no drive remaining), wins,
		// and holds the job for a trip it will never make - the wedge OnAgentPhase's Stranded branch releases jobs from.
		// Its jobs are already back on the board (LoseAgent / ReleaseJobsOf), and it bids for nothing until the player
		// unsticks it.
		if (IsStranded(Vehicle, Traffic))
		{
			continue;
		}
		Candidates.Add({ Vehicle.Home, Vehicle.TypeCode, Vehicle.Id });
	}
	return Candidates;
}

void UJobBoard::AssignOpenJobs(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	const uint32 Revision = Network.GetGuidelineRevision();
	for (FServiceJob& Job : Jobs)
	{
		if (Job.State != EServiceJobState::Open)
		{
			continue;
		}

		// NOT A STRANDED ONE (CandidatesFor says why): the wedge OnAgentPhase's Stranded branch just released the job from.
		FJudgement Judged;
		const TArray<FCandidate> Eligible = Judge(Network, Job.Role, Job.Stand, CandidatesFor(Job.Role, Traffic), Judged);

		// THE BIDS: when each would FINISH this job appended to its queue (user's ruling 6). The
		// runner-up is kept for the log, because "why did the tow go?" is the first question in play.
		FServiceVehicle* Best = nullptr;
		double BestFinish = TNumericLimits<double>::Max();
		const FServiceVehicle* Next = nullptr;
		double NextFinish = TNumericLimits<double>::Max();
		int32 Bidders = 0;
		int32 DryBidders = 0;   // NoFuelStock only when EVERY bid failed for the stock alone - see FJudgement::bNoStock
		for (const FCandidate& Candidate : Eligible)
		{
			FServiceVehicle* Vehicle = FindVehicleMutable(Candidate.VehicleId);
			if (Vehicle == nullptr)
			{
				continue;
			}
			const ServiceBid::FResult Bid = BidFor(*Vehicle, Job, Traffic, Network, Clock);
			++Bidders;
			DryBidders += Bid.bNoStock ? 1 : 0;
			if (!Bid.bReachable)
			{
				continue;
			}
			if (Bid.Finish < BestFinish)
			{
				Next = Best;
				NextFinish = BestFinish;
				Best = Vehicle;
				BestFinish = Bid.Finish;
			}
			else if (Bid.Finish < NextFinish)
			{
				Next = Vehicle;
				NextFinish = Bid.Finish;
			}
		}

		if (Best == nullptr)
		{
			// NOTHING MAY BID. Unserviceable, which is TERMINAL until the graph changes (or, for NoFuelStock, until fuel
			// arrives - ReopenStockRefusals) - and there is no "busy" to fall back on here, because a busy vehicle bids
			// with its queue.
			++RevisionCount;   // See Revision: the job's state and reason changed, and the fuel line says why.
			Judged.bNoStock = Bidders > 0 && DryBidders == Bidders;
			Job.State = EServiceJobState::Unserviceable;
			Job.Why = RefusalOf(Judged);
			Job.RefusedAtRevision = Revision;
			if (Job.Why == EServiceRefusal::VehicleTooLarge)
			{
				UE_LOG(LogAirportOps, Warning,
					TEXT("Fuel: %s is larger than the %s this stand was built for; no depot can serve it"),
					*Judged.TooLargeType.ToString(), *Judged.DesignType.ToString());
			}
			else if (Job.Why == EServiceRefusal::TooNarrow)
			{
				UE_LOG(LogAirportOps, Warning,
					TEXT("Fuel: no road wide enough for any fuel vehicle from any depot - the first edge one does not fit is guideline edge %d"),
					Judged.NarrowAt.Index);
			}
			else if (Job.Why == EServiceRefusal::NoFuelStock)
			{
				UE_LOG(LogAirportOps, Warning, TEXT("Fuel: %d vehicle(s) could reach aircraft %d but none carries fuel, and the airport holds %.1f L"),
					Bidders, Job.AircraftId, FuelAvailable());
			}

			// THE COUNTS THAT DECIDED IT, in the line itself. A bare reason sent the player to look at
			// the wrong end of the airport once already (PIE 2026-09-07); these three numbers say which
			// end without a second repro - read off the judgement, which already counted them.
			UE_LOG(LogAirportOps, Warning,
				TEXT("Fuel: aircraft %d at stand %d cannot be served: %s. %d depot(s), %d on a "
					 "road; the stand's hydrant %s. Check the 'Anchor links:' line."),
				Job.AircraftId, Job.Stand.Index, RefusalText(Job.Why),
				Judged.Depots, Judged.DepotsOnRoad,
				!Judged.Hydrant.IsSet() ? TEXT("has no node at all")
					: Network.IsServiceNodeConnected(Judged.Hydrant) ? TEXT("reaches a road")
					: TEXT("reaches NO road from any of its stand's entrances"));
			continue;
		}

		Assign(*Best, Job, BestFinish);
		// THE POLICY'S TANK, FLOORED - the one the stand writes (#430, the #443 A13 note). This read the row's raw
		// Capacity, so a zero-tank kind's card said 0 L from the bid until the truck reached the stand.
		Job.TankLitres = FFuelRolePolicy::CapacityOf(TypeFor(Best->TypeCode));
		UE_LOG(LogAirportOps, Log,
			TEXT("Bid: job %d (fuel %.0f L, aircraft %d, stand %d) -> vehicle %d %s finish +%.1f game min (next: %s)"),
			Job.Id, Job.QuantityOwed, Job.AircraftId, Job.Stand.Index, Best->Id, *Best->TypeCode.ToString(),
			(BestFinish - Clock.Now()) / 60.0,
			Next != nullptr
				? *FString::Printf(TEXT("vehicle %d %s +%.1f"), Next->Id, *Next->TypeCode.ToString(), (NextFinish - Clock.Now()) / 60.0)
				: TEXT("none"));
	}
}

void UJobBoard::RebidQueued(UGroundTraffic& Traffic, const URoadNetwork& Network, const USimClock& Clock)
{
	// NOTHING CHANGED, NOTHING RE-BID (#190's rule): a vehicle's step ending, a vehicle added or
	// withdrawn (FleetRevision) and an airport edit (the guideline revision) are the only things that
	// can make a different vehicle the better one.
	const uint32 Revision = Network.GetGuidelineRevision();
	if (LastRebidFleetRevision == FleetRevision && LastRebidGuidelineRevision == Revision)
	{
		return;
	}
	LastRebidFleetRevision = FleetRevision;
	LastRebidGuidelineRevision = Revision;

	// IDS FIRST: a move edits two queues, and the walk must not be over either of them.
	TArray<int32> Queued;
	for (const FServiceJob& Job : Jobs)
	{
		if (Job.State == EServiceJobState::Queued)
		{
			Queued.Add(Job.Id);
		}
	}
	// IT RAN AGAINST QUEUED JOBS, and a run refreshes each one's PromisedFinish (the depot card's "clears in" reads it)
	// even when nothing moves vehicle - so it is a change for Revision, which a guideline edit alone (no vehicle
	// transition) would otherwise leave standing. With none queued there is nothing to refresh: the first Step of a
	// board runs this once regardless (the stamps start at MAX), and that must not read as a change.
	if (Queued.Num() > 0)
	{
		++RevisionCount;
	}

	for (const int32 JobId : Queued)
	{
		FServiceJob* Job = FindJobMutable(JobId);
		FServiceVehicle* Holder = Job != nullptr ? FindVehicleMutable(Job->VehicleId) : nullptr;
		if (Job == nullptr || Holder == nullptr || Job->State != EServiceJobState::Queued)
		{
			continue;
		}
		const int32 Position = Holder->Queue.Find(JobId);
		if (Position == INDEX_NONE)
		{
			continue;
		}

		// ITS CURRENT FINISH, where it is: the holder's queue up to it, then it. Re-computed rather than
		// read off PromisedFinish, because the holder's plan has moved since (a serve ran long, a job
		// ahead of it left) and a stale promise would compare the alternative against a fiction.
		const ServiceBid::FResult Current = BidFor(*Holder, *Job, Traffic, Network, Clock, Position);

		// THE ALTERNATIVES: every candidate but the holder itself.
		FJudgement Judged;
		FServiceVehicle* Best = nullptr;
		double BestFinish = TNumericLimits<double>::Max();
		for (const FCandidate& Candidate : Judge(Network, Job->Role, Job->Stand, CandidatesFor(Job->Role, Traffic, Holder->Id), Judged))
		{
			FServiceVehicle* Vehicle = FindVehicleMutable(Candidate.VehicleId);
			if (Vehicle == nullptr)
			{
				continue;
			}
			const ServiceBid::FResult Bid = BidFor(*Vehicle, *Job, Traffic, Network, Clock);
			if (Bid.bReachable && Bid.Finish < BestFinish)
			{
				Best = Vehicle;
				BestFinish = Bid.Finish;
			}
		}

		const double CurrentFinish = Current.bReachable ? Current.Finish : TNumericLimits<double>::Max();
		if (Best == nullptr || BestFinish >= CurrentFinish - RebidMarginSeconds)
		{
			// STAYS - but with the promise brought up to date, so the card and the next re-bid read what
			// is now true.
			Job->PromisedFinish = CurrentFinish;
			continue;
		}

		Holder->Queue.RemoveAt(Position);
		Assign(*Best, *Job, BestFinish);
		Job->TankLitres = FFuelRolePolicy::CapacityOf(TypeFor(Best->TypeCode));   // the one tank rule - see the bid's
		UE_LOG(LogAirportOps, Log, TEXT("Rebid: job %d (aircraft %d, stand %d) vehicle %d -> %d, +%.1f -> +%.1f game min"),
			Job->Id, Job->AircraftId, Job->Stand.Index, Holder->Id, Best->Id,
			(CurrentFinish - Clock.Now()) / 60.0, (BestFinish - Clock.Now()) / 60.0);
	}
}

bool UJobBoard::CouldServe(const UGroundTraffic& Traffic, const URoadNetwork& Network, const FAirframe& Airframe) const
{
	// THE CANDIDATES THE BOARD HAS, and only those (#443). This used to add the STARTER fleet a not-yet-seeded depot would
	// get, so an offer asked before the first tick could say yes - a prediction written beside the seeding it had to mirror
	// (FServiceFleet::SeedStarterFleets), and one that had already drifted from it once: a starter depot whose fleet the
	// player sold said "fuel OK" on the offer while the board refused the aircraft NoVehicles (final review 2026-09-30).
	// The seeding is the "FleetSeed" pass's, through the fleet's door, and runs before the job board's pass in the drain that
	// heard of the depot, so a starter depot has real vehicles before any bid could use them and there is nothing to predict; a depot the player drew has Trucks 0 and, until a vehicle is bought, no
	// candidate - its offers say "no fuel", and so does a seeded depot whose fleet was sold. THE TRAFFIC MODEL IS ASKED, as
	// the bids ask it (#443): a stranded vehicle is no candidate, and this used to pass null on the reasoning that an offer
	// is judged before its aircraft exists so no agent is stranded - but the vehicles and their agents exist without the
	// aircraft, and a depot whose only bowser stood stranded said "fuel OK" while every job would be refused.
	// ENFORCED BY: AirportOps.Fuel.CouldServe.StarterDepotVerdictAgreesWithItsFirstBid, AirportOps.Fuel.CouldServe.SoldOutStarterDepotCannot,
	// AirportOps.Fuel.CouldServe.StrandedOnlyVehicleCannot
	const TArray<FCandidate> Candidates = CandidatesFor(EServiceRole::Fuel, Traffic);
	const TArray<FEntityInstance>& Entities = Network.GetEntities();
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		const FEntityInstance& Stand = Entities[Index];
		// THE SAME TWO FILTERS UStandAllocator::Hold applies to every stand it holds (Reserve's, until #471 took Reserve
		// away), so "a stand it would take" means a stand an accept could actually hold.
		if (!Stand.IsStandCandidate() || !StandAdmission::Judge(Network, Stand, Airframe).IsAdmitted())
		{
			continue;
		}
		FJudgement Judged;
		if (Judge(Network, EServiceRole::Fuel, Network.EntityIdAt(Index), Candidates, Judged).Num() > 0)
		{
			return true;
		}
	}
	return false;
}
