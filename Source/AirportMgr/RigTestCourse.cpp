#include "RigTestCourse.h"

#include "Content/AirsideSettings.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/SpeedProfile.h"
#include "Model/RouteSearch.h"
#include "Model/VehicleFit.h"
#include "Present/AirsideTraffic.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "RoadBuildLog.h"
#include "Solve/GuidelineGeom.h"
#include "Tool/RoadEditTarget.h"

/** A lane-local point in world XY for Tier. */
FVector2D FRigCourseLayout::LanePoint(int32 Tier, double X, double Y)
{
	const bool bRotated = (Tier % 2) == 1;
	const FVector2D Local = bRotated ? FVector2D(LaneLength - X, LaneHeight - Y) : FVector2D(X, Y);
	return Local + FVector2D(0.0, TierPitch * Tier);
}

/** Right or left from the turn In -> Out. UE is left-handed seen from above: +X to +Y is right. */
ERigCourseFeature FRigCourseLayout::TurnFeature(const FVector2D& In, const FVector2D& Out)
{
	return FVector2D::CrossProduct(In, Out) > 0.0 ? ERigCourseFeature::Right90 : ERigCourseFeature::Left90;
}

const TCHAR* FRigCourseLayout::FeatureText(ERigCourseFeature Feature)
{
	switch (Feature)
	{
	case ERigCourseFeature::Straight:    return TEXT("straight 80 m");
	case ERigCourseFeature::Right90:     return TEXT("right 90");
	case ERigCourseFeature::Left90:      return TEXT("left 90");
	case ERigCourseFeature::TeeJunction: return TEXT("T junction");
	case ERigCourseFeature::DeadEnd:     return TEXT("dead end U-turn");
	case ERigCourseFeature::WidthStep:   return TEXT("Narrow->Wide mid-straight");
	default:                             return TEXT("connector");
	}
}

const TCHAR* FRigCourseLayout::TierName(int32 Tier)
{
	static const TCHAR* const Names[TierCount] = { TEXT("Narrow"), TEXT("Standard"), TEXT("Wide") };
	return Names[FMath::Clamp(Tier, 0, TierCount - 1)];
}

