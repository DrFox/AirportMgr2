#include "RigTestCourse.h"

#include "Content/AirsideSettings.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/VehicleFit.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "RoadBuildLog.h"
#include "Tool/RoadEditTarget.h"

// NAMED, NOT ANONYMOUS: this module is a unity build, and two files' anonymous helpers of one
// name collide once they share a blob.
namespace RigCourse
{
	// THE LAYOUT, uu (1 uu = 1 cm). One LANE per tier, drawn in lane-local coordinates entering
	// at the origin heading +X (UE: +Y is to the driver's RIGHT):
	//
	//                 P3 -------------- P4   (exit, heading +X)
	//                 |
	//                 P2 ---- S  )  (T junction; stem east to a dead end, balloon past S)
	//                 |
	//   P0 ---------- P1
	//   (entry)   80 m straight
	//
	// P1 turns +X to +Y: a RIGHT 90. P3 turns +Y to +X: a LEFT 90. The labels are computed
	// from the geometry (TurnFeature), never typed, so this sketch cannot disagree with them.
	constexpr double LaneStraight = 8000.0;   // P0 -> P1, the 80 m straight
	constexpr double CornerRise   = 3000.0;   // P1 -> P2
	constexpr double StemLength   = 3000.0;   // P2 -> S, the T's stem and the dead end
	constexpr double UpperRise    = 3000.0;   // P2 -> P3
	constexpr double ExitRun      = 6000.0;   // P3 -> P4
	constexpr double LaneLength   = LaneStraight + ExitRun;   // 140 m
	constexpr double LaneHeight   = CornerRise + UpperRise;   // 60 m

	// 60 m of CLEAR GROUND between lanes (the brief's figure), on top of a lane's own height:
	// a lane is not a line, and the dead end's U-turn balloon reaches ~3.9x the bowser's lock
	// (~27 m) past S (UTurnGeom::HeightFactor), which stays inside the lane's own band.
	constexpr double TierGap   = 6000.0;
	constexpr double TierPitch = LaneHeight + TierGap;

	// Connectors: WIDE in the middle, so a join refuses as little as it can, but the 20 m stub
	// that meets a lane is laid in THAT LANE'S tier, so the width changes at a connector CORNER
	// and never at a straight-through node. Measured 2026-09-25: with the change at the lane's
	// entry node, the rig took 55 s over the 69 m Narrow straight against 14 s on Wide, and the
	// utility 28 s (why is untraced; a lane offset step at the join is the suspect). Odd tiers are the lane ROTATED 180 degrees
	// about its centre, so every lane is driven with the same turns in the same order (a
	// serpentine), joined by an east link, a west link, and a return road round the outside.
	constexpr double EastLinkX   = LaneLength + 2000.0;
	constexpr double WestLinkX   = -3000.0;
	constexpr double ReturnEastX = LaneLength + 6000.0;
	constexpr double ReturnWestX = -5000.0;
	constexpr double ReturnSouthY = -4000.0;

	constexpr int32 TierCount = 3;
	constexpr int32 ConnectorTier = 2;   // Wide
	const TCHAR* const TierNames[TierCount] = { TEXT("Narrow"), TEXT("Standard"), TEXT("Wide") };

	/** A lane-local point in world XY for Tier. */
	FVector2D LanePoint(int32 Tier, double X, double Y)
	{
		const bool bRotated = (Tier % 2) == 1;
		const FVector2D Local = bRotated ? FVector2D(LaneLength - X, LaneHeight - Y) : FVector2D(X, Y);
		return Local + FVector2D(0.0, TierPitch * Tier);
	}

	/** Right or left from the turn In -> Out. UE is left-handed seen from above: +X to +Y is right. */
	ERigCourseFeature TurnFeature(const FVector2D& In, const FVector2D& Out)
	{
		return FVector2D::CrossProduct(In, Out) > 0.0 ? ERigCourseFeature::Right90 : ERigCourseFeature::Left90;
	}

	const TCHAR* FeatureText(ERigCourseFeature Feature)
	{
		switch (Feature)
		{
		case ERigCourseFeature::Straight:    return TEXT("straight 80 m");
		case ERigCourseFeature::Right90:     return TEXT("right 90");
		case ERigCourseFeature::Left90:      return TEXT("left 90");
		case ERigCourseFeature::TeeJunction: return TEXT("T junction");
		case ERigCourseFeature::DeadEnd:     return TEXT("dead end U-turn");
		default:                             return TEXT("connector");
		}
	}
}

