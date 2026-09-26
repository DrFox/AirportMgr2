#include "RigYard.h"

#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/VehicleFit.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "RigTestCourse.h"
#include "RoadBuildLog.h"
#include "Solve/RoadGeom.h"
#include "Tool/RoadEditTarget.h"

bool FRigYardLayout::Lay(IRoadEditTarget& Target, TArray<FReverseTurn>& OutTurns, FRoadNodeId& OutStartNode,
	FRoadNodeId& OutStartFrom, int32& OutSegmentsLaid, int32& OutRefused)
{
	OutTurns.Reset();
	OutSegmentsLaid = 0;
	OutRefused = 0;
	auto Place = [&Target](double X, double Y) { return Target.PlaceNode(FVector2D(X, Y)); };
	auto Id = [&Target](int32 Index)
	{
		FRoadNodeId Out;
		Target.MakeLiveNodeId(Index, Out);
		return Out;
	};
	auto Connect = [&Target, &OutSegmentsLaid, &OutRefused](int32 A, int32 B)
	{
		if (Target.ConnectNodes(A, B, ERoadKind::ServiceRoad, Tier))
		{
			++OutSegmentsLaid;
		}
		else
		{
			++OutRefused;
			UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: ConnectNodes(%d, %d, tier %d) refused."), A, B, Tier);
		}
	};

	const int32 NW = Place(WestX, NorthY);
	const int32 W = Place(StraightBayX, NorthY);
	const int32 P1 = Place(P1X, NorthY);
	const int32 J = Place(JX, NorthY);
	const int32 P2 = Place(P2X, NorthY);
	const int32 NE = Place(EastX, NorthY);
	const int32 Bay90 = Place(JX, Bay90Y);
	const int32 S = Place(EastX, SpurY);
	const int32 SE = Place(EastX, SouthY);
	const int32 SW = Place(WestX, SouthY);
	const int32 H = Place(HX, SpurY);
	const int32 D = Place(DX, SpurY);
	const int32 Hammer = Place(HX, HammerY);

	// THE RING, one way round being as good as the other - every road is two-way.
	Connect(NW, P1);
	Connect(P1, J);
	Connect(J, P2);
	Connect(P2, NE);
	Connect(NE, S);
	Connect(S, SE);
	Connect(SE, SW);
	Connect(SW, NW);
	// THE BAYS AND THE SPUR.
	Connect(W, NW);
	Connect(J, Bay90);
	Connect(S, H);
	Connect(H, D);
	Connect(H, Hammer);

	// THE THREE REVERSE TURNS, in ERigYardFeature order: the arm pulled past along, then the one
	// backed into. The straight bay's two arms are opposite - the reverse runs straight through NW.
	const int32 Records[3][3] = { { NW, P1, W }, { J, P2, Bay90 }, { H, D, Hammer } };
	for (const auto& Record : Records)
	{
		if (!Target.AddReverseTurn(Record[0], Record[1], Record[2]))
		{
			UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: AddReverseTurn(%d, %d, %d) refused."), Record[0], Record[1], Record[2]);
			continue;
		}
		FReverseTurn& Turn = OutTurns.AddDefaulted_GetRef();
		Turn.Node = Id(Record[0]);
		Turn.FromFar = Id(Record[1]);
		Turn.IntoFar = Id(Record[2]);
	}

	// A RUNNER STARTS on the south side heading west, at SW: its first leg turns north up the
	// west side and east at NW, the approach the straight bay is laid for.
	OutStartNode = Id(SW);
	OutStartFrom = Id(SE);
	return OutRefused == 0 && OutTurns.Num() == static_cast<int32>(ERigYardFeature::Count);
}

const TCHAR* FRigYard::FeatureName(ERigYardFeature Feature)
{
	switch (Feature)
	{
	case ERigYardFeature::StraightBay: return TEXT("straight bay");
	case ERigYardFeature::Bay90:       return TEXT("90 degree bay");
	case ERigYardFeature::Hammerhead:  return TEXT("hammerhead");
	default:                           return TEXT("?");
	}
}

void FRigYard::Build(IRoadEditTarget& Target, int32 VehicleCount)
{
	const bool bLaid = FRigYardLayout::Lay(Target, Turns, StartNode, StartFrom, SegmentsLaid, SegmentsRefused);
	Runners.Reset();
	for (int32 Slot = 0; Slot < VehicleCount; ++Slot)
	{
		FRigYardRunner& Runner = Runners.AddDefaulted_GetRef();
		Runner.Slot = Slot;
		Runner.StartDelay = Slot * StartStagger;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("RigYard: yard laid - %d segment(s) (%d refused), %d reverse turn(s)%s."),
		SegmentsLaid, SegmentsRefused, Turns.Num(), bLaid ? TEXT("") : TEXT(" - INCOMPLETE, see the warnings above"));
}