FRigCourseLayoutResult FRigCourseLayout::Lay(IRoadEditTarget& Target)
{
	FRigCourseLayoutResult Result;
	Result.BoxMin = FVector2D(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	Result.BoxMax = FVector2D(TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest());

	// EVERY PLACED NODE, TRACKED HERE (#301): the bounding box ToJson exports is measured off
	// what Lay() actually places, never a hand re-derivation of the constants above - the shape
	// Tools/Python/build_rig_test_level.py's own retyped comment used to be.
	auto Place = [&Target, &Result](const FVector2D& Where) -> int32
	{
		Result.BoxMin.X = FMath::Min(Result.BoxMin.X, Where.X);
		Result.BoxMin.Y = FMath::Min(Result.BoxMin.Y, Where.Y);
		Result.BoxMax.X = FMath::Max(Result.BoxMax.X, Where.X);
		Result.BoxMax.Y = FMath::Max(Result.BoxMax.Y, Where.Y);
		return Target.PlaceNode(Where);
	};
	auto Id = [&Target](int32 Index)
	{
		FRoadNodeId Out;
		Target.MakeLiveNodeId(Index, Out);
		return Out;
	};
	auto Connect = [&Target, &Result](int32 A, int32 B, int32 Tier)
	{
		if (Target.ConnectNodes(A, B, ERoadKind::ServiceRoad, Tier)) { ++Result.SegmentsLaid; }
		else
		{
			++Result.SegmentsRefused;
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
	const int32 Step = Place(FVector2D(WidthStepX, ReturnSouthY));
	const int32 R2 = Place(FVector2D(ReturnWestX, ReturnSouthY));
	const int32 R3 = Place(FVector2D(ReturnWestX, 0.0));
	Connect(Lanes[2].P4, R0, 2);
	Connect(R0, R1, ConnectorTier);
	Connect(R1, Step, WidthStepFrom);
	Connect(Step, R2, WidthStepTo);
	Connect(R2, R3, ConnectorTier);
	Connect(R3, Lanes[0].P0, 0);

	const int32 EntryFrom[TierCount] = { R3, E1, W1 };
	// The first node after each lane's exit, on the road out: the reverse runner arrives at P4
	// from here.
	const int32 ExitNext[TierCount] = { E0, W0, R0 };
	for (int32 Tier = 0; Tier < TierCount; ++Tier)
	{
		const FLaneNodes& L = Lanes[Tier];
		const TCHAR* Name = TierName(Tier);
		auto Add = [&Result, &Id, Name, Tier](int32 Node, int32 From, int32 Next, ERigCourseFeature Feature, const TCHAR* Detail)
		{
			FRigCourseWaypoint& W = Result.Waypoints.AddDefaulted_GetRef();
			W.Node = Id(Node);
			W.From = Id(From);
			W.Next = Id(Next);
			W.Tier = Tier;
			W.Feature = Feature;
			W.Label = FString::Printf(TEXT("%s, %s"), Name,
				Detail != nullptr ? Detail : FeatureText(Feature));
		};
		const FVector2D P0 = LanePoint(Tier, 0.0, 0.0);
		const FVector2D P1 = LanePoint(Tier, LaneStraight, 0.0);
		const FVector2D P2 = LanePoint(Tier, LaneStraight, CornerRise);
		const FVector2D P3 = LanePoint(Tier, LaneStraight, LaneHeight);
		const FVector2D P4 = LanePoint(Tier, LaneLength, LaneHeight);

		// Next: the leg from S leaves it back towards P2, through the dead end's balloon - so S
		// arrived along (S, P2) is the lane end BEFORE the balloon, whichever way it is run.
		Add(L.P0, EntryFrom[Tier], L.P1, ERigCourseFeature::Connector, TEXT("connector in"));
		Add(L.P1, L.P0, L.P2, ERigCourseFeature::Straight, nullptr);
		Add(L.P2, L.P1, L.S,  TurnFeature(P1 - P0, P2 - P1), nullptr);          // the corner at P1
		Add(L.S,  L.P2, L.P2, ERigCourseFeature::TeeJunction, TEXT("T junction into the stem"));
		Add(L.P2, L.S,  L.P3, ERigCourseFeature::DeadEnd, nullptr);
		Add(L.P3, L.P2, L.P4, ERigCourseFeature::TeeJunction, TEXT("T junction out of the stem"));
		Add(L.P4, L.P3, ExitNext[Tier], TurnFeature(P3 - P2, P4 - P3), nullptr);          // the corner at P3
	}
	// The return: to the width step's node, then ACROSS it to the west corner - so the one leg
	// that drives the taper is the one labelled with it, and no other leg does.
	auto AddReturn = [&Result, &Id](int32 Node, int32 From, int32 Next, ERigCourseFeature Feature, const TCHAR* Label)
	{
		FRigCourseWaypoint& W = Result.Waypoints.AddDefaulted_GetRef();
		W.Node = Id(Node);
		W.From = Id(From);
		W.Next = Id(Next);
		W.Tier = INDEX_NONE;
		W.Feature = Feature;
		W.Label = Label;
	};
	AddReturn(Step, R1, R2, ERigCourseFeature::Connector, TEXT("return, to the width step"));
	AddReturn(R2, Step, R3, ERigCourseFeature::WidthStep, FeatureText(ERigCourseFeature::WidthStep));

	return Result;
}

FString FRigCourseLayout::ToJson(const FRigCourseLayoutResult& Result)
{
	// EVERY FIGURE build_rig_test_level.py used to retype by hand off a code comment (#301),
	// plus the bounding box Lay() actually measured - so a Python re-run always reads THIS
	// build's numbers, never a stale sketch. Flat and hand-formatted: this project has no JSON
	// writer dependency to reach for, and the shape is fixed and small.
	return FString::Printf(
		TEXT("{")
		TEXT("\"LaneStraight\":%.1f,\"CornerRise\":%.1f,\"StemLength\":%.1f,\"UpperRise\":%.1f,\"ExitRun\":%.1f,")
		TEXT("\"LaneLength\":%.1f,\"LaneHeight\":%.1f,\"TierGap\":%.1f,\"TierPitch\":%.1f,")
		TEXT("\"EastLinkX\":%.1f,\"WestLinkX\":%.1f,\"ReturnEastX\":%.1f,\"ReturnWestX\":%.1f,\"ReturnSouthY\":%.1f,")
		TEXT("\"WidthStepX\":%.1f,\"WidthStepFrom\":%d,\"WidthStepTo\":%d,")
		TEXT("\"TierCount\":%d,\"ConnectorTier\":%d,")
		TEXT("\"BoxMinX\":%.1f,\"BoxMaxX\":%.1f,\"BoxMinY\":%.1f,\"BoxMaxY\":%.1f,")
		TEXT("\"SegmentsLaid\":%d,\"SegmentsRefused\":%d,\"Waypoints\":%d")
		TEXT("}"),
		LaneStraight, CornerRise, StemLength, UpperRise, ExitRun,
		LaneLength, LaneHeight, TierGap, TierPitch,
		EastLinkX, WestLinkX, ReturnEastX, ReturnWestX, ReturnSouthY,
		WidthStepX, WidthStepFrom, WidthStepTo,
		TierCount, ConnectorTier,
		Result.BoxMin.X, Result.BoxMax.X, Result.BoxMin.Y, Result.BoxMax.Y,
		Result.SegmentsLaid, Result.SegmentsRefused, Result.Waypoints.Num());
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
	Waypoints.Reset();
	RefusalLabels.Reset();

	Vehicles = { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() };
	VehicleNames = { TEXT("rig"), TEXT("utility") };

	const int32 Widths = Target.GetWidthCount(ERoadKind::ServiceRoad);
	if (Widths < FRigCourseLayout::TierCount)
	{
		// Laid anyway - ResolveWidthProfile clamps - but the tier names would then lie.
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: the content set has %d service-road tiers, the course expects %d."),
			Widths, FRigCourseLayout::TierCount);
	}

	// THE GEOMETRY ITSELF (#301): FRigCourseLayout::Lay is a plain function, world-free but for
	// Target - this actor's own job is the vehicles, the runners and the bookkeeping below.
	FRigCourseLayoutResult Result = FRigCourseLayout::Lay(Target);
	Waypoints = MoveTemp(Result.Waypoints);
	RefusedConnects = Result.SegmentsRefused;

	// THE RUNNERS: the rig forwards from the loop's start, the utility the other way from the
	// same node, after UtilityStartDelay.
	Runners.Reset();
	for (int32 Slot = 0; Slot < Vehicles.Num(); ++Slot)
	{
		FRigCourseRunner& Runner = Runners.AddDefaulted_GetRef();
		Runner.Slot = Slot;
		Runner.bReverse = Slot == 1;
		Runner.StartDelay = Runner.bReverse ? UtilityStartDelay : 0.0;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: course laid - %d segment(s) (%d refused), %d waypoint(s), %d feature(s), %d tier(s) of %d."),
		Result.SegmentsLaid, RefusedConnects, Waypoints.Num(), FeatureCountForTest(), FRigCourseLayout::TierCount, Widths);
}

int32 ARigTestCourse::FeatureCountForTest() const
{
	TSet<TPair<int32, uint8>> Seen;
	for (const FRigCourseWaypoint& W : Waypoints)
	{
		// The 3 x 5 lane features; the width step is its own named feature, counted apart.
		if (W.Feature != ERigCourseFeature::Connector && W.Feature != ERigCourseFeature::WidthStep)
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

double ARigTestCourse::LegTimeoutSeconds(const FVehicle& Vehicle, double Length, double Factor)
{
	const FGroundRegime& Taxi = Vehicle.Chassis.Ground.Taxi;
	const double Cap = FMath::Max(Taxi.SpeedCap, 1.0);
	const double Ideal = Length / Cap + Cap / FMath::Max(Taxi.Accel, 1.0) + Cap / FMath::Max(Taxi.Decel, 1.0);
	return Factor * Ideal;
}

int32 ARigTestCourse::LoopsCompletedByAllForTest() const
{
	int32 Fewest = Runners.Num() > 0 ? TNumericLimits<int32>::Max() : 0;
	for (const FRigCourseRunner& Runner : Runners)
	{
		Fewest = FMath::Min(Fewest, Runner.LoopsCompleted);
	}
	return Fewest;
}

FRigCourseWaypoint ARigTestCourse::StopAt(bool bReverse, int32 Position) const
{
	const int32 N = Waypoints.Num();
	if (N == 0)
	{
		return FRigCourseWaypoint();
	}
	const int32 P = ((Position % N) + N) % N;
	if (!bReverse)
	{
		return Waypoints[P];
	}
	// THE SAME NODE, ARRIVED AT THE OTHER WAY: along the road the forward leg leaves it by. The
	// reverse runner's leg P then runs forward leg N - 1 - P's node pair backwards - and, since a
	// leg's turn is at the node it LEAVES, turns at the other end of it: at the dead end the
	// U-turn falls in the reversed "T junction into the stem" leg. Keyed by node pair, not by
	// turn, so a result's index means the same road in both directions.
	FRigCourseWaypoint Stop = Waypoints[(N - P) % N];
	Stop.From = Stop.Next;
	return Stop;
}

int64 ARigTestCourse::LogKey(int32 Loop, int32 Slot) const
{
	// TWO HALVES PER LOOP: legs [0, N) for refusals and bypasses, N + stop for strandings.
	return static_cast<int64>(Loop) * 2 * Waypoints.Num() + Slot;
}

int32 ARigTestCourse::LegAt(bool bReverse, int32 Position) const
{
	const int32 N = Waypoints.Num();
	if (N == 0)
	{
		return 0;
	}
	const int32 P = ((Position % N) + N) % N;
	return bReverse ? N - 1 - P : P;
}

FString ARigTestCourse::LegLabel(const FRigCourseRunner& Runner, int32 Position) const
{
	const int32 N = Waypoints.Num();
	const FString& Label = Waypoints[(LegAt(Runner.bReverse, Position) + 1) % N].Label;
	return Runner.bReverse ? Label + TEXT(", reversed") : Label;
}

void ARigTestCourse::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Waypoints.Num() >= 2 && ResolveNetworkActor() != nullptr)
	{
		for (FRigCourseRunner& Runner : Runners)
		{
			TickRunner(Runner, DeltaSeconds);
		}
	}
	DrawRefusals();
}

void ARigTestCourse::TickRunner(FRigCourseRunner& Runner, double DeltaSeconds)
{
	const FString& Who = VehicleNames[Runner.Slot];
	if (Runner.StartDelay > 0.0)
	{
		Runner.StartDelay -= DeltaSeconds;
		if (Runner.StartDelay > 0.0)
		{
			return;
		}
		UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s starts after %.0f s, running the course %s."),
			*Who, UtilityStartDelay, Runner.bReverse ? TEXT("in reverse") : TEXT("forwards"));
	}
	if (Runner.AgentId == 0)
	{
		DispatchFresh(Runner);
		return;
	}

	// The leg being driven now: the next marker's, or the last stop when none is left.
	const bool bHasMarker = Runner.Markers.Num() > 0;
	const int32 Loop = bHasMarker ? Runner.Markers[0].Loop : Runner.LoopsCompleted + 1;
	const int32 Leg = LegAt(Runner.bReverse, Runner.Target - 1);
	const FString Label = LegLabel(Runner, Runner.Target - 1);
	UGroundTraffic* Model = NetworkActor->GetGroundTraffic();
	const FRoadAgent* Agent = Model != nullptr ? Model->FindAgent(Runner.AgentId) : nullptr;
	if (Agent == nullptr)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s) vanished before it arrived; skipped as stuck."),
			*Who, Loop, Leg, *Label);
		Runner.AgentId = 0;
		AbandonDrive(Runner, ERigLegOutcome::Stuck, TEXT("vanished"));
		return;
	}

	// The course has no stands and no reverse legs, so a Reversing agent means the layout is
	// wrong - and the Reversing phase does not step the tow chain, so it would drive a lie.
	if (Agent->Phase == EAgentPhase::Reversing && !Runner.bReversed)
	{
		Runner.bReversed = true;
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s) entered Reversing - the course has no reverse legs."),
			*Who, Loop, Leg, *Label);
	}

	// A JACK-KNIFED AGENT HOLDS FOR EVER: only StartDrive clears the link, and neither a
	// redirect (RestartTaxi) nor an extension calls it. Retire it through the normal path; the
	// next tick dispatches a fresh one, chain laid straight, on a new route from the next stop.
	// ENFORCED BY: Airside.Model.Tow.JackknifeStops (the fold holds 60 frames on) and
	// AirportMgr.RigCourse.JackknifeIsRetired (this branch is what ends the leg)
	if (Agent->GetJackknifedLink() != INDEX_NONE)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s) jack-knifed at link %d"),
			*Who, Loop, Leg, *Label, Agent->GetJackknifedLink());
		AbandonDrive(Runner, ERigLegOutcome::Jackknifed);
		return;
	}

	// THE ROUTE UNDER THE MARKERS, CHANGED BY SOMEONE ELSE. A marker is a distance along the
	// route the course planned; a replan by the traffic (a graph rebuild's re-resolve, a deadlock
	// replan) re-routes from where the vehicle is to the route's END, and the markers after it
	// then fire wherever that distance falls - which is how a rebuild once cut three dead ends
	// out of the utility's route with every leg still logged "arrived" (2026-09-25).
	if (Runner.RouteLength > 0.0 && FMath::Abs(Agent->PlanInProgress().Length - Runner.RouteLength) > 1.0)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d: its live route was replanned outside the course (%.0f m -> %.0f m) - its markers no longer measure its waypoints."),
			*Who, Loop, Runner.RouteLength / 100.0, Agent->PlanInProgress().Length / 100.0);
		Runner.RouteLength = Agent->PlanInProgress().Length;
	}

	// THE MARKERS PASSED THIS TICK - more than one if a tick carried it past a short leg. A
	// waypoint is a distance along the route, not a stop: nothing here slows the agent down.
	Runner.Elapsed += DeltaSeconds;
	const double Travelled = Agent->DistanceAlongPlan();
	while (Runner.Markers.Num() > 0 && Travelled >= Runner.Markers[0].EndDistance - 1.0)
	{
		const FRigLegMarker Passed = Runner.Markers[0];
		Runner.Markers.RemoveAt(0);
		PassMarker(Runner, *Agent, Passed);
	}

	// THE NEXT STRETCH, JOINED ON BEFORE THE VEHICLE BRAKES FOR THE ROUTE'S END. Twice the
	// stopping distance from the speed cap: FSpeedProfile starts braking for the end one stopping
	// distance out, so extending at two means the rebuilt profile finds road ahead before any
	// braking for the old end has begun - the loop boundary is driven through, not stopped at.
	const FGroundRegime& Taxi = Vehicles[Runner.Slot].Chassis.Ground.Taxi;
	const double ExtendWithin = FMath::Square(Taxi.SpeedCap) / FMath::Max(Taxi.Decel, 1.0);
	const double Remaining = Agent->PlanInProgress().Length - Travelled;
	if (Agent->Phase == EAgentPhase::Parked)
	{
		// THE ROUTE RAN OUT with nothing joined on (a leg that would not splice, a stranding, or
		// an extension refused): the one place the course still starts a leg from rest.
		ContinueRoute(Runner, *Agent, true);
		return;
	}
	if (Remaining <= ExtendWithin && !Runner.bExtendFailed)
	{
		ContinueRoute(Runner, *Agent, false);
	}

	if (Runner.Markers.Num() > 0 && Runner.Elapsed > Runner.Timeout)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s) stuck: not arrived after %.0f s (allowance %.0f s); skipped."),
			*Who, Runner.Markers[0].Loop, LegAt(Runner.bReverse, Runner.Target - 1), *LegLabel(Runner, Runner.Target - 1),
			Runner.Elapsed, Runner.Timeout);
		AbandonDrive(Runner, ERigLegOutcome::Stuck);
	}
}

