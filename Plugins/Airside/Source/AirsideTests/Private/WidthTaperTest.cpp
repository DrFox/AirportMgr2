#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadMeshBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideContent.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/SpeedProfile.h"
#include "Model/Vehicle.h"
#include "Profiles/RoadDesignVehicles.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/RoadGeom.h"

#if WITH_DEV_AUTOMATION_TESTS

// A WIDTH CHANGE MID-STRAIGHT TAPERS ON AN S (user ruling 2026-09-25). Two collinear service
// roads of different tiers met at a degree-2 node with both cuts on the node, and the lane turn
// between them was the lane-offset step laid across nothing: a 90 degree jog, MinRadius 0,
// crawled by FSpeedProfile (the rig course measured 55 s over a straight that takes 14). Now both
// cuts are inset, the pavement between them is a taper polygon, and each lane crosses it on an S
// sized for the WIDER tier's design vehicle. Every network here is derived the production way:
// the content tiers, UAirsideSettings::ResolveRoadDesignVehicles, the solver, then the builder.

namespace WidthTaper
{
	/** The three content tiers, narrow first, or empty when the set does not have them. */
	TArray<URoadProfile*> Tiers()
	{
		TArray<URoadProfile*> Out;
		const UAirsideContent* Content = UAirsideSettings::GetContent();
		if (Content == nullptr || Content->ServiceRoadProfiles.Num() != 3)
		{
			return Out;
		}
		for (const TSoftObjectPtr<URoadProfile>& Tier : Content->ServiceRoadProfiles)
		{
			Out.Add(Tier.LoadSynchronous());
		}
		return Out;
	}

	/** Narrow (0,0)->(6000,0), then Wide (6000,0)->(12000,0): one straight, one width change. */
	struct FStep
	{
		URoadNetwork* Net = nullptr;
		FRoadSolveResult Solved;
		FRoadNodeId Mid;
		FRoadSegmentId Narrow;
		FRoadSegmentId Wide;
	};

	FStep Build(URoadProfile* NarrowProfile, URoadProfile* WideProfile)
	{
		FStep Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		const FRoadNodeId West = Out.Net->AddNode(FVector2D(0.0, 0.0));
		Out.Mid = Out.Net->AddNode(FVector2D(6000.0, 0.0));
		const FRoadNodeId East = Out.Net->AddNode(FVector2D(12000.0, 0.0));
		Out.Narrow = Out.Net->AddStraightSegment(West, Out.Mid, NarrowProfile);
		Out.Wide = Out.Net->AddStraightSegment(Out.Mid, East, WideProfile);
		const FRoadDesignVehicles Designs = UAirsideSettings::ResolveRoadDesignVehicles();
		Out.Solved = FRoadNetworkSolver::SolveAll(*Out.Net, 12, &Designs);
		FRoadGuidelineBuilder::Build(*Out.Net, Out.Solved, Designs);
		return Out;
	}

	/** A lane end of Segment: its guideline running Dir, at its A end or its B end. */
	FGuidelineNodeId LaneEnd(const URoadNetwork& Net, FRoadSegmentId Segment, EGuidelineDir Dir, bool bEndA)
	{
		for (const FGuidelineEdge& Edge : Net.GetGuidelineEdges())
		{
			if (Edge.bAlive && Edge.DerivedFrom == Segment && Edge.Direction == Dir)
			{
				return bEndA ? Edge.A : Edge.B;
			}
		}
		return FGuidelineNodeId();
	}