FGuidelineNodeId FRigYard::GoalNode(const URoadNetwork& Network, ERigYardFeature Feature) const
{
	const int32 Mine = static_cast<int32>(Feature);
	if (!Turns.IsValidIndex(Mine))
	{
		return FGuidelineNodeId();
	}
	// BY IDENTITY, NOT BY INDEX: the network's record list is shared with anything else that
	// lays reverse turns, and a record dropped ahead of these would shift every index.
	const TArray<FReverseTurn>& OnFile = Network.GetReverseTurns();
	for (int32 Index = 0; Index < OnFile.Num(); ++Index)
	{
		if (OnFile[Index].Node == Turns[Mine].Node && OnFile[Index].FromFar == Turns[Mine].FromFar
			&& OnFile[Index].IntoFar == Turns[Mine].IntoFar)
		{
			return Network.GetReverseTurnEnd(Index);
		}
	}
	return FGuidelineNodeId();
}

bool FRigYard::PlanTo(const URoadNetwork& Network, FGuidelineNodeId From, ERigYardFeature Feature, const FVehicle& Vehicle,
	FRoutePlan& OutPlan, FString& OutWhy, const FRoadAgent* Parked) const
{
	const FGuidelineNodeId Goal = GoalNode(Network, Feature);
	if (!From.IsSet() || !Goal.IsSet())
	{
		OutWhy = Goal.IsSet() ? TEXT("no lane end to start from")
			: TEXT("its reverse turn was not laid (see the 'Reverse turn' warning)");
		return false;
	}
	// TO THE STRAIGHT BAY, VIA THE RING'S START: its approach is eastbound along the north side,
	// and from the hammerhead the only honest way there is round the ring (south side west, west
	// side north). The alternatives the search tries first are U-turns in the yard's four dead-end
	// balloons, each of which folds the rig - four refusals spend MaxTowRetries, and the leg was
	// refused outright (2026-09-26). The via makes the ring the route, not the fifth guess.
	FGuidelineNodeId Via;
	if (Feature == ERigYardFeature::StraightBay)
	{
		FRigCourseWaypoint Ring;
		Ring.Node = StartNode;
		Ring.From = StartFrom;
		Via = ARigTestCourse::ResolveWaypoint(Network, Ring);
		if (Via == From)
		{
			Via = FGuidelineNodeId();
		}
	}

	// PlayerIssued, as the loop's legs are: a job the player gave, judged whole-route - the
	// reverse included, by VehicleFit::JudgePlan's TowReverse solve.
	FRouteQuery Query = FRouteQuery::For(ERouteErrand::PlayerIssued, From, Via.IsSet() ? Via : Goal, 0.0, ETraversalClass::GroundVehicle);
	Query.WithVehicle(Vehicle);
	// OUT OF A BAY, judged from where the tow IS: its trailer axle on the bay end (the route's
	// start), its steered axle a chain's length up the exit - where RedirectAgent seats it - not a
	// chain laid straight back behind the bay end into the grass.
	if (Parked != nullptr && Parked->TowAxles.Num() > 0)
	{
		const FGuidelineNode* Start = Network.GetGuidelineNode(From);
		const FVector2D Steered = Parked->LastMotion.Position
			+ FVector2D(FMath::Cos(Parked->LastMotion.Heading), FMath::Sin(Parked->LastMotion.Heading)) * Vehicle.Chassis.SteerAxleX;
		FTowSeed Seed;
		Seed.Axles = Parked->TowAxles;
		Seed.Heading = Parked->LastMotion.Heading;
		Seed.Speed = 0.0;
		Seed.Travelled = Start != nullptr ? FVector2D::Distance(Start->Position, Steered) : 0.0;
		Query.TowSeed = Seed;
	}
	OutPlan = RouteSearch::Find(Network, Query);
	if (OutPlan.IsValid() && Via.IsSet())
	{
		// The ring to the bay, then the two welded - and the WHOLE judged again from the live chain,
		// so the splice is admitted on the drive the agent will make, not on two halves that each
		// started from a chain laid straight.
		FRouteQuery Rest = FRouteQuery::For(ERouteErrand::PlayerIssued, Via, Goal, 0.0, ETraversalClass::GroundVehicle);
		Rest.WithVehicle(Vehicle);
		const FRoutePlan Tail = RouteSearch::Find(Network, Rest);
		if (!Tail.IsValid())
		{
			OutPlan = Tail;
		}
		else
		{
			OutPlan = RouteSearch::Splice(OutPlan, OutPlan.Steps.Num(), Tail);
			const FFitVerdict Whole = OutPlan.IsValid()
				? VehicleFit::JudgePlan(OutPlan, Vehicle, Network, Query.TowSeed.IsSet() ? &Query.TowSeed.GetValue() : nullptr)
				: FFitVerdict();
			if (OutPlan.IsValid() && !Whole.Fits())
			{
				OutWhy = FString::Printf(TEXT("the route round the ring: %s"), *Whole.Describe());
				return false;
			}
		}
	}
	if (!OutPlan.IsValid())
	{
		if (OutWhy.IsEmpty())
		{
			OutWhy = OutPlan.RejectedBy.Fits() ? FString::Printf(TEXT("no route (result %d)"), static_cast<int32>(OutPlan.Result))
				: OutPlan.RejectedBy.Describe();
		}
		return false;
	}
	return true;
}