double ARigTestCourse::HistoryToKeep(const FVehicle& Vehicle)
{
	// THE WHOLE VEHICLE BEHIND ITS STEERED AXLE, and room over: the chain's axles ride road the
	// route must still name for the tow's clearance probe (RigTestCourseTest's FClearanceProbe
	// searches a chain's length plus 5 m behind), and 10 m more so a trim never cuts under it.
	double Behind = Vehicle.Chassis.Wheelbase() + FMath::Abs(Vehicle.BodyRearX);
	for (const FTowLink& Link : Vehicle.Tow)
	{
		Behind += FMath::Abs(Link.HitchX) + Link.Length + Link.BodyRear + Link.BodyFront;
	}
	return Behind + 500.0 + 1000.0;
}

void ARigTestCourse::ReportSharpJoin(FRigCourseRunner& Runner, int32 Loop, const FRoutePlan& Head, const FRoutePlan& Tail, int32 IntoLeg)
{
	// THE JOIN ITSELF, not either side: a per-leg profile starts and ends AT the join, where a
	// polyline's end vertex has only one span and cannot be sharp. So the check runs over a few
	// samples either side of the weld and reports only a sharp vertex AT the weld.
	if (Head.Polyline.Num() < 2 || Tail.Polyline.Num() < 2)
	{
		return;
	}
	TArray<FVector2D> Around;
	for (int32 At = FMath::Max(0, Head.Polyline.Num() - 4); At < Head.Polyline.Num(); ++At)
	{
		Around.Add(Head.Polyline[At]);
	}
	const int32 JoinIndex = Around.Num() - 1;
	for (int32 At = 1; At < FMath::Min(Tail.Polyline.Num(), 4); ++At)
	{
		Around.Add(Tail.Polyline[At]);
	}
	double JoinDistance = 0.0;
	for (int32 At = 1; At <= JoinIndex; ++At)
	{
		JoinDistance += FVector2D::Distance(Around[At - 1], Around[At]);
	}
	FSpeedProfile Profile;
	Profile.Build(Around, Vehicles[Runner.Slot].Chassis);
	if (Profile.HasSharpVertex() && FMath::Abs(Profile.GetSharpestAt() - JoinDistance) < 1.0)
	{
		Runner.SharpJoinLegs.Add(IntoLeg);
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d: FSpeedProfile reports a sharp vertex at the join into leg %d (%s), %.0f deg - it will crawl there."),
			*VehicleNames[Runner.Slot], Loop, IntoLeg, *Waypoints[(IntoLeg + 1) % Waypoints.Num()].Label, Profile.GetSharpestDegrees());
	}
}