ARigTestCourse::ARigTestCourse()
{
	PrimaryActorTick.bCanEverTick = true;
	// The vehicles are NOT resolved here: a constructor also builds the CDO at module load,
	// and the content resolvers read the content set - see BuildCourse.
}

void ARigTestCourse::BeginPlay()
{
	Super::BeginPlay();
	if (ARoadNetworkActor* Actor = ResolveNetworkActor())
	{
		BuildCourse(*Actor);
	}
	else
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: no ARoadNetworkActor in the level - nothing laid, nothing driven."));
	}
}

ARoadNetworkActor* ARigTestCourse::ResolveNetworkActor()
{
	if (NetworkActor == nullptr)
	{
		NetworkActor = ARoadNetworkActor::Find(GetWorld());
	}
	return NetworkActor;
}

void ARigTestCourse::BuildCourse(IRoadEditTarget& Target)
{
	using namespace RigCourse;

	Waypoints.Reset();
	LoopResults.Reset();
	LastLoopResults.Reset();
	RefusalLabels.Reset();
	Leg = 0;
	VehicleSlot = 0;
	ActiveAgentId = 0;

	Vehicles = { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() };
	VehicleNames = { TEXT("rig"), TEXT("utility+trailer") };

	const int32 Widths = Target.GetWidthCount(ERoadKind::ServiceRoad);
	if (Widths < TierCount)
	{
		// Laid anyway - ResolveWidthProfile clamps - but the tier names would then lie.
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: the content set has %d service-road tiers, the course expects %d."),
			Widths, TierCount);
	}

	auto Place = [&Target](const FVector2D& Where) -> int32
	{
		return Target.PlaceNode(Where);
	};
	auto Id = [&Target](int32 Index)
	{
		FRoadNodeId Out;
		Target.MakeLiveNodeId(Index, Out);
		return Out;
	};
	int32 Laid = 0, Failed = 0;
	auto Connect = [&Target, &Laid, &Failed](int32 A, int32 B, int32 Tier)
	{
		if (Target.ConnectNodes(A, B, ERoadKind::ServiceRoad, Tier)) { ++Laid; }
		else
		{
			++Failed;
			UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: ConnectNodes(%d, %d, tier %d) refused."), A, B, Tier);
		}
	};

	struct FLaneNodes { int32 P0, P1, P2, S, P3, P4; };
	FLaneNodes Lanes[TierCount];
	for (int32 Tier = 0; Tier < TierCount; ++Tier)
	{
		FLaneNodes& L = Lanes[Tier];
		L.P0 = Place(LanePoint(Tier, 0.0, 0.0));
		L.P1 = Place(LanePoint(Tier, LaneStraight, 0.0));
		L.P2 = Place(LanePoint(Tier, LaneStraight, CornerRise));
		L.S  = Place(LanePoint(Tier, LaneStraight + StemLength, CornerRise));
		L.P3 = Place(LanePoint(Tier, LaneStraight, LaneHeight));
		L.P4 = Place(LanePoint(Tier, LaneLength, LaneHeight));
		Connect(L.P0, L.P1, Tier);
		Connect(L.P1, L.P2, Tier);
		Connect(L.P2, L.S, Tier);
		Connect(L.P2, L.P3, Tier);
		Connect(L.P3, L.P4, Tier);
	}

	// East link: tier 0's exit up to tier 1's entry. West link: tier 1's exit up to tier 2's.
	const int32 E0 = Place(FVector2D(EastLinkX, LaneHeight));
	const int32 E1 = Place(FVector2D(EastLinkX, TierPitch + LaneHeight));
	Connect(Lanes[0].P4, E0, 0);
	Connect(E0, E1, ConnectorTier);
	Connect(E1, Lanes[1].P0, 1);
	const int32 W0 = Place(FVector2D(WestLinkX, TierPitch));
	const int32 W1 = Place(FVector2D(WestLinkX, 2.0 * TierPitch));
	Connect(Lanes[1].P4, W0, 1);
	Connect(W0, W1, ConnectorTier);
	Connect(W1, Lanes[2].P0, 2);
	// Return: tier 2's exit round the outside, back to tier 0's entry.
	const int32 R0 = Place(FVector2D(ReturnEastX, 2.0 * TierPitch + LaneHeight));
	const int32 R1 = Place(FVector2D(ReturnEastX, ReturnSouthY));
	const int32 R2 = Place(FVector2D(ReturnWestX, ReturnSouthY));
	const int32 R3 = Place(FVector2D(ReturnWestX, 0.0));
	Connect(Lanes[2].P4, R0, 2);
	Connect(R0, R1, ConnectorTier);
	Connect(R1, R2, ConnectorTier);
	Connect(R2, R3, ConnectorTier);
	Connect(R3, Lanes[0].P0, 0);

	const int32 EntryFrom[TierCount] = { R3, E1, W1 };
	for (int32 Tier = 0; Tier < TierCount; ++Tier)
	{
		const FLaneNodes& L = Lanes[Tier];
		const FString Name = TierNames[Tier];
		auto Add = [this, &Id, &Name, Tier](int32 Node, int32 From, ERigCourseFeature Feature, const TCHAR* Detail)
		{
			FRigCourseWaypoint& W = Waypoints.AddDefaulted_GetRef();
			W.Node = Id(Node);
			W.From = Id(From);
			W.Tier = Tier;
			W.Feature = Feature;
			W.Label = FString::Printf(TEXT("%s, %s"), *Name,
				Detail != nullptr ? Detail : RigCourse::FeatureText(Feature));
		};
		const FVector2D P0 = LanePoint(Tier, 0.0, 0.0);
		const FVector2D P1 = LanePoint(Tier, LaneStraight, 0.0);
		const FVector2D P2 = LanePoint(Tier, LaneStraight, CornerRise);
		const FVector2D P3 = LanePoint(Tier, LaneStraight, LaneHeight);
		const FVector2D P4 = LanePoint(Tier, LaneLength, LaneHeight);

		Add(L.P0, EntryFrom[Tier], ERigCourseFeature::Connector, TEXT("connector in"));
		Add(L.P1, L.P0, ERigCourseFeature::Straight, nullptr);
		Add(L.P2, L.P1, TurnFeature(P1 - P0, P2 - P1), nullptr);          // the corner at P1
		Add(L.S,  L.P2, ERigCourseFeature::TeeJunction, TEXT("T junction into the stem"));
		Add(L.P2, L.S,  ERigCourseFeature::DeadEnd, nullptr);
		Add(L.P3, L.P2, ERigCourseFeature::TeeJunction, TEXT("T junction out of the stem"));
		Add(L.P4, L.P3, TurnFeature(P3 - P2, P4 - P3), nullptr);          // the corner at P3
	}

	LoopResults.SetNum(Waypoints.Num() * Vehicles.Num());
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: course laid - %d segment(s) (%d refused), %d waypoint(s), %d feature(s), %d tier(s) of %d."),
		Laid, Failed, Waypoints.Num(), FeatureCountForTest(), TierCount, Widths);
}