void FRigYard::Dispatch(FRigYardRunner& Runner, ARoadNetworkActor& Actor, const FVehicle& Vehicle, const FString& Who)
{
	const URoadNetwork* Network = Actor.GetNetwork();
	if (Network == nullptr)
	{
		return;
	}
	FRigCourseWaypoint Start;
	Start.Node = StartNode;
	Start.From = StartFrom;
	const FGuidelineNodeId From = ARigTestCourse::ResolveWaypoint(*Network, Start);
	for (int32 Try = 0; Try < static_cast<int32>(ERigYardFeature::Count); ++Try)
	{
		FRoutePlan Plan;
		FString Why;
		if (PlanTo(*Network, From, Runner.Goal, Vehicle, Plan, Why) && Actor.DispatchAgent(Plan, Vehicle, ETraversalClass::GroundVehicle))
		{
			Runner.AgentId = Actor.GetTraffic()->GetNewestAgentId();
			++Runner.Dispatches;
			Runner.Elapsed = 0.0;
			Runner.LastAxles.Reset();
			UE_LOG(LogRoadBuild, Log, TEXT("RigYard: %s dispatched (agent %d) to the %s, %.0f m."),
				*Who, Runner.AgentId, FeatureName(Runner.Goal), Plan.Length / 100.0);
			return;
		}
		UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: %s %s - REFUSED: %s"), *Who, FeatureName(Runner.Goal), *Why);
		++Runner.Refusals;
		Runner.Goal = static_cast<ERigYardFeature>((static_cast<int32>(Runner.Goal) + 1) % static_cast<int32>(ERigYardFeature::Count));
	}
}

void FRigYard::Tick(ARoadNetworkActor& Actor, const TArray<FVehicle>& Vehicles, const TArray<FString>& Names, double DeltaSeconds)
{
	if (!IsBuilt())
	{
		return;
	}
	for (FRigYardRunner& Runner : Runners)
	{
		if (Vehicles.IsValidIndex(Runner.Slot) && Names.IsValidIndex(Runner.Slot))
		{
			TickRunner(Runner, Actor, Vehicles[Runner.Slot], Names[Runner.Slot], DeltaSeconds);
		}
	}
}