TArray<FRigLegResult>& ARigTestCourse::ResultsFor(FRigCourseRunner& Runner, int32 Loop)
{
	TArray<FRigLegResult>& Results = Runner.ResultsByLoop.FindOrAdd(Loop);
	if (Results.Num() != Waypoints.Num())
	{
		Results.SetNum(Waypoints.Num());
	}
	return Results;
}

void ARigTestCourse::PassMarker(FRigCourseRunner& Runner, const FRoadAgent& Agent, const FRigLegMarker& Marker)
{
	const int32 Leg = LegAt(Runner.bReverse, Marker.Target - 1);
	const double Travelled = Agent.DistanceAlongPlan();
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d leg %d (%s) arrived in %.1f s, passing at %.0f uu/s."),
		*VehicleNames[Runner.Slot], Marker.Loop, Leg, *LegLabel(Runner, Marker.Target - 1), Runner.Elapsed, Agent.Follower.Speed);
	if (Marker.Target == Marker.Position + 1)
	{
		FRigLegResult& Result = ResultsFor(Runner, Marker.Loop)[Leg];
		Result.Outcome = ERigLegOutcome::Driven;
		Result.EndPosition = Agent.GroundPosition();
		const FVector2D Facing(FMath::Cos(Agent.LastMotion.Heading), FMath::Sin(Agent.LastMotion.Heading));
		Result.SteeredPosition = Agent.LastMotion.Position + Facing * Vehicles[Runner.Slot].Chassis.SteerAxleX;
		double Unused = 0.0;
		GuidelineGeom::PointAtDistance(Agent.PlanInProgress().Polyline, Marker.EndDistance, Result.RoutePosition, Unused);
		Result.DistanceLeft = Marker.EndDistance - Travelled;
		Result.Elapsed = Runner.Elapsed;
		Result.PassSpeed = Agent.Follower.Speed;
		Result.bReversed = Runner.bReversed;
		Result.AgentId = Runner.AgentId;
		RefusalLabels.Remove(Runner.Slot * Waypoints.Num() + Leg);
	}
	// A routed-past stretch's legs were all recorded when it was planned (PlanLoopRoute).
	Runner.bReversed = false;
	Runner.Elapsed = 0.0;
	Runner.Position = Marker.Target;
	if (Runner.Position >= Waypoints.Num())
	{
		Runner.Position -= Waypoints.Num();
		EndLoop(Runner, Marker.Loop);
	}
	Runner.Target = Runner.Markers.Num() > 0 ? Runner.Markers[0].Target : Runner.Position;
	Runner.Timeout = Runner.Markers.Num() > 0
		? LegTimeoutSeconds(Vehicles[Runner.Slot], Runner.Markers[0].Length, LegTimeoutFactor) : Runner.Timeout;
}

uint32 ARigTestCourse::VehicleIdentity(const FVehicle& Vehicle)
{
	// FORWARDS (#301): the figures-hash itself now lives in Airside/Model/RoutePlanCache.h,
	// shared with FuelService::ChooseDepot. Kept at this name and signature - see the header.
	return RoutePlanCache::VehicleIdentity(Vehicle);
}

bool ARigTestCourse::PlanBetween(const FRigCourseWaypoint& From, const FRigCourseWaypoint& To, int32 Slot,
	FRoutePlan& OutPlan, FString& OutReason) const
{
	const URoadNetwork* Network = NetworkActor != nullptr ? NetworkActor->GetNetwork() : nullptr;
	const FString& Who = VehicleNames[Slot];
	if (Network == nullptr)
	{
		OutReason = FString::Printf(TEXT("%s: no network"), *Who);
		return false;
	}
	const FGuidelineNodeId Start = ResolveWaypoint(*Network, From);
	const FGuidelineNodeId Goal = ResolveWaypoint(*Network, To);
	if (!Start.IsSet() || !Goal.IsSet())
	{
		OutReason = FString::Printf(TEXT("%s: no lane end at %s"), *Who, Start.IsSet() ? TEXT("the goal") : TEXT("the start"));
		return false;
	}
	// CACHED PER (start, goal, vehicle), dated by the graph's revision (review of 9441ccf1): the
	// look-ahead asks the same pairs again and again on the one tick a loop is planned - HasOnward
	// is a lap of Finds per stop - and with the whole-route tow check each Find of a rig into a
	// balloon is a drive round it. Measured 2026-09-25: 2.4 s per rig loop boundary before, see
	// AirportMgr.RigCourse.OneLoopHeadless's "route plan(s)" line for after. A refusal is cached
	// too: it is as much a fact about the graph and the body as a route is. The vehicle is its
	// slot: Vehicles is fixed once the course is laid.
	// FRoutePlanCache (#301): the cache itself moved to Airside/Model/RoutePlanCache.h, the one
	// owner FuelService::ChooseDepot's identical cache now shares - this call site is unchanged
	// in shape, only in which type owns the tables.
	Cache.EnsureFresh(*Network);
	if (const FCachedRoutePlan* Hit = Cache.Lookup(Start, Goal, Vehicles[Slot]))
	{
		OutPlan = Hit->Plan;
		OutReason = Hit->Reason;
		return Hit->Plan.IsValid();
	}
	ON_SCOPE_EXIT
	{
		Cache.Store(Start, Goal, Vehicles[Slot], OutPlan, OutReason);
	};
	// PlayerIssued, NOT VehicleToJob: the course is a tool asking for a route directly, and
	// VehicleToJob requires the occupancy table - a congestion cost that, with the other vehicle
	// out, would make this route depend on where that vehicle happens to be, so a refusal would
	// stop being a fact about the body. The gate that matters is WithVehicle.
	FRouteQuery Query = FRouteQuery::For(ERouteErrand::PlayerIssued, Start, Goal, 0.0, ETraversalClass::GroundVehicle);
	Query.WithVehicle(Vehicles[Slot]);
	// AND EACH EDGE'S FIT, per vehicle, dated with the plans above: most of a Find's cost is
	// tracing the vehicle round curves, and every Find of the look-ahead asks the same curves.
	Query.FitCache = &Cache.FitCacheFor(Vehicles[Slot]);
	const double FindBegan = FPlatformTime::Seconds();
	OutPlan = RouteSearch::Find(*Network, Query);
	++PlanFinds;
	++TotalPlanFinds;
	PlanFindMs += (FPlatformTime::Seconds() - FindBegan) * 1000.0;
	if (!OutPlan.IsValid())
	{
		OutReason = DescribeRefusal(*Network, OutPlan, Slot);
		return false;
	}
	return true;
}