int32 ARigTestCourse::FeatureCountForTest() const
{
	TSet<TPair<int32, uint8>> Seen;
	for (const FRigCourseWaypoint& W : Waypoints)
	{
		if (W.Feature != ERigCourseFeature::Connector)
		{
			Seen.Add(TPair<int32, uint8>(W.Tier, static_cast<uint8>(W.Feature)));
		}
	}
	return Seen.Num();
}

FGuidelineNodeId ARigTestCourse::ResolveWaypoint(const URoadNetwork& Network, const FRigCourseWaypoint& Waypoint)
{
	const FRoadNode* Node = Network.GetNode(Waypoint.Node);
	if (Node == nullptr || Network.GetNode(Waypoint.From) == nullptr)
	{
		return FGuidelineNodeId();
	}
	FRoadSegmentId Along;
	for (const FRoadSegmentId& SegId : Node->Incident)
	{
		const FRoadSegment* Seg = Network.GetSegment(SegId);
		if (Seg != nullptr && ((Seg->A == Waypoint.From && Seg->B == Waypoint.Node)
			|| (Seg->B == Waypoint.From && Seg->A == Waypoint.Node)))
		{
			Along = SegId;
			break;
		}
	}
	if (!Along.IsSet())
	{
		return FGuidelineNodeId();
	}

	// The lane of that segment travelling TOWARDS the node: of its two ends, the one it drives
	// INTO, and nearer the node than the one it leaves. The other lane arrives at From.
	FGuidelineNodeId Best;
	double BestDistance = TNumericLimits<double>::Max();
	const TArray<FGuidelineEdge>& Edges = Network.GetGuidelineEdges();
	for (const FGuidelineEdge& Edge : Edges)
	{
		if (!Edge.bAlive || Edge.DerivedFrom != Along || Edge.Direction == EGuidelineDir::Bidirectional)
		{
			continue;
		}
		const bool bAToB = Edge.Direction == EGuidelineDir::AToB;
		const FGuidelineNode* Into = Network.GetGuidelineNode(bAToB ? Edge.B : Edge.A);
		const FGuidelineNode* OutOf = Network.GetGuidelineNode(bAToB ? Edge.A : Edge.B);
		if (Into == nullptr || OutOf == nullptr)
		{
			continue;
		}
		const double Distance = FVector2D::Distance(Into->Position, Node->Position);
		if (Distance < FVector2D::Distance(OutOf->Position, Node->Position) && Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = bAToB ? Edge.B : Edge.A;
		}
	}
	return Best;
}

