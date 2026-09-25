#include "CoreMinimal.h"
#include "Build/RoadGuidelineBuilder.h"
#include "Build/RoadNetworkSolver.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Model/AgentMotion.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteFollower.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
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

// THE TOW IS A CHAIN (spec 2026-09-24 §4, revised). The rig is one link, a drawbar trailer two,
// and route gating (VehicleSweep::Trace) and the driving agent (FRoadAgent) step the SAME chain
// with the SAME stepper. These tests hold the agent to that: it starts straight, drives the
// trailer where Trace said it would go, steps its chain in sub-steps rather than once a frame,
// and stops - loudly - when a link folds past square.

namespace TowChain
{
	/**
	 * Straight in, TurnPath, straight out - 2500 uu each way, and ALONG THE PATH'S OWN END
	 * CHORDS, which is how Trace leads in and out. Not FollowerMatchesSweep's axis-aligned
	 * legs: the sampled quadratic's first chord is ~2.3 degrees off its tangent, and a trailer
	 * 10 m long carries a lead-in that differs by that much 120 uu off Trace's line and well
	 * into the turn (measured 2026-09-24) - a difference in the ROUTE, not in the stepper.
	 */
	FRoutePlan PlanThrough(const TArray<FVector2D>& TurnPath)
	{
		const FVector2D In = (TurnPath[1] - TurnPath[0]).GetSafeNormal();
		const FVector2D Out = (TurnPath.Last() - TurnPath[TurnPath.Num() - 2]).GetSafeNormal();
		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		Plan.Polyline.Add(TurnPath[0] - In * 2500.0);
		Plan.Polyline.Append(TurnPath);
		Plan.Polyline.Add(TurnPath.Last() + Out * 2500.0);
		Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);
		return Plan;
	}

	/**
	 * A utility tug with a DRAWBAR trailer, HAND FIGURES for this test only (the real
	 * utility1 + fuelTrailer1 figures are measured in a later task): the towbar couples 91 uu
	 * BEHIND the tug's fixed axle and is 150 uu to the trailer's steered front axle, and the
	 * body's own wheelbase is 221 uu. The tug is the bowser's chassis.
	 */
	FVehicle Drawbar()
	{
		FVehicle Tug = UAirsideSettings::ResolveDefaultVehicle();
		Tug.TypeCode = TEXT("DRAWBAR");
		FTowLink Bar;
		Bar.HitchX = -91.0; Bar.Length = 150.0; Bar.Width = 10.0;   // a bar: no body
		FTowLink Body;
		Body.HitchX = 0.0; Body.Length = 221.0; Body.BodyFront = 60.0; Body.BodyRear = 70.0; Body.Width = 160.0;
		Tug.Tow = { Bar, Body };
		return Tug;
	}

	/** Distance from Point to the polyline Line; bPastEnd when it projects beyond either end. */
	double DistanceTo(TArrayView<const FVector2D> Line, const FVector2D& Point, bool& bPastEnd)
	{
		double Best = TNumericLimits<double>::Max();
		bPastEnd = true;
		for (int32 Span = 0; Span + 1 < Line.Num(); ++Span)
		{
			const FVector2D AB = Line[Span + 1] - Line[Span];
			const double Len2 = AB.SizeSquared();
			if (Len2 <= UE_DOUBLE_SMALL_NUMBER) { continue; }
			const double T = FVector2D::DotProduct(Point - Line[Span], AB) / Len2;
			const double D = FVector2D::Distance(Point, Line[Span] + AB * FMath::Clamp(T, 0.0, 1.0));
			if (D < Best)
			{
				Best = D;
				bPastEnd = (Span == 0 && T < 0.0) || (Span + 2 == Line.Num() && T > 1.0);
			}
		}
		return Best;
	}

	/** Watches LogAirside for WARNINGS. Unbuffered - see FLogLineSpy for the #216 flake. */
	struct FWarningSpy : public FOutputDevice
	{
		TArray<FString> Lines;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == FName(TEXT("LogAirside")) && Verbosity == ELogVerbosity::Warning)
			{
				Lines.Add(FString(V));
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowRigStartsStraightTest, "Airside.Model.Tow.RigStartsStraight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowRigStartsStraightTest::RunTest(const FString& Parameters)
{
	// SPAWNED STRAIGHT (spec §1): the chain is laid dead behind the cab once, at dispatch, and
	// never re-derived from the route afterwards.
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	TestEqual(TEXT("the rig is ONE link - its figures moved into the chain unchanged"), Rig.Tow.Num(), 1);
	if (Rig.Tow.Num() != 1) { return false; }
	TestEqual(TEXT("kingpin 57.3 ahead of the drive axle"), Rig.Tow[0].HitchX, 57.3);
	TestEqual(TEXT("1029.5 kingpin to tandem"), Rig.Tow[0].Length, 1029.5);

	FRoadAgent Agent;
	const FRoutePlan Plan = TowChain::PlanThrough(SweepAgreement::Turn(1500.0));
	Agent.StartDrive(Plan, Rig);
	if (!TestEqual(TEXT("one axle carried per link"), Agent.TowAxles.Num(), 1)) { return false; }

	// The steered axle on the plan's first point, facing along its first span; the fixed axle
	// a wheelbase (370) behind, the kingpin 57.3 ahead of that, the tandem 1029.5 behind the
	// kingpin - 1342.2 behind the steered axle, on the same line.
	const FVector2D Along = (Plan.Polyline[1] - Plan.Polyline[0]).GetSafeNormal();
	const FVector2D Expected = Plan.Polyline[0] - Along * (370.0 - 57.3 + 1029.5);
	TestTrue(*FString::Printf(TEXT("the trailer axle starts dead behind the cab, a trailer-length behind the kingpin (%.2f,%.2f) vs (%.2f,%.2f)"),
		Agent.TowAxles[0].X, Agent.TowAxles[0].Y, Expected.X, Expected.Y), Agent.TowAxles[0].Equals(Expected, 0.01));

	FAgentMotion Motion;
	EAgentEvent Event;
	Agent.Advance(1.0 / 30.0, Motion, Event);
	if (!TestEqual(TEXT("the motion carries one pose per link"), Motion.Tow.Num(), 1)) { return false; }
	TestEqual(TEXT("the trailer is in line with the cab: hitch angle zero"),
		FMath::UnwindRadians(Motion.Tow[0].Heading - Motion.Heading), 0.0, 1.0e-6);
	const FVector2D Forward(FMath::Cos(Motion.Heading), FMath::Sin(Motion.Heading));
	TestTrue(TEXT("the hitch sits 57.3 ahead of the drive axle (the chassis origin)"),
		Motion.Tow[0].Hitch.Equals(Motion.Position + Forward * 57.3, 0.01));
	TestEqual(TEXT("and the axle a trailer-length behind the hitch"),
		FVector2D::Distance(Motion.Tow[0].Hitch, Motion.Tow[0].Axle), 1029.5, 0.01);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowFollowerMatchesTraceTest, "Airside.Model.Tow.FollowerMatchesTraceForRig",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowFollowerMatchesTraceTest::RunTest(const FString& Parameters)
{
	// FollowerMatchesSweep, for the trailer: route search admitted the rig on Trace's trailer
	// path, so the agent must drive THAT path, not one of its own. One stepper (StepTrailer)
	// and one chain walk (StepChain) serve both; what is left to differ is the step length.
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const TArray<FVector2D> TurnPath = SweepAgreement::Turn(1500.0);

	TArray<double> Inner, Outer;
	TArray<FVector2D> TraceAxles;
	if (!TestTrue(TEXT("Trace takes the rig round the turn"),
		VehicleSweep::Trace(VehicleFit::BodyOf(Rig), TurnPath, Inner, Outer, &TraceAxles))) { return false; }
	TestTrue(TEXT("and reports one trailer axle per step"), TraceAxles.Num() > 100);

	FRoadAgent Agent;
	Agent.StartDrive(TowChain::PlanThrough(TurnPath), Rig);
	double Worst = 0.0;
	int32 Compared = 0;
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	for (int32 Frame = 0; Frame < 20000 && Event != EAgentEvent::Parked; ++Frame)
	{
		Agent.Advance(1.0 / 30.0, Motion, Event);
		bool bPastEnd = false;
		const double D = TowChain::DistanceTo(TraceAxles, Agent.TowAxles[0], bPastEnd);
		if (!bPastEnd)
		{
			Worst = FMath::Max(Worst, D);
			++Compared;
		}
	}
	AddInfo(FString::Printf(TEXT("trailer axle: %d samples, worst %.2f uu off Trace's path"), Compared, Worst));
	TestTrue(TEXT("the agent drove the whole turn"), Compared > 100);
	TestEqual(TEXT("its jack-knife guard never fired"), Agent.GetJackknifedLink(), static_cast<int32>(INDEX_NONE));
	// 5 uu: Trace steps every 10 uu, the agent every sub-step (TraceStep over its speed cap, so
	// never longer) - two discretisations of one pursuit, not two models of it.
	TestTrue(TEXT("and its trailer axle rode Trace's trailer path within 5 uu"), Worst <= 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowDrawbarChainFollowsTest, "Airside.Model.Tow.DrawbarChainFollows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowDrawbarChainFollowsTest::RunTest(const FString& Parameters)
{
	// A CHAIN, NOT A COPY: link 1 is pulled by link 0's hitch point, not by the tug. Stepped
	// off the tug directly, the body would ride the towbar's path instead of cutting inside it.
	const FVehicle Tug = TowChain::Drawbar();
	const TArray<FVector2D> TurnPath = SweepAgreement::Turn(1500.0);

	FRoadAgent Agent;
	Agent.StartDrive(TowChain::PlanThrough(TurnPath), Tug);
	if (!TestEqual(TEXT("two axles, one per link"), Agent.TowAxles.Num(), 2)) { return false; }

	double BarInside = 0.0, BodyInside = 0.0, WorstLength = 0.0;
	int32 Samples = 0;
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	for (int32 Frame = 0; Frame < 20000 && Event != EAgentEvent::Parked; ++Frame)
	{
		Agent.Advance(1.0 / 30.0, Motion, Event);
		if (Motion.Tow.Num() != 2) { continue; }
		++Samples;
		for (int32 Link = 0; Link < 2; ++Link)
		{
			WorstLength = FMath::Max(WorstLength, FMath::Abs(
				FVector2D::Distance(Motion.Tow[Link].Hitch, Motion.Tow[Link].Axle) - Tug.Tow[Link].Length));
		}
		BarInside = FMath::Max(BarInside, SweepAgreement::InsideOf(TurnPath, Motion.Tow[0].Axle));
		BodyInside = FMath::Max(BodyInside, SweepAgreement::InsideOf(TurnPath, Motion.Tow[1].Axle));
	}
	AddInfo(FString::Printf(TEXT("drawbar: towbar axle %.1f uu inside, body axle %.1f uu inside"), BarInside, BodyInside));
	TestTrue(TEXT("the drawbar drove the turn"), Samples > 100);
	TestEqual(TEXT("no link folded"), Agent.GetJackknifedLink(), static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("every axle stayed exactly its link's length from its hitch, every sample"), WorstLength < 0.01);
	TestTrue(TEXT("the towbar's axle cuts inside the line"), BarInside > 0.0);
	TestTrue(TEXT("and the body's axle cuts further inside than the towbar's - pulled by the chain"), BodyInside > BarInside + 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowLongFrameIsSubSteppedTest, "Airside.Model.Tow.LongFrameIsSubStepped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowLongFrameIsSubSteppedTest::RunTest(const FString& Parameters)
{
	// A PURSUIT IS FIRST-ORDER IN ITS STEP. Stepped once a frame, a 2 s hitch would drag the
	// kingpin metres in one step and the trailer would land wherever that jump put it. Stepped
	// in sub-steps together with the cab, one long frame is many short ones.
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), TowChain::Drawbar() })
	{
		FRoadAgent Long;
		Long.StartDrive(TowChain::PlanThrough(SweepAgreement::Turn(1500.0)), Vehicle);
		FAgentMotion Motion;
		EAgentEvent Event;
		// Into the turn at speed, so the long frame is spent cornering.
		for (int32 Frame = 0; Frame < 20000 && Long.Follower.Travelled < 2300.0; ++Frame)
		{
			Long.Advance(1.0 / 30.0, Motion, Event);
		}
		FRoadAgent Short = Long;

		FAgentMotion LongMotion;
		Long.Advance(2.0, LongMotion, Event);
		FAgentMotion ShortMotion;
		for (int32 Step = 0; Step < 20; ++Step)
		{
			Short.Advance(0.1, ShortMotion, Event);
		}

		const FString Name = Vehicle.TypeCode.ToString() + FString::Printf(TEXT(" (%d links)"), Vehicle.Tow.Num());
		AddInfo(FString::Printf(TEXT("%s: cab %.4f uu apart after 2 s"), *Name,
			FVector2D::Distance(LongMotion.Position, ShortMotion.Position)));
		TestTrue(*(Name + TEXT(": the long frame moved the cab into the turn")), Long.Follower.Travelled > 3000.0);
		TestTrue(*(Name + TEXT(": the cab ends within 1 uu either way")),
			LongMotion.Position.Equals(ShortMotion.Position, 1.0));
		if (!TestEqual(*(Name + TEXT(": both carry the chain")), Long.TowAxles.Num(), Short.TowAxles.Num())) { continue; }
		for (int32 Link = 0; Link < Long.TowAxles.Num(); ++Link)
		{
			AddInfo(FString::Printf(TEXT("%s: link %d %.4f uu apart"), *Name, Link,
				FVector2D::Distance(Long.TowAxles[Link], Short.TowAxles[Link])));
			TestTrue(*FString::Printf(TEXT("%s: link %d's axle ends within 1 uu either way"), *Name, Link),
				Long.TowAxles[Link].Equals(Short.TowAxles[Link], 1.0));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowRigidHasNoTowTest, "Airside.Model.Tow.RigidHasNoTow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowRigidHasNoTowTest::RunTest(const FString& Parameters)
{
	// A RIGID VEHICLE TAKES THE PATH IT ALWAYS TOOK: no chain, no sub-steps, one follower call
	// per Advance - so its pose is BITWISE the bare follower's, and every agent test written
	// before tows existed still measures the code it always measured.
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	TestFalse(TEXT("a bowser has no tow"), Bowser.HasTrailer());
	const FRoutePlan Plan = TowChain::PlanThrough(SweepAgreement::Turn(1500.0));

	FRoadAgent Agent;
	Agent.StartDrive(Plan, Bowser);
	TestEqual(TEXT("and carries no axles"), Agent.TowAxles.Num(), 0);

	FRouteFollower Bare;
	Bare.Start(Plan, Bowser.Chassis);
	FAgentMotion Motion;
	EAgentEvent Event;
	bool bBitwise = true;
	bool bNoPoses = true;
	for (int32 Frame = 0; Frame < 40; ++Frame)
	{
		// LONG FRAMES ON PURPOSE: a sub-stepped rigid agent would show here as a different pose.
		const double Dt = Frame % 2 == 0 ? 0.5 : 1.0 / 30.0;
		Agent.Advance(Dt, Motion, Event);
		FVector2D At;
		double Heading = 0.0;
		Bare.Advance(Dt, Bowser.Chassis, At, Heading);
		bBitwise &= Motion.Position == At && Motion.Heading == Heading;
		bNoPoses &= Motion.Tow.Num() == 0;
	}
	TestTrue(TEXT("its pose is bitwise the bare follower's, long frames included"), bBitwise);
	TestTrue(TEXT("and its motion carries no tow poses"), bNoPoses);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowJackknifeStopsTest, "Airside.Model.Tow.JackknifeStops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowJackknifeStopsTest::RunTest(const FString& Parameters)
{
	// A HAIRPIN TIGHTER THAN THE TRAILER, driven in small steps: a 700 uu circle is inside the
	// cab's lock (576) but puts the kingpin on ~597, well under the 1029.5 trailer, so the hitch
	// angle grows GRADUALLY until the link is past square to the cab. No single step is long -
	// this is the fold at tick cadence, the one the agent will actually meet.
	FRoutePlan Plan;
	Plan.Result = ERouteResult::Found;
	Plan.Polyline.Add(FVector2D(0.0, -4000.0));
	const double R = 700.0;
	for (int32 Deg = 0; Deg <= 330; Deg += 3)
	{
		const double A = FMath::DegreesToRadians(static_cast<double>(Deg));
		Plan.Polyline.Add(FVector2D(-R + R * FMath::Cos(A), R * FMath::Sin(A)));
	}
	Plan.Length = GuidelineGeom::PolylineLength(Plan.Polyline);

	TowChain::FWarningSpy Spy;
	GLog->AddOutputDevice(&Spy);
	FRoadAgent Agent;
	Agent.StartDrive(Plan, UAirsideSettings::ResolveRigVehicle());
	FAgentMotion Motion;
	EAgentEvent Event;
	for (int32 Frame = 0; Frame < 3000 && Agent.GetJackknifedLink() == INDEX_NONE; ++Frame)
	{
		Agent.Advance(1.0 / 30.0, Motion, Event);
	}
	const FAgentMotion Folded = Motion;
	const double TravelledAtFold = Agent.Follower.Travelled;
	for (int32 More = 0; More < 60; ++More)
	{
		Agent.Advance(1.0 / 30.0, Motion, Event);
	}
	GLog->RemoveOutputDevice(&Spy);

	TestEqual(TEXT("the trailer - link 0 - jack-knifed"), Agent.GetJackknifedLink(), 0);
	TestTrue(TEXT("part-way round, not at the end of the route"), TravelledAtFold < Plan.Length - 100.0);
	TestTrue(TEXT("and the agent stopped where it folded"), Motion.Position.Equals(Folded.Position, 0.001));
	TestEqual(TEXT("with its wheels still"), Motion.GroundSpeed, 0.0);
	TestEqual(TEXT("and went no further along its route"), Agent.Follower.Travelled, TravelledAtFold);
	const FString* Line = Spy.Lines.FindByPredicate([](const FString& L) { return L.Contains(TEXT("jack-knifed at link 0")); });
	TestNotNull(TEXT("LogAirside warned that the tow jack-knifed"), Line);
	TestEqual(TEXT("once, not every frame after"), Spy.Lines.FilterByPredicate(
		[](const FString& L) { return L.Contains(TEXT("jack-knifed")); }).Num(), 1);
	if (Line != nullptr)
	{
		AddInfo(*Line);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowRedirectKeepsChainAndHeadingTest, "Airside.Model.Tow.RedirectKeepsChainAndHeading",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowRedirectKeepsChainAndHeadingTest::RunTest(const FString& Parameters)
{
	// A REDIRECT IS NOT A NEW VEHICLE (the rig test course's continuous run, 2026-09-25): a rig
	// parked at the end of one leg and sent on along the next keeps its trailer where it is AND
	// its cab facing the way it faced. The next leg starts with a 45 degree jog, the shape of
	// the course's width step, because that is where seeding the cab from the new line swung
	// its fixed axle sideways and folded the chain on the first frame.
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();

	FRoutePlan First;
	First.Result = ERouteResult::Found;
	for (double X = 0.0; X <= 4000.0; X += 100.0)
	{
		First.Polyline.Add(FVector2D(X, 0.0));
	}
	First.Length = GuidelineGeom::PolylineLength(First.Polyline);
	const int32 Id = Traffic->DispatchAgent(nullptr, First, Rig, ETraversalClass::GroundVehicle, 0.0);
	if (!TestTrue(TEXT("dispatched"), Id > 0)) { return false; }
	for (int32 Tick = 0; Tick < 4000 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(0.05, nullptr);
	}
	if (!TestEqual(TEXT("the rig parked at the end of its first leg"),
		static_cast<int32>(Traffic->FindAgent(Id)->Phase), static_cast<int32>(EAgentPhase::Parked))) { return false; }
	const TArray<FVector2D> Before = Traffic->FindAgent(Id)->TowAxles;
	const double HeadingBefore = Traffic->FindAgent(Id)->Follower.Heading;

	FRoutePlan Next;
	Next.Result = ERouteResult::Found;
	Next.Polyline.Add(First.Polyline.Last());
	Next.Polyline.Add(First.Polyline.Last() + FVector2D(100.0, 100.0));
	for (double X = 4200.0; X <= 8000.0; X += 100.0)
	{
		Next.Polyline.Add(FVector2D(X, 100.0));
	}
	Next.Length = GuidelineGeom::PolylineLength(Next.Polyline);
	if (!TestTrue(TEXT("the redirect is accepted"), Traffic->RedirectAgent(Id, nullptr, Next))) { return false; }

	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	if (!TestEqual(TEXT("the chain is still one axle per link"), Agent->TowAxles.Num(), Before.Num())) { return false; }
	for (int32 I = 0; I < Before.Num(); ++I)
	{
		TestTrue(*FString::Printf(TEXT("link %d's axle did not move in the redirect - the chain was not re-laid (%.2f uu)"),
			I, FVector2D::Distance(Agent->TowAxles[I], Before[I])), Agent->TowAxles[I] == Before[I]);
	}
	TestEqual(TEXT("the cab kept its heading rather than snapping to the jog's 45 degrees"),
		Agent->Follower.Heading, HeadingBefore);

	// And it drives the jog without folding, its axles moving no further per tick than it travels.
	double WorstStep = 0.0;
	TArray<FVector2D> Prev = Agent->TowAxles;
	for (int32 Tick = 0; Tick < 400 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(0.05, nullptr);
		Agent = Traffic->FindAgent(Id);
		if (Tick < 2)
		{
			for (int32 I = 0; I < Prev.Num(); ++I)
			{
				WorstStep = FMath::Max(WorstStep, FVector2D::Distance(Agent->TowAxles[I], Prev[I]));
			}
		}
		Prev = Agent->TowAxles;
	}
	TestEqual(TEXT("the tow never jack-knifed on the jog"), Agent->GetJackknifedLink(), static_cast<int32>(INDEX_NONE));
	TestTrue(*FString::Printf(TEXT("pulling away from rest, no axle moved more than a sub-step (%.2f uu)"), WorstStep),
		WorstStep <= VehicleSweep::TraceStep);
	return true;
}

#endif