bool ARigTestCourse::PlanOwnLeg(const FRigCourseRunner& Runner, int32 Position, FRoutePlan& OutPlan, FString& OutReason,
	bool bDispatching) const
{
	const int32 Leg = LegAt(Runner.bReverse, Position);
	if (PlanOverrideForTest && PlanOverrideForTest(Leg, Runner.Slot, bDispatching, OutPlan))
	{
		UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s leg %d (%s): plan overridden by the test."),
			*VehicleNames[Runner.Slot], Leg, *LegLabel(Runner, Position));
		return true;
	}
	return PlanBetween(StopAt(Runner.bReverse, Position), StopAt(Runner.bReverse, Position + 1), Runner.Slot, OutPlan, OutReason);
}

bool ARigTestCourse::HasOnward(const FRigCourseRunner& Runner, int32 Stop) const
{
	// ANY later stop, not only the next: a stop whose own next leg is refused but which can route
	// on past it is no trap. A lap's worth, so a stop just before the loop's end looks into the next.
	const FRigCourseWaypoint From = StopAt(Runner.bReverse, Stop);
	for (int32 Later = Stop + 1; Later < Stop + Waypoints.Num(); ++Later)
	{
		FRoutePlan Unused;
		FString Reason;
		if (PlanBetween(From, StopAt(Runner.bReverse, Later), Runner.Slot, Unused, Reason))
		{
			return true;
		}
	}
	return false;
}

void ARigTestCourse::RecordBypass(FRigCourseRunner& Runner, int32 Loop, int32 Position, const TCHAR* Why)
{
	const int32 Leg = LegAt(Runner.bReverse, Position);
	ResultsFor(Runner, Loop)[Leg].Outcome = ERigLegOutcome::Bypassed;
	// ONCE PER LOOP, like a refusal: a Warning, because a leg that fits and is not driven is
	// something the log reader must see to read the loop's "D/T legs driven" right.
	if (!Runner.Logged.Contains(LogKey(Loop, Leg)))
	{
		Runner.Logged.Add(LogKey(Loop, Leg));
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s) bypassed: fits, but %s."),
			*VehicleNames[Runner.Slot], Loop, Leg, *LegLabel(Runner, Position), Why);
	}
}

void ARigTestCourse::RecordRefusal(FRigCourseRunner& Runner, int32 Loop, int32 Position, const FString& Reason)
{
	const int32 Leg = LegAt(Runner.bReverse, Position);
	const FString Label = LegLabel(Runner, Position);
	FRigLegResult& Result = ResultsFor(Runner, Loop)[Leg];
	Result.Outcome = ERigLegOutcome::Refused;
	Result.Reason = Reason;

	// ONCE PER LOOP: a no-hang exit re-plans the rest of its loop, and would otherwise say it twice.
	if (Runner.Logged.Contains(LogKey(Loop, Leg)))
	{
		return;
	}
	Runner.Logged.Add(LogKey(Loop, Leg));
	UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s) refused: %s"),
		*VehicleNames[Runner.Slot], Loop, Leg, *Label, *Reason);

	const URoadNetwork* Network = NetworkActor->GetNetwork();
	const FRigCourseWaypoint From = StopAt(Runner.bReverse, Position);
	const FGuidelineNodeId Start = Network != nullptr ? ResolveWaypoint(*Network, From) : FGuidelineNodeId();
	const FGuidelineNode* StartNode = Start.IsSet() ? Network->GetGuidelineNode(Start) : nullptr;
	const FRoadNode* RoadNode = Network != nullptr ? Network->GetNode(From.Node) : nullptr;
	const FVector2D StartAt = StartNode != nullptr ? StartNode->Position
		: (RoadNode != nullptr ? RoadNode->Position : FVector2D::ZeroVector);
	const double Z = NetworkActor->GetActorLocation().Z + 400.0;
	RefusalLabels.Add(Runner.Slot * Waypoints.Num() + Leg, TPair<FVector, FString>(FVector(StartAt, Z),
		FString::Printf(TEXT("%s leg %d (%s) refused: %s"), *VehicleNames[Runner.Slot], Leg, *Label, *Reason)));
}