double ARigTestCourse::LegTimeoutSeconds(const FVehicle& Vehicle, double Length)
{
	const FGroundRegime& Taxi = Vehicle.Chassis.Ground.Taxi;
	const double Cap = FMath::Max(Taxi.SpeedCap, 1.0);
	const double Ideal = Length / Cap + Cap / FMath::Max(Taxi.Accel, 1.0) + Cap / FMath::Max(Taxi.Decel, 1.0);
	return LegTimeoutFactor * Ideal;
}

void ARigTestCourse::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	TickDriver(DeltaSeconds);
	DrawRefusals();
}

void ARigTestCourse::TickDriver(double DeltaSeconds)
{
	if (Waypoints.Num() < 2 || Vehicles.Num() == 0 || ResolveNetworkActor() == nullptr)
	{
		return;
	}
	if (ActiveAgentId == 0)
	{
		StartAttempt();
		return;
	}

	const FRigCourseWaypoint& To = Waypoints[(Leg + 1) % Waypoints.Num()];
	const FString& Who = VehicleNames[VehicleSlot];
	UGroundTraffic* Model = NetworkActor->GetGroundTraffic();
	const FRoadAgent* Agent = Model != nullptr ? Model->FindAgent(ActiveAgentId) : nullptr;
	if (Agent == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) %s vanished before it arrived."), Leg, *To.Label, *Who);
		ActiveAgentId = 0;
		FinishAttempt(ERigLegOutcome::Stuck, TEXT("vanished"));
		return;
	}

	// The course has no stands and no reverse legs, so a Reversing agent means the layout is
	// wrong - and the Reversing phase does not step the tow chain, so it would drive a lie.
	if (Agent->Phase == EAgentPhase::Reversing && !bActiveReversed)
	{
		bActiveReversed = true;
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) %s entered Reversing - the course has no reverse legs."),
			Leg, *To.Label, *Who);
	}

	// A JACK-KNIFED AGENT HOLDS FOR EVER: only StartDrive clears the link, and every course
	// agent is fresh. Retire it through the normal path and move on.
	if (Agent->GetJackknifedLink() != INDEX_NONE)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) %s jack-knifed at link %d"),
			Leg, *To.Label, *Who, Agent->GetJackknifedLink());
		NetworkActor->GetTraffic()->RetireAgent(ActiveAgentId);
		ActiveAgentId = 0;
		FinishAttempt(ERigLegOutcome::Jackknifed);
		return;
	}
	if (Agent->Phase == EAgentPhase::Parked)
	{
		const FVector2D End = Agent->GroundPosition();
		const double Left = Agent->PlanInProgress().Length - Agent->DistanceAlongPlan();
		UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: leg %d (%s) %s arrived in %.1f s, %.0f uu of its line left."),
			Leg, *To.Label, *Who, ActiveElapsed, Left);
		FRigLegResult& Result = LoopResults[Leg * Vehicles.Num() + VehicleSlot];
		Result.EndPosition = End;
		Result.DistanceLeft = Left;
		NetworkActor->GetTraffic()->RetireAgent(ActiveAgentId);
		ActiveAgentId = 0;
		FinishAttempt(ERigLegOutcome::Driven);
		return;
	}

	ActiveElapsed += DeltaSeconds;
	if (ActiveElapsed > ActiveTimeout)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) %s stuck: not arrived after %.0f s (allowance %.0f s); skipped."),
			Leg, *To.Label, *Who, ActiveElapsed, ActiveTimeout);
		NetworkActor->GetTraffic()->RetireAgent(ActiveAgentId);
		ActiveAgentId = 0;
		FinishAttempt(ERigLegOutcome::Stuck);
	}
}