void FRigYard::TickRunner(FRigYardRunner& Runner, ARoadNetworkActor& Actor, const FVehicle& Vehicle, const FString& Who,
	double DeltaSeconds)
{
	if (Runner.StartDelay > 0.0)
	{
		Runner.StartDelay -= DeltaSeconds;
		return;
	}
	if (Runner.AgentId == 0)
	{
		Dispatch(Runner, Actor, Vehicle, Who);
		return;
	}
	const URoadNetwork* Network = Actor.GetNetwork();
	UGroundTraffic* Model = Actor.GetGroundTraffic();
	const FRoadAgent* Agent = Model != nullptr ? Model->FindAgent(Runner.AgentId) : nullptr;
	if (Agent == nullptr || Network == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: %s agent %d vanished; dispatching fresh."), *Who, Runner.AgentId);
		Runner.AgentId = 0;
		return;
	}
	Runner.Elapsed += DeltaSeconds;

	// THE CHAIN NEVER JUMPS: no axle moves further in a tick than the cab could drive in one,
	// plus 2 uu for the chain's own sub-steps - the handover check, measured in play.
	const double Allowed = Vehicle.Chassis.Ground.Taxi.SpeedCap * DeltaSeconds + 2.0;
	if (Runner.LastAxles.Num() == Agent->TowAxles.Num())
	{
		for (int32 Axle = 0; Axle < Agent->TowAxles.Num(); ++Axle)
		{
			Runner.WorstAxleJump = FMath::Max(Runner.WorstAxleJump,
				FVector2D::Distance(Agent->TowAxles[Axle], Runner.LastAxles[Axle]) - Allowed);
		}
	}
	Runner.LastAxles = Agent->TowAxles;

	if (Agent->Phase == EAgentPhase::Reversing && !Runner.bSawReversing)
	{
		Runner.bSawReversing = true;
		for (const TowReverse::FSample& Sample : Agent->TowReverse.Samples)
		{
			Runner.LegWorstHitchDegrees = FMath::Max(Runner.LegWorstHitchDegrees, FMath::RadiansToDegrees(FMath::Abs(Sample.HitchRadians)));
		}
		UE_LOG(LogRoadBuild, Log, TEXT("RigYard: %s %s - armed, worst hitch %.0f deg."),
			*Who, FeatureName(Runner.Goal), Runner.LegWorstHitchDegrees);
	}

	if (Agent->GetJackknifedLink() != INDEX_NONE)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: %s %s - jack-knifed; retired, dispatching fresh."), *Who, FeatureName(Runner.Goal));
		++Runner.Jackknifes;
		Actor.GetTraffic()->RetireAgent(Runner.AgentId);
		Runner.AgentId = 0;
		Runner.bSawReversing = false;
		Runner.LegWorstHitchDegrees = 0.0;
		return;
	}

	if (Agent->Phase == EAgentPhase::Parked)
	{
		// IN THE BAY: a yard leg's route ends with its reverse, so parking IS arriving. Measured
		// where it matters - the trailer's rearmost axle against the bay end, the trailer facing
		// back out along the bay (away from the way it backed in).
		FRigYardReverse Result;
		Result.Feature = Runner.Goal;
		Result.bArmed = Runner.bSawReversing;
		Result.WorstHitchDegrees = Runner.LegWorstHitchDegrees;
		const FGuidelineNode* Goal = Network->GetGuidelineNode(GoalNode(*Network, Runner.Goal));
		const TArray<FVector2D>& Line = Agent->TowReverse.Plan.Polyline;
		if (Goal != nullptr && Agent->TowAxles.Num() > 0 && Line.Num() >= 2)
		{
			Result.EndError = FVector2D::Distance(Agent->TowAxles.Last(), Goal->Position);
			const FVector2D Hitch = Agent->TowAxles.Num() >= 2 ? Agent->TowAxles[Agent->TowAxles.Num() - 2]
				: Agent->LastMotion.Position + FVector2D(FMath::Cos(Agent->LastMotion.Heading), FMath::Sin(Agent->LastMotion.Heading)) * Vehicle.Tow[0].HitchX;
			const FVector2D Facing = (Hitch - Agent->TowAxles.Last()).GetSafeNormal();
			const FVector2D Backed = (Line.Last() - Line[Line.Num() - 2]).GetSafeNormal();
			Result.EndHeadingDegrees = FMath::RadiansToDegrees(RoadGeom::AngleBetween(Facing, -Backed));
		}
		Runner.Completed.Add(Result);
		const double EndHitch = Agent->TowReverse.Last() != nullptr
			? FMath::RadiansToDegrees(Agent->TowReverse.Last()->HitchRadians) : 0.0;
		UE_LOG(LogRoadBuild, Log, TEXT("RigYard: %s %s - in%s, trailer axle %.0f uu / %.1f deg off the bay end, hitch %.1f deg at rest, worst hitch %.0f deg, %.0f s."),
			*Who, FeatureName(Runner.Goal), Result.bArmed ? TEXT("") : TEXT(" WITHOUT REVERSING"), Result.EndError,
			Result.EndHeadingDegrees, EndHitch, Result.WorstHitchDegrees, Runner.Elapsed);

		// ON TO THE NEXT BAY, the same agent redirected from rest - its chain kept (RedirectAgent
		// re-seats the follower and leaves TowAxles, as the loop's fallback relies on).
		const FGuidelineNodeId Here = GoalNode(*Network, Runner.Goal);
		Runner.Goal = static_cast<ERigYardFeature>((static_cast<int32>(Runner.Goal) + 1) % static_cast<int32>(ERigYardFeature::Count));
		Runner.bSawReversing = false;
		Runner.LegWorstHitchDegrees = 0.0;
		Runner.Elapsed = 0.0;
		FRoutePlan Plan;
		FString Why;
		if (!PlanTo(*Network, Here, Runner.Goal, Vehicle, Plan, Why, Agent) || !Actor.GetTraffic()->RedirectAgent(Runner.AgentId, Network, Plan))
		{
			UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: %s %s - REFUSED from the last bay: %s; retired, dispatching fresh."),
				*Who, FeatureName(Runner.Goal), Why.IsEmpty() ? TEXT("the redirect was refused") : *Why);
			++Runner.Refusals;
			Actor.GetTraffic()->RetireAgent(Runner.AgentId);
			Runner.AgentId = 0;
		}
		return;
	}

	if (Runner.Elapsed > LegTimeout)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigYard: %s %s - stuck for %.0f s (phase %d); retired, dispatching fresh."),
			*Who, FeatureName(Runner.Goal), Runner.Elapsed, static_cast<int32>(Agent->Phase));
		++Runner.Stuck;
		Actor.GetTraffic()->RetireAgent(Runner.AgentId);
		Runner.AgentId = 0;
		Runner.bSawReversing = false;
		Runner.LegWorstHitchDegrees = 0.0;
	}
}