bool ARigTestCourse::PlanLoopRoute(FRigCourseRunner& Runner, int32 Loop, int32 From, bool bHeadIsDispatch,
	FRoutePlan& OutPlan, TArray<FRigLegMarker>& OutMarkers, int32& OutEndStop)
{
	const int32 N = Waypoints.Num();
	const FString& Who = VehicleNames[Runner.Slot];
	const FVehicle& Vehicle = Vehicles[Runner.Slot];
	// THE HITCH, TIMED: this whole call runs on one tick - every leg's plan and every look-ahead.
	const double PlanBegan = FPlatformTime::Seconds();
	PlanFinds = 0;
	PlanFindMs = 0.0;
	JoinedJudgeMs = 0.0;
	const int32 TowChecksBefore = RouteSearch::TowCheckCountForTest();
	const double TowSecondsBefore = RouteSearch::TowCheckSecondsForTest();
	ON_SCOPE_EXIT
	{
		const double Ms = (FPlatformTime::Seconds() - PlanBegan) * 1000.0;
		WorstPlanMs = FMath::Max(WorstPlanMs, Ms);
		TotalPlanMs += Ms;
		++PlanCalls;
		// WHERE IT WENT, so a hitch is a measurement and not a guess: the Finds the look-ahead ran
		// (cache misses) and their time, the whole-route tow checks inside them, the joined judge.
		UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d route from waypoint %d planned in %.1f ms: %d Find(s) %.1f ms (%d tow check(s) %.1f ms), joined judge %.1f ms."),
			*Who, Loop, From, Ms, PlanFinds, PlanFindMs, RouteSearch::TowCheckCountForTest() - TowChecksBefore,
			(RouteSearch::TowCheckSecondsForTest() - TowSecondsBefore) * 1000.0, JoinedJudgeMs);
	};
	OutPlan = FRoutePlan();
	OutMarkers.Reset();
	OutEndStop = From;

	for (int32 Position = From; Position < N;)
	{
		// LOOK AHEAD ONE STOP: a stop with nothing onward that fits (the rig's view of a dead end it
		// cannot U-turn in) is a trap - driving into it could only end in a stranding, a retire and
		// a fresh chain laid back into a stem another vehicle may be U-turning in (the 173 uu
		// overlap measured 2026-09-25). So the leg into a trap is BYPASSED, not driven: the route
		// goes from this stop straight to the next one the vehicle can reach AND leave.
		FRoutePlan Leg;
		FString Reason;
		int32 Target = INDEX_NONE;
		const bool bOwnFits = PlanOwnLeg(Runner, Position, Leg, Reason, false);
		if (bOwnFits && HasOnward(Runner, Position + 1))
		{
			// ONLY THE HEAD OF A DISPATCH may spend a test override: the judging call above left
			// it unspent, and a leg joined further along never becomes a plan of its own.
			if (bHeadIsDispatch && !OutPlan.IsValid())
			{
				PlanOwnLeg(Runner, Position, Leg, Reason, true);
			}
			Target = Position + 1;
		}
		else
		{
			if (bOwnFits)
			{
				RecordBypass(Runner, Loop, Position, TEXT("nothing onward fits from its end"));
			}
			else
			{
				RecordRefusal(Runner, Loop, Position, Reason);
			}
			// ROUTE ON, FROM THIS STOP: the next waypoint it CAN reach and leave, up to the loop's
			// end (so a loop's results are all its own).
			const FRigCourseWaypoint Here = StopAt(Runner.bReverse, Position);
			for (int32 Stop = Position + 2; Stop <= N; ++Stop)
			{
				FString Unused;
				if (PlanBetween(Here, StopAt(Runner.bReverse, Stop), Runner.Slot, Leg, Unused) && HasOnward(Runner, Stop))
				{
					Target = Stop;
					break;
				}
			}
			if (Target == INDEX_NONE)
			{
				// STRANDED, THE FALLBACK: nothing ahead is reachable from this lane end. The route
				// ends here; when the vehicle runs out of it, ContinueRoute retires it and a fresh
				// one starts at the next waypoint.
				// ONCE PER LOOP, like a refusal: an extension retried and the from-rest path re-plan
				// the same stretch.
				if (!Runner.Logged.Contains(LogKey(Loop, Waypoints.Num() + Position)))
				{
					Runner.Logged.Add(LogKey(Loop, Waypoints.Num() + Position));
					UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d stranded at the start of leg %d (%s): no later waypoint is reachable from here; the route ends here and a fresh vehicle takes over at the next waypoint."),
						*Who, Loop, LegAt(Runner.bReverse, Position), *LegLabel(Runner, Position));
				}
				break;
			}
			// THE LEGS IT PASSES, each judged on its OWN plan: the loop's refusals are then exactly
			// the legs the router refuses this body, whatever road the vehicle took past them.
			for (int32 Passed = Position + 1; Passed < Target; ++Passed)
			{
				FRoutePlan Own;
				FString OwnReason;
				if (PlanOwnLeg(Runner, Passed, Own, OwnReason, false))
				{
					RecordBypass(Runner, Loop, Passed, TEXT("routed past"));
				}
				else
				{
					RecordRefusal(Runner, Loop, Passed, OwnReason);
				}
			}
			UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d routed on past the refusal to waypoint %d, %.0f m."),
				*Who, Loop, Target % N, Leg.Length / 100.0);
		}

		// JOINED WITH THE ROUTER'S OWN SPLICE, never by hand: it welds at the node the tail starts
		// from and re-bases every step's EndDistance/EndVertex, which is what the follower and the
		// arbiter read. A leg that does not start where the route ends (a test's overriding plan)
		// cannot be joined; the route ends before it, and ContinueRoute picks up from there.
		const FRoutePlan Joined = OutPlan.IsValid() ? RouteSearch::Splice(OutPlan, OutPlan.Steps.Num(), Leg) : Leg;
		if (!Joined.IsValid())
		{
			UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d: leg %d's plan does not join the route; the route ends before it."),
				*Who, Loop, LegAt(Runner.bReverse, Target - 1));
			break;
		}
		if (OutPlan.IsValid())
		{
			ReportSharpJoin(Runner, Loop, OutPlan, Leg, LegAt(Runner.bReverse, Target - 1));
		}
		OutPlan = Joined;

		if (Target == Position + 1)
		{
			FRigLegResult& Result = ResultsFor(Runner, Loop)[LegAt(Runner.bReverse, Position)];
			// The leg's own end: the goal's lane end, or wherever an overriding plan goes.
			Result.GoalPosition = Leg.Polyline.Last();

			// THE CRAWL, REPORTED PER LEG, from the leg's own plan: FSpeedProfile is the
			// drivability authority, and a sharp vertex is an instantaneous heading change it
			// crawls at steering speed. Nothing on this course should have one - the width step's
			// jog was the last, until the builder tapered it (AirportMgr.RigCourse.OneLoopHeadless).
			FSpeedProfile Profile;
			Profile.Build(Leg.Polyline, Vehicle.Chassis);
			Result.SharpVertexCount = Profile.GetSharpVertexCount();
			Result.SharpestDegrees = Profile.GetSharpestDegrees();
			if (Profile.HasSharpVertex())
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s leg %d (%s): FSpeedProfile reports %d sharp vertex(es), sharpest %.0f deg at %.0f m - it will crawl there."),
					*Who, LegAt(Runner.bReverse, Position), *LegLabel(Runner, Position), Profile.GetSharpVertexCount(),
					Profile.GetSharpestDegrees(), Profile.GetSharpestAt() / 100.0);
			}
		}
		FRigLegMarker& Marker = OutMarkers.AddDefaulted_GetRef();
		Marker.Loop = Loop;
		Marker.Position = Position;
		Marker.Target = Target;
		Marker.EndDistance = OutPlan.Length;
		Marker.Length = Leg.Length;
		Position = Target;
		OutEndStop = Target;
	}
	// THE JOINED ROUTE JUDGED WHOLE (review of 9441ccf1): every leg was judged on its own plan,
	// from a straight lay at its start, but the vehicle arrives at each join with its trailer
	// swung by the leg before. Judged ONCE, as the route will be dispatched (from rest at its first
	// point) - judging each join as it was made drove the route again per leg, 344 / 895 ms per
	// rig / utility loop boundary (2026-09-25). An extension of a live route is judged again, from
	// the live chain, by UGroundTraffic::ExtendRoute. A route that folds is CUT at the last leg's
	// end before the fold: the vehicle runs out there and plans on from rest, as for a leg that
	// does not join.
	// ENFORCED BY: AirportMgr.RigCourse.JoinThatFoldsIsCut (the cut, forced on a three-quarter
	// fixture) and Airside.Model.Tow.ExtendRouteJudgesTheJoin (the live re-judge).
	//
	// CUT BY DISTANCE, NOT BY MATCHING A STEP END (re-review of aa90eec2): the kept leg is the last
	// whose END lies strictly before the fold, and the plan keeps every step ending at or before
	// that - so a marker that no step end equals exactly still cuts. Nothing before the fold to keep
	// (it folds on the first leg) refuses the whole section, loudly: a folding route is never
	// handed on uncut.
	if (OutPlan.IsValid() && Vehicle.HasTrailer() && OutMarkers.Num() > 1)
	{
		const double JudgeBegan = FPlatformTime::Seconds();
		const FFitVerdict Whole = VehicleFit::JudgePlan(OutPlan, Vehicle, *NetworkActor->GetNetwork());
		JoinedJudgeMs += (FPlatformTime::Seconds() - JudgeBegan) * 1000.0;
		if (Whole.Refusal == EFitRefusal::TrailerFolds)
		{
			int32 Keep = INDEX_NONE;
			while (Keep + 1 < OutMarkers.Num() && OutMarkers[Keep + 1].EndDistance < Whole.Along)
			{
				++Keep;
			}
			int32 LastStep = INDEX_NONE;
			if (Keep != INDEX_NONE)
			{
				// A HAIR OF SLACK, not an equality: the marker's distance is the plan's length when
				// its leg was joined, and the steps' are re-summed by Splice.
				const double CutAt = OutMarkers[Keep].EndDistance + 0.01;
				while (LastStep + 1 < OutPlan.Steps.Num() && OutPlan.Steps[LastStep + 1].EndDistance <= CutAt)
				{
					++LastStep;
				}
			}
			if (LastStep != INDEX_NONE)
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d: the joined route folds the tow (%s); it ends at waypoint %d and plans on from rest."),
					*Who, Loop, *Whole.Describe(), OutMarkers[Keep].Target % N);
				OutPlan = RouteSearch::Section(OutPlan, 0, LastStep);
				OutMarkers.SetNum(Keep + 1);
				OutEndStop = OutMarkers[Keep].Target;
			}
			else
			{
				UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d: the joined route from waypoint %d folds the tow on its first leg (%s) and cannot be cut before it; this section is refused."),
					*Who, Loop, From, *Whole.Describe());
				OutPlan = FRoutePlan();
				OutMarkers.Reset();
				OutEndStop = From;
			}
		}
	}
	return OutPlan.IsValid();
}

