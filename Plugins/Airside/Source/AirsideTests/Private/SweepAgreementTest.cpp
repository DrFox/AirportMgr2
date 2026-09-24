#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Profiles/RoadProfile.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// ROUTING AND DRIVING MUST AGREE ABOUT WHERE A VEHICLE GOES (2026-09-24). Route search admits a
// vehicle on VehicleSweep::Trace's prediction of its sweep; FRouteFollower is what then drives
// it. They are two implementations of one kinematic model - the steered axle on the line, the
// fixed axle travelling along the body - and these tests are what hold them to it, rather than
// comments saying so.

namespace SweepAgreement
{
	/** A right-angle quadratic from (0,-Leg) through (0,0) to (Leg,0): a junction's turn. */
	TArray<FVector2D> Turn(double Leg)
	{
		TArray<FVector2D> Out;
		GuidelineGeom::Sample(FVector2D(0.0, -Leg), FVector2D::ZeroVector, FVector2D(Leg, 0.0), Out);
		return Out;
	}

	/** How far Point lies inside Path's turn, or a negative value when outside or past an end. */
	double InsideOf(TArrayView<const FVector2D> Path, const FVector2D& Point)
	{
		const FVector2D Mid = Path[Path.Num() / 2];
		const double Sign = FVector2D::CrossProduct(Mid - Path[0], Path.Last() - Mid) >= 0.0 ? 1.0 : -1.0;
		double Best = TNumericLimits<double>::Max();
		double Lateral = -1.0;
		for (int32 Span = 0; Span + 1 < Path.Num(); ++Span)
		{
			const FVector2D AB = Path[Span + 1] - Path[Span];
			const double T = FVector2D::DotProduct(Point - Path[Span], AB) / AB.SizeSquared();
			const FVector2D Foot = Path[Span] + AB * FMath::Clamp(T, 0.0, 1.0);
			const double Distance = FVector2D::Distance(Point, Foot);
			if (Distance < Best)
			{
				Best = Distance;
				const bool bPastEnd = (Span == 0 && T < 0.0) || (Span + 2 == Path.Num() && T > 1.0);
				const FVector2D Normal(-AB.Y, AB.X);
				Lateral = bPastEnd ? -1.0 : FVector2D::DotProduct(Point - Foot, Normal.GetSafeNormal() * Sign);
			}
		}
		return Lateral;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFollowerMatchesSweepTest, "Airside.Model.FollowerMatchesSweep",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FFollowerMatchesSweepTest::RunTest(const FString& Parameters)
{
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	const TArray<FVector2D> TurnPath = SweepAgreement::Turn(1500.0);   // tightest ~1060, above the 510 lock

	// The route the follower drives: straight in, the turn, straight out - long enough that
	// it is running straight into the turn, as Trace's own lead-in assumes.
	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	Plan.Polyline.Add(FVector2D(0.0, -4000.0));
	Plan.Polyline.Append(TurnPath);
	Plan.Polyline.Add(FVector2D(4000.0, 0.0));
	Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

	FRouteFollower Follower;
	Follower.Start(Plan, Bowser.Chassis);

	// The FOLLOWER's sweep: its reported position is the chassis origin, the fixed axle, and
	// the fixed axle's inner end is the deepest a rigid truck reaches inside a turn.
	double FollowerInner = 0.0;
	FVector2D At;
	double Heading = 0.0;
	for (int32 Frame = 0; Frame < 20000 && Follower.Advance(1.0 / 120.0, Bowser.Chassis, At, Heading); ++Frame)
	{
		const FVector2D Across(-FMath::Sin(Heading), FMath::Cos(Heading));
		for (const double Side : { 1.0, -1.0 })
		{
			FollowerInner = FMath::Max(FollowerInner,
				SweepAgreement::InsideOf(TurnPath, At + Across * (Side * Bowser.BodyWidth * 0.5)));
		}
	}

	// The PREDICTION route search gated it on.
	VehicleSweep::FBody Body;
	Body.Wheelbase = Bowser.Chassis.Wheelbase();
	Body.Width = Bowser.BodyWidth;
	Body.FrontX = Bowser.BodyFrontX;
	Body.RearX = Bowser.BodyRearX;
	TArray<double> Inner, Outer;
	TestTrue(TEXT("the sweep traces the turn"), VehicleSweep::Trace(Body, TurnPath, Inner, Outer));
	double Predicted = 0.0;
	for (const double V : Inner) { Predicted = FMath::Max(Predicted, V); }

	AddInfo(FString::Printf(TEXT("rear axle inner end: follower %.1f uu inside the line, sweep predicted %.1f"), FollowerInner, Predicted));
	TestTrue(TEXT("the follower's rear axle does cut inside the line"), FollowerInner > Bowser.BodyWidth * 0.5 + 10.0);
	// 10 uu: the follower integrates at 120 Hz and Trace steps every 10 uu - two discretisations
	// of one model, not two models.
	TestEqual(TEXT("and exactly as far as the sweep route search used predicted"), FollowerInner, Predicted, 10.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeasuredOnFollowerSamplesTest, "Airside.Build.MeasuredOnFollowerSamples",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMeasuredOnFollowerSamplesTest::RunTest(const FString& Parameters)
{
	// The builder measures a turn's clearance, and VehicleFit simulates a vehicle along it, on
	// GuidelineGeom::Sample(A, Control, B). URoadNetwork::SampleGuideline is what the plan - and
	// so the follower - is built from. If the two ever sampled differently, the clearances would
	// be read against points nobody drives through.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Road = URoadProfile::MakeServiceRoadTransient();
	const FRoadNodeId Hub = Net->AddNode(FVector2D(0.0, 0.0));
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(-30000.0, 0.0)), Road);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(30000.0, 0.0)), Road);
	Net->AddStraightSegment(Hub, Net->AddNode(FVector2D(0.0, 30000.0)), Road);
	FRoadGuidelineBuilder::Build(*Net, FRoadNetworkSolver::SolveAll(*Net), UAirsideSettings::ResolveLargestServiceVehicle());

	int32 Checked = 0;
	for (int32 Index = 0; Index < Net->GetGuidelineEdges().Num(); ++Index)
	{
		const FGuidelineEdge& Edge = Net->GetGuidelineEdges()[Index];
		if (!Edge.bAlive || Edge.ClearInnerAt.Num() == 0) { continue; }
		TArray<FVector2D> Walked, Measured;
		Net->SampleGuideline(Net->GuidelineEdgeIdAt(Index), Walked);
		GuidelineGeom::Sample(Net->GetGuidelineNode(Edge.A)->Position, Edge.Control,
			Net->GetGuidelineNode(Edge.B)->Position, Measured);
		++Checked;
		TestTrue(TEXT("a measured turn is measured on exactly the points the follower walks"), Walked == Measured);
		TestEqual(TEXT("one clearance per walked point"), Edge.ClearInnerAt.Num(), Walked.Num());
	}
	TestEqual(TEXT("the four curved turns at a two-lane T were checked"), Checked, 4);
	return true;
}

#endif