void ARigTestCourse::StartAttempt()
{
	const URoadNetwork* Network = NetworkActor->GetNetwork();
	if (Network == nullptr)
	{
		return;
	}
	const FRigCourseWaypoint& From = Waypoints[Leg];
	const FRigCourseWaypoint& To = Waypoints[(Leg + 1) % Waypoints.Num()];
	const FVehicle& Vehicle = Vehicles[VehicleSlot];
	const FString& Who = VehicleNames[VehicleSlot];
	const int32 Key = Leg * Vehicles.Num() + VehicleSlot;

	const FGuidelineNodeId Start = ResolveWaypoint(*Network, From);
	const FGuidelineNodeId Goal = ResolveWaypoint(*Network, To);
	FRoutePlan Plan;
	FString Reason;
	if (!Start.IsSet() || !Goal.IsSet())
	{
		Reason = FString::Printf(TEXT("%s: no lane end at %s"), *Who, Start.IsSet() ? TEXT("the goal") : TEXT("the start"));
	}
	else
	{
		// PlayerIssued, NOT VehicleToJob: the course is a tool asking for a route directly, and
		// VehicleToJob requires the occupancy table - a congestion cost that, with one vehicle
		// out at a time, could only ever be zero. The gate that matters is WithVehicle.
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::PlayerIssued, Start, Goal, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Vehicle);
		Plan = RouteSearch::Find(*Network, Query);
		if (!Plan.IsValid())
		{
			Reason = DescribeRefusal(*Network, Plan, Vehicle);
		}
	}

	const FGuidelineNode* StartNode = Start.IsSet() ? Network->GetGuidelineNode(Start) : nullptr;
	const FVector2D StartAt = StartNode != nullptr ? StartNode->Position
		: (Network->GetNode(From.Node) != nullptr ? Network->GetNode(From.Node)->Position : FVector2D::ZeroVector);
	if (!Reason.IsEmpty())
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) refused: %s"), Leg, *To.Label, *Reason);
		const double Z = NetworkActor->GetActorLocation().Z + 400.0;
		RefusalLabels.Add(Key, TPair<FVector, FString>(FVector(StartAt, Z),
			FString::Printf(TEXT("leg %d (%s) refused: %s"), Leg, *To.Label, *Reason)));
		FinishAttempt(ERigLegOutcome::Refused, Reason);
		return;
	}
	RefusalLabels.Remove(Key);

	if (!NetworkActor->DispatchAgent(Plan, Vehicle, ETraversalClass::GroundVehicle))
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: leg %d (%s) %s: the dispatch was refused; skipped."), Leg, *To.Label, *Who);
		FinishAttempt(ERigLegOutcome::Stuck, TEXT("dispatch refused"));
		return;
	}
	ActiveAgentId = NetworkActor->GetTraffic()->GetNewestAgentId();
	ActiveElapsed = 0.0;
	ActiveTimeout = LegTimeoutSeconds(Vehicle, Plan.Length);
	ActiveGoal = Network->GetGuidelineNode(Goal)->Position;
	bActiveReversed = false;
	LoopResults[Key].GoalPosition = ActiveGoal;
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: leg %d (%s) %s dispatched as agent %d, %.0f m, allowance %.0f s."),
		Leg, *To.Label, *Who, ActiveAgentId, Plan.Length / 100.0, ActiveTimeout);
}