void ARigTestCourse::DispatchFresh(FRigCourseRunner& Runner)
{
	if (NetworkActor->GetNetwork() == nullptr)
	{
		// Nothing to plan over: say so ONCE and wait, rather than a warning every tick or a
		// loop of "refused" legs that would blame the roads for a missing network.
		if (!bWarnedNoNetwork)
		{
			bWarnedNoNetwork = true;
			UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: the network actor has no network - nothing to drive."));
		}
		return;
	}
	const FString& Who = VehicleNames[Runner.Slot];
	const int32 Loop = Runner.LoopsCompleted + 1;
	FRoutePlan Plan;
	TArray<FRigLegMarker> Markers;
	int32 EndStop = Runner.Position;
	if (!PlanLoopRoute(Runner, Loop, Runner.Position, true, Plan, Markers, EndStop))
	{
		// Nothing drivable from this stop: on to the next, planned on the next tick.
		ArriveAt(Runner, Runner.Position + 1);
		return;
	}
	if (!NetworkActor->DispatchAgent(Plan, Vehicles[Runner.Slot], ETraversalClass::GroundVehicle))
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d leg %d (%s): the dispatch was refused; skipped as stuck."),
			*Who, Loop, LegAt(Runner.bReverse, Markers[0].Target - 1), *LegLabel(Runner, Markers[0].Target - 1));
		Runner.Markers = MoveTemp(Markers);
		Runner.Target = Runner.Markers[0].Target;
		AbandonDrive(Runner, ERigLegOutcome::Stuck, TEXT("dispatch refused"));
		return;
	}
	// THE NEWEST ID, because the forwarder answers only a bool: ARoadNetworkActor::DispatchAgent
	// is an IRoadEditTarget override, and widening that seam for one dev tool is not worth it.
	// Sound with two vehicles out because nothing dispatches between that call and this line:
	// the game thread runs both, back to back.
	Runner.AgentId = NetworkActor->GetTraffic()->GetNewestAgentId();
	++Runner.Dispatches;
	Runner.Markers = MoveTemp(Markers);
	Runner.RouteEndLoop = Loop;
	Runner.RouteEndStop = EndStop;
	Runner.bExtendFailed = false;
	Runner.RouteLength = Plan.Length;
	Runner.Target = Runner.Markers[0].Target;
	Runner.Elapsed = 0.0;
	Runner.Timeout = LegTimeoutSeconds(Vehicles[Runner.Slot], Runner.Markers[0].Length, LegTimeoutFactor);
	Runner.bReversed = false;
	for (const FRigLegMarker& Marker : Runner.Markers)
	{
		ResultsFor(Runner, Marker.Loop)[LegAt(Runner.bReverse, Marker.Target - 1)].AgentId = Runner.AgentId;
	}
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d dispatched as agent %d from waypoint %d: one route, %d leg(s), %.0f m."),
		*Who, Loop, Runner.AgentId, Runner.Position, Runner.Markers.Num(), Plan.Length / 100.0);
}

void ARigTestCourse::ContinueRoute(FRigCourseRunner& Runner, const FRoadAgent& Agent, bool bFromRest)
{
	const FString& Who = VehicleNames[Runner.Slot];
	const int32 N = Waypoints.Num();
	// WHERE THE LIVE ROUTE ENDS is where the next stretch starts: the next loop's start when it
	// ends a loop, or the stop it was cut short at.
	int32 Loop = Runner.RouteEndLoop;
	int32 From = Runner.RouteEndStop;
	if (From >= N)
	{
		++Loop;
		From = 0;
	}
	FRoutePlan Tail;
	TArray<FRigLegMarker> Markers;
	int32 EndStop = From;
	const bool bPlanned = PlanLoopRoute(Runner, Loop, From, false, Tail, Markers, EndStop);
	const URoadNetwork* Network = NetworkActor->GetNetwork();

	if (!bFromRest)
	{
		// SPLICED ONTO THE LIVE ROUTE, NOT A REDIRECT: RedirectAgent restarts the follower from
		// rest at the plan's first point, which is the stop at every loop boundary this change is
		// here to remove. ExtendRoute keeps Travelled, Speed, Heading and the chain, so the new
		// markers sit at the old route's length plus their own.
		//
		// AND THE DRIVEN HISTORY TRIMMED, so a course left running does not grow its route - or
		// every per-tick walk over it - for ever: all but HistoryToKeep behind the vehicle, which
		// covers the tow chain and the clearance probe's window. What was dropped comes off every
		// distance this course keeps along the route.
		const double JoinAt = Agent.PlanInProgress().Length;
		double Dropped = 0.0;
		// The join is judged BEFORE the extension (the live route is about to change under it) and
		// reported only if the extension is made.
		const FRoutePlan LiveBefore = Agent.PlanInProgress();
		if (bPlanned && !bRefuseExtensionsForTest && NetworkActor->GetTraffic()->ExtendRoute(Runner.AgentId, Network, Tail,
			HistoryToKeep(Vehicles[Runner.Slot]), &Dropped))
		{
			ReportSharpJoin(Runner, Loop, LiveBefore, Tail, LegAt(Runner.bReverse, Markers[0].Target - 1));
			for (FRigLegMarker& Marker : Runner.Markers)
			{
				Marker.EndDistance -= Dropped;
			}
			for (FRigLegMarker& Marker : Markers)
			{
				Marker.EndDistance += JoinAt - Dropped;
				ResultsFor(Runner, Marker.Loop)[LegAt(Runner.bReverse, Marker.Target - 1)].AgentId = Runner.AgentId;
			}
			if (Runner.Markers.Num() == 0)
			{
				Runner.Target = Markers[0].Target;
				Runner.Timeout = LegTimeoutSeconds(Vehicles[Runner.Slot], Markers[0].Length, LegTimeoutFactor);
			}
			Runner.Markers.Append(Markers);
			// Re-read: ExtendRoute replaced the plan (and trimmed its history) in place.
			if (const FRoadAgent* Extended = NetworkActor->GetGroundTraffic()->FindAgent(Runner.AgentId))
			{
				Runner.RouteLength = Extended->PlanInProgress().Length;
			}
			Runner.RouteEndLoop = Loop;
			Runner.RouteEndStop = EndStop;
			++Runner.Extensions;
			UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d spliced onto the live route at %.0f m: %d leg(s), %.0f m, no stop."),
				*Who, Loop, JoinAt / 100.0, Markers.Num(), Tail.Length / 100.0);
			return;
		}
		// NOT NOW: the vehicle drives on to the end of what it has and ContinueRoute runs again
		// from rest when it parks. Not re-planned every tick until then.
		Runner.bExtendFailed = true;
		return;
	}

	// FROM REST: the route ran out. A stranding with nothing onward retires the vehicle and a
	// fresh one takes the next waypoint; otherwise the same agent is redirected on, its chain kept.
	//
	// THE CHAIN CARRIES ON THROUGH THE REDIRECT: RedirectAgent's RestartTaxi re-seats the follower
	// on the plan's first point - the lane end it has just stopped on - and leaves TowAxles, the
	// fold and (for a tow) the cab's heading alone; StartDrive, which a redirect does not call, is
	// where a chain is laid straight. So even the fallback does not re-lay the trailer.
	// ENFORCED BY: AirportMgr.RigCourse.RanOutRestartsWithTheChain (extensions refused, every loop
	// boundary taken from rest, one agent throughout, the chain never jumping)
	if (!bPlanned)
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s loop %d: nothing drivable from waypoint %d; retired, and dispatched fresh at the next waypoint."),
			*Who, Loop, From);
		NetworkActor->GetTraffic()->RetireAgent(Runner.AgentId);
		Runner.AgentId = 0;
		Runner.Markers.Reset();
		ArriveAt(Runner, From + 1);
		return;
	}
	if (!NetworkActor->GetTraffic()->RedirectAgent(Runner.AgentId, Network, Tail))
	{
		UE_LOG(LogRoadBuild, Warning, TEXT("RigCourse: %s agent %d: the redirect was refused; retired, dispatching fresh."),
			*Who, Runner.AgentId);
		NetworkActor->GetTraffic()->RetireAgent(Runner.AgentId);
		Runner.AgentId = 0;
		Runner.Markers.Reset();
		return;
	}
	++Runner.Redirects;
	Runner.RouteLength = Tail.Length;
	Runner.Markers = MoveTemp(Markers);
	Runner.RouteEndLoop = Loop;
	Runner.RouteEndStop = EndStop;
	Runner.bExtendFailed = false;
	Runner.Target = Runner.Markers[0].Target;
	Runner.Elapsed = 0.0;
	Runner.Timeout = LegTimeoutSeconds(Vehicles[Runner.Slot], Runner.Markers[0].Length, LegTimeoutFactor);
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d redirected from rest at waypoint %d: %d leg(s), %.0f m."),
		*Who, Loop, From, Runner.Markers.Num(), Tail.Length / 100.0);
}