	/** The route the rig takes from Start to Goal, with its body. */
	FRoutePlan RigRoute(const URoadNetwork& Net, FGuidelineNodeId Start, FGuidelineNodeId Goal, const FVehicle& Rig)
	{
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, Start, Goal, 0.0, ETraversalClass::GroundVehicle);
		Query.WithVehicle(Rig);
		return RouteSearch::Find(Net, Query);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidthTaperDrivableTest, "Airside.Build.WidthTaper.Drivable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthTaperDrivableTest::RunTest(const FString& Parameters)
{
	using namespace WidthTaper;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FStep Step = Build(Profiles[0], Profiles[2]);
	TestEqual(TEXT("every node solved"), Step.Solved.FailedNodes, 0);
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const double Lock = Rig.Chassis.TightestFollowableRadius();
	if (!TestTrue(TEXT("the rig has a lock to clear"), Lock > 0.0)) { return false; }

	// THE INSET: both cuts back from the node - the taper's room. Before this, both were 0.
	const FRoadSegment* Narrow = Step.Net->GetSegment(Step.Narrow);
	const FRoadSegment* Wide = Step.Net->GetSegment(Step.Wide);
	if (!TestTrue(TEXT("both segments resolve"), Narrow != nullptr && Wide != nullptr)) { return false; }
	const double Taper = Narrow->TrimB + Wide->TrimA;
	UE_LOG(LogTemp, Display, TEXT("WidthTaper: Narrow %.0f -> Wide %.0f uu, taper %.0f uu (inset %.0f + %.0f), rig lock R %.0f uu"),
		Profiles[0]->GetTotalWidth(), Profiles[2]->GetTotalWidth(), Taper, Narrow->TrimB, Wide->TrimA, Lock);
	TestTrue(FString::Printf(TEXT("both cuts are inset from the node (%.0f, %.0f uu)"), Narrow->TrimB, Wide->TrimA),
		Narrow->TrimB > 0.0 && Wide->TrimA > 0.0);

	// BOTH LANES, BOTH WAYS: Narrow -> Wide on the A->B lane, Wide -> Narrow on the other.
	struct FLane { const TCHAR* Name; FGuidelineNodeId Start; FGuidelineNodeId Goal; };
	const FLane Lanes[2] = {
		{ TEXT("Narrow -> Wide"), LaneEnd(*Step.Net, Step.Narrow, EGuidelineDir::AToB, true), LaneEnd(*Step.Net, Step.Wide, EGuidelineDir::AToB, false) },
		{ TEXT("Wide -> Narrow"), LaneEnd(*Step.Net, Step.Wide, EGuidelineDir::BToA, false), LaneEnd(*Step.Net, Step.Narrow, EGuidelineDir::BToA, true) },
	};
	for (const FLane& Lane : Lanes)
	{
		const FRoutePlan Plan = RigRoute(*Step.Net, Lane.Start, Lane.Goal, Rig);
		if (!TestTrue(FString::Printf(TEXT("%s: the rig is admitted across the taper (%d)"), Lane.Name, static_cast<int32>(Plan.Result)),
			Plan.IsValid())) { continue; }

		// FSpeedProfile IS THE DRIVABILITY AUTHORITY: over the whole route, not per edge.
		FSpeedProfile Profile;
		Profile.Build(Plan.Polyline, Rig.Chassis);
		TestFalse(FString::Printf(TEXT("%s: no sharp vertex - the jog is gone (%d, sharpest %.0f deg)"), Lane.Name,
			Profile.GetSharpVertexCount(), Profile.GetSharpestDegrees()), Profile.HasSharpVertex());

		// AND THE S IS MEASURED, not left at the 0 that meant "unmeasured, ungated".
		int32 TurnEdges = 0;
		for (const FRouteStep& RouteStep : Plan.Steps)
		{
			const FGuidelineEdge* Edge = Step.Net->GetGuidelineEdge(RouteStep.Edge);
			if (Edge == nullptr || Edge->DerivedFrom.IsSet()) { continue; }
			++TurnEdges;
			UE_LOG(LogTemp, Display, TEXT("WidthTaper: %s turn edge %d, MinRadius %.1f uu"), Lane.Name, RouteStep.Edge.Index, Edge->MinRadius);
			TestTrue(FString::Printf(TEXT("%s: the taper's curve is at least the rig's lock (%.1f vs %.1f uu)"), Lane.Name, Edge->MinRadius, Lock),
				Edge->MinRadius >= Lock);
		}
		TestEqual(FString::Printf(TEXT("%s: the lane crosses the taper on an S - two curves"), Lane.Name), TurnEdges, 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidthTaperLanesContinuousTest, "Airside.Build.WidthTaper.LanesContinuous",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthTaperLanesContinuousTest::RunTest(const FString& Parameters)
{
	using namespace WidthTaper;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FStep Step = Build(Profiles[0], Profiles[2]);

	// EACH LANE END IS JOINED TO THE ONE ACROSS THE TAPER, BY HANDLE, AND THE LINE HAS NO STEP:
	// the S's two pieces share their middle node, start on the arriving lane's end and finish on
	// the leaving lane's, and the heading is continuous everywhere along the samples - at the
	// lane ends, at the inflection and between. A jog is a 90 degree step between two samples.
	struct FPair { const TCHAR* Name; FGuidelineNodeId From; FGuidelineNodeId To; };
	const FPair Pairs[2] = {
		{ TEXT("Narrow -> Wide"), LaneEnd(*Step.Net, Step.Narrow, EGuidelineDir::AToB, false), LaneEnd(*Step.Net, Step.Wide, EGuidelineDir::AToB, true) },
		{ TEXT("Wide -> Narrow"), LaneEnd(*Step.Net, Step.Wide, EGuidelineDir::BToA, true), LaneEnd(*Step.Net, Step.Narrow, EGuidelineDir::BToA, false) },
	};
	for (const FPair& Pair : Pairs)
	{
		if (!TestTrue(FString::Printf(TEXT("%s: both lane ends exist"), Pair.Name), Pair.From.IsSet() && Pair.To.IsSet())) { continue; }
		const FGuidelineNode* From = Step.Net->GetGuidelineNode(Pair.From);
		const FGuidelineNode* To = Step.Net->GetGuidelineNode(Pair.To);
		TestTrue(FString::Printf(TEXT("%s: the lane ends are apart - the taper is between them (%.0f uu)"), Pair.Name,
			FVector2D::Distance(From->Position, To->Position)), FVector2D::Distance(From->Position, To->Position) > 100.0);

		// Walk the turn edges out of From: the first piece, then the one out of its end.
		const FGuidelineEdge* First = nullptr;
		for (const FGuidelineEdge& Edge : Step.Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet() && Edge.A == Pair.From) { First = &Edge; }
		}
		if (!TestNotNull(FString::Printf(TEXT("%s: a turn path leaves the arriving lane's end"), Pair.Name), First)) { continue; }
		const FGuidelineEdge* Second = nullptr;
		for (const FGuidelineEdge& Edge : Step.Net->GetGuidelineEdges())
		{
			if (Edge.bAlive && !Edge.DerivedFrom.IsSet() && Edge.A == First->B) { Second = &Edge; }
		}
		if (!TestNotNull(FString::Printf(TEXT("%s: and a second piece leaves where the first ends - no gap"), Pair.Name), Second)) { continue; }
		TestTrue(FString::Printf(TEXT("%s: the second piece ends ON the leaving lane's end"), Pair.Name), Second->B == Pair.To);

		// Sampled ONCE, by GuidelineGeom, exactly as route search and the follower take them.
		TArray<FVector2D> Line;
		GuidelineGeom::Sample(From->Position, First->Control, Step.Net->GetGuidelineNode(First->B)->Position, Line);
		TArray<FVector2D> Tail;
		GuidelineGeom::Sample(Step.Net->GetGuidelineNode(Second->A)->Position, Second->Control, To->Position, Tail);
		TestTrue(FString::Printf(TEXT("%s: the pieces meet bitwise at the inflection"), Pair.Name), Line.Last() == Tail[0]);
		Line.Append(&Tail[1], Tail.Num() - 1);

		// The lane direction either side, from the lane edges themselves.
		const FVector2D Travel = FVector2D(Pair.Name[0] == TEXT('N') ? 1.0 : -1.0, 0.0);
		double Worst = 0.0;
		FVector2D Heading = Travel;
		for (int32 I = 0; I + 1 < Line.Num(); ++I)
		{
			const FVector2D Span = (Line[I + 1] - Line[I]).GetSafeNormal();
			Worst = FMath::Max(Worst, FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(FVector2D::CrossProduct(Heading, Span)), FVector2D::DotProduct(Heading, Span))));
			Heading = Span;
		}
		Worst = FMath::Max(Worst, FMath::RadiansToDegrees(FMath::Atan2(FMath::Abs(FVector2D::CrossProduct(Heading, Travel)), FVector2D::DotProduct(Heading, Travel))));
		TestTrue(FString::Printf(TEXT("%s: the heading never steps - worst %.2f deg between samples, lane to lane"), Pair.Name, Worst), Worst < 5.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidthTaperSurfaceWeldsTest, "Airside.Build.WidthTaper.SurfaceWelds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWidthTaperSurfaceWeldsTest::RunTest(const FString& Parameters)
{
	using namespace WidthTaper;
	const TArray<URoadProfile*> Profiles = Tiers();
	if (!TestTrue(TEXT("the content set has its three service-road tiers, and they load"),
		Profiles.Num() == 3 && !Profiles.Contains(nullptr))) { return false; }
	const FStep Step = Build(Profiles[0], Profiles[2]);
	FRoadMeshBuilder Builder(10.0);
	Builder.Build(*Step.Net, Step.Solved, 3);
	const FRoadMeshBuffers& Buffers = Builder.GetBuffers();

	const FJunctionResult* Junction = Step.Solved.NodeResults.Find(Step.Mid.Index);
	if (!TestNotNull(TEXT("the width-change node solved"), Junction)) { return false; }
	const FRoadSegment* Narrow = Step.Net->GetSegment(Step.Narrow);
	const FRoadSegment* Wide = Step.Net->GetSegment(Step.Wide);

	// THE TAPER IS PAVED: a polygon of area, not the zero-width line both cuts on the node made.
	const int32 RimCount = Junction->Boundary.Num() - 1;
	TArray<FVector2D> Rim;
	for (int32 I = 0; I < RimCount; ++I) { Rim.Add(Junction->Boundary[I]); }
	const double Area = FMath::Abs(RoadGeom::PolygonArea(Rim));
	TestTrue(FString::Printf(TEXT("the taper polygon has area (%.0f uu^2)"), Area), Area > 1000.0);

	// THE WELD, MEASURED. Each of the four cut vertices at the taper is ONE mesh vertex - the
	// ribbon and the taper produced the same bits - and every mesh edge lying along either cut
	// line is used by exactly TWO triangles: one of the ribbon's, one of the taper's. A crack is
	// an edge used once; a tolerance weld would still find the vertex but leave the edges open.
	const FVector2D Cuts[4] = { Narrow->LeftCutB, Narrow->RightCutB, Wide->LeftCutA, Wide->RightCutA };
	for (const FVector2D& Cut : Cuts)
	{
		int32 Matches = 0;
		for (const FVector3d& P : Buffers.Positions) { Matches += (P.X == Cut.X && P.Y == Cut.Y) ? 1 : 0; }
		TestEqual(FString::Printf(TEXT("cut vertex (%.1f, %.1f) is one welded mesh vertex"), Cut.X, Cut.Y), Matches, 1);
	}
	TMap<TPair<int32, int32>, int32> EdgeUse;
	for (int32 Slot = 0; Slot + 2 < Buffers.Indices.Num(); Slot += 3)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 A = Buffers.Indices[Slot + K];
			const int32 B = Buffers.Indices[Slot + (K + 1) % 3];
			EdgeUse.FindOrAdd(TPair<int32, int32>(FMath::Min(A, B), FMath::Max(A, B)))++;
		}
	}
	auto OnLine = [](const FVector3d& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Q(P.X, P.Y);
		const FVector2D AB = B - A;
		const double T = FVector2D::DotProduct(Q - A, AB) / AB.SizeSquared();
		return T >= -1e-9 && T <= 1.0 + 1e-9 && FMath::Abs(FVector2D::CrossProduct(AB, Q - A)) / AB.Size() < 1e-3;
	};
	const TPair<FVector2D, FVector2D> Lines[2] = { { Narrow->LeftCutB, Narrow->RightCutB }, { Wide->LeftCutA, Wide->RightCutA } };
	int32 CutEdges = 0;
	for (const TPair<FVector2D, FVector2D>& Line : Lines)
	{
		for (const TPair<TPair<int32, int32>, int32>& Use : EdgeUse)
		{
			if (OnLine(Buffers.Positions[Use.Key.Key], Line.Key, Line.Value) && OnLine(Buffers.Positions[Use.Key.Value], Line.Key, Line.Value))
			{
				++CutEdges;
				TestEqual(FString::Printf(TEXT("cut-line edge (%.1f, %.1f)-(%.1f, %.1f) is shared by the ribbon and the taper"),
					Buffers.Positions[Use.Key.Key].X, Buffers.Positions[Use.Key.Key].Y,
					Buffers.Positions[Use.Key.Value].X, Buffers.Positions[Use.Key.Value].Y), Use.Value, 2);
			}
		}
	}
	TestTrue(FString::Printf(TEXT("edges along both cut lines were measured - not vacuous (%d)"), CutEdges), CutEdges >= 2);
	return true;
}

#endif