FString ARigTestCourse::DescribeRefusal(const URoadNetwork& Network, const FRoutePlan& Plan, const FVehicle& Vehicle) const
{
	const UEnum* ResultEnum = StaticEnum<ERouteResult>();
	FString Text = FString::Printf(TEXT("%s %s"), *VehicleNames[VehicleSlot],
		*ResultEnum->GetNameStringByValue(static_cast<int64>(Plan.Result)));
	if (Plan.Result != ERouteResult::TooNarrow || !Plan.RejectedEdge.IsSet())
	{
		return Text;
	}
	const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Plan.RejectedEdge);
	const FGuidelineNode* At = Edge != nullptr ? Network.GetGuidelineNode(Edge->A) : nullptr;
	if (Edge == nullptr || At == nullptr)
	{
		return Text;
	}
	// THE ROUTER'S OWN RULE, asked again for its figures (VehicleFit::Judge is Fits' body), so
	// the numbers printed are the ones the refusal was made on.
	const FFitVerdict Verdict = VehicleFit::Judge(*Edge, Vehicle, Network);
	Text += FString::Printf(TEXT(" at node %d (%.0f, %.0f)"), Edge->A.Index, At->Position.X, At->Position.Y);
	if (!Verdict.Fits())
	{
		Text += TEXT(", ") + Verdict.Describe();
	}
	return Text;
}

void ARigTestCourse::FinishAttempt(ERigLegOutcome Outcome, const FString& Reason)
{
	FRigLegResult& Result = LoopResults[Leg * Vehicles.Num() + VehicleSlot];
	Result.Outcome = Outcome;
	Result.Reason = Reason;
	Result.bReversed = bActiveReversed;
	bActiveReversed = false;

	if (++VehicleSlot < Vehicles.Num())
	{
		return;
	}
	VehicleSlot = 0;
	if (++Leg < Waypoints.Num())
	{
		return;
	}
	Leg = 0;
	EndLoop();
}

void ARigTestCourse::EndLoop()
{
	++LoopsCompleted;
	TArray<int32> Driven;
	Driven.Init(0, Vehicles.Num());
	TArray<FString> Refused;
	for (int32 V = 0; V < Vehicles.Num(); ++V)
	{
		TArray<FString> Mine;
		for (int32 L = 0; L < Waypoints.Num(); ++L)
		{
			const FRigLegResult& R = LoopResults[L * Vehicles.Num() + V];
			const FString& Label = Waypoints[(L + 1) % Waypoints.Num()].Label;
			switch (R.Outcome)
			{
			case ERigLegOutcome::Driven:     ++Driven[V]; break;
			case ERigLegOutcome::Refused:    Mine.Add(FString::Printf(TEXT("%d (%s)"), L, *Label)); break;
			case ERigLegOutcome::Jackknifed: Mine.Add(FString::Printf(TEXT("%d (%s, jack-knifed)"), L, *Label)); break;
			case ERigLegOutcome::Stuck:      Mine.Add(FString::Printf(TEXT("%d (%s, stuck)"), L, *Label)); break;
			default: break;
			}
		}
		Refused.Add(FString::Printf(TEXT("%s: %s"), *VehicleNames[V],
			Mine.Num() > 0 ? *FString::Join(Mine, TEXT(", ")) : TEXT("none")));
	}
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: loop %d - rig %d/%d, utility+trailer %d/%d legs driven; refused: %s"),
		LoopsCompleted, Driven[0], Waypoints.Num(), Vehicles.Num() > 1 ? Driven[1] : 0, Waypoints.Num(),
		*FString::Join(Refused, TEXT("; ")));

	LastLoopResults = LoopResults;
	LoopResults.Reset();
	LoopResults.SetNum(Waypoints.Num() * Vehicles.Num());
}

void ARigTestCourse::DrawRefusals() const
{
#if ENABLE_DRAW_DEBUG
	// PIE only: a headless world has no viewport and no HUD to draw on, and the test runs there.
	const UWorld* World = GetWorld();
	if (World == nullptr || World->GetGameViewport() == nullptr)
	{
		return;
	}
	for (const TPair<int32, TPair<FVector, FString>>& Label : RefusalLabels)
	{
		// Zero duration, redrawn every tick: the label lasts exactly as long as the refusal.
		DrawDebugString(World, Label.Value.Key, Label.Value.Value, nullptr, FColor::Red, 0.f, true, 1.2f);
	}
#endif
}