FString ARigTestCourse::DescribeRefusal(const URoadNetwork& Network, const FRoutePlan& Plan, int32 Slot) const
{
	const UEnum* ResultEnum = StaticEnum<ERouteResult>();
	FString Text = FString::Printf(TEXT("%s %s"), *VehicleNames[Slot],
		*ResultEnum->GetNameStringByValue(static_cast<int64>(Plan.Result)));
	if (Plan.Result != ERouteResult::TooNarrow || !Plan.RejectedEdge.IsSet())
	{
		return Text;
	}
	// A WHOLE-ROUTE REFUSAL (a tow folding, 2026-09-25) names its own place and figures: no edge
	// re-judged on its own would refuse it, which is the point of the check.
	if (Plan.RejectedBy.bWholeRoute && !Plan.RejectedBy.Fits())
	{
		return Text + TEXT(", ") + Plan.RejectedBy.Describe();
	}
	const FGuidelineEdge* Edge = Network.GetGuidelineEdge(Plan.RejectedEdge);
	const FGuidelineNode* At = Edge != nullptr ? Network.GetGuidelineNode(Edge->A) : nullptr;
	if (Edge == nullptr || At == nullptr)
	{
		return Text;
	}
	// THE ROUTER'S OWN RULE, asked again for its figures (VehicleFit::Judge is Fits' body), so
	// the numbers printed are the ones the refusal was made on.
	const FFitVerdict Verdict = VehicleFit::Judge(*Edge, Vehicles[Slot], Network);
	Text += FString::Printf(TEXT(" at guideline node %d (%.0f, %.0f)"), Edge->A.Index, At->Position.X, At->Position.Y);
	if (!Verdict.Fits())
	{
		Text += TEXT(", ") + Verdict.Describe();
	}
	return Text;
}

void ARigTestCourse::AbandonDrive(FRigCourseRunner& Runner, ERigLegOutcome Outcome, const FString& Reason)
{
	// THE LEG BEING DRIVEN: the next marker's, in that marker's loop.
	const int32 Loop = Runner.Markers.Num() > 0 ? Runner.Markers[0].Loop : Runner.LoopsCompleted + 1;
	const int32 Target = Runner.Markers.Num() > 0 ? Runner.Markers[0].Target : Runner.Target;
	FRigLegResult& Result = ResultsFor(Runner, Loop)[LegAt(Runner.bReverse, Target - 1)];
	Result.Outcome = Outcome;
	Result.Reason = Reason;
	Result.bReversed = Runner.bReversed;
	Runner.bReversed = false;
	if (Runner.AgentId != 0)
	{
		// Named on the result even for a routed-past stretch, whose last leg had no agent recorded.
		Result.AgentId = Runner.AgentId;
		NetworkActor->GetTraffic()->RetireAgent(Runner.AgentId);
		Runner.AgentId = 0;
	}
	// THE ROUTE GOES WITH IT: the next tick dispatches a fresh agent on a new route from the stop
	// it was driving to.
	Runner.Markers.Reset();
	ArriveAt(Runner, Target);
}

void ARigTestCourse::ArriveAt(FRigCourseRunner& Runner, int32 Stop)
{
	Runner.Position = Stop;
	if (Runner.Position >= Waypoints.Num())
	{
		Runner.Position -= Waypoints.Num();
		EndLoop(Runner, Runner.LoopsCompleted + 1);
	}
	Runner.Target = Runner.Position;
}

void ARigTestCourse::EndLoop(FRigCourseRunner& Runner, int32 Loop)
{
	Runner.LoopsCompleted = Loop;
	// BOUNDED: the loop's once-per-loop keys are spent with it.
	for (int32 Slot = 0; Slot < 2 * Waypoints.Num(); ++Slot)
	{
		Runner.Logged.Remove(LogKey(Loop, Slot));
	}
	const TArray<FRigLegResult> Results = ResultsFor(Runner, Loop);
	Runner.ResultsByLoop.Remove(Loop);
	int32 Driven = 0;
	TArray<FString> Mine;
	// Apart from the refusals: a bypassed leg FITS, and listing it under "refused" would say
	// the road needs widening when it does not.
	TArray<FString> Passed;
	for (int32 L = 0; L < Waypoints.Num(); ++L)
	{
		const FRigLegResult& R = Results[L];
		const FString& Label = Waypoints[(L + 1) % Waypoints.Num()].Label;
		switch (R.Outcome)
		{
		case ERigLegOutcome::Driven:     ++Driven; break;
		case ERigLegOutcome::Refused:    Mine.Add(FString::Printf(TEXT("%d (%s)"), L, *Label)); break;
		case ERigLegOutcome::Bypassed:   Passed.Add(FString::Printf(TEXT("%d (%s)"), L, *Label)); break;
		case ERigLegOutcome::Jackknifed: Mine.Add(FString::Printf(TEXT("%d (%s, jack-knifed)"), L, *Label)); break;
		case ERigLegOutcome::Stuck:      Mine.Add(FString::Printf(TEXT("%d (%s, stuck)"), L, *Label)); break;
		default: break;
		}
	}
	UE_LOG(LogRoadBuild, Log, TEXT("RigCourse: %s loop %d - %d/%d legs driven; refused: %s; bypassed: %s"),
		*VehicleNames[Runner.Slot], Loop, Driven, Waypoints.Num(),
		Mine.Num() > 0 ? *FString::Join(Mine, TEXT(", ")) : TEXT("none"),
		Passed.Num() > 0 ? *FString::Join(Passed, TEXT(", ")) : TEXT("none"));
	Runner.LastLoopResults = Results;
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
