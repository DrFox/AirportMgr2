#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"
#include "Solve/GuidelineGeom.h"
#include "Solve/VehicleSweep.h"

#if WITH_DEV_AUTOMATION_TESTS

// THE WHOLE-ROUTE TOW CHECK (2026-09-25; spec 2026-09-24 Open, "one evaluator for a whole
// route's tow"). Per edge, VehicleFit lays a trailer straight at the start of every curve, so
// two curves that each hold it can still fold it together - measured on the reshaped dead-end
// balloons, where the router admitted the rig and it jack-knifed in all three. These tests hold
// the router to the agent: it judges the plan the agent will drive, with the chain it will
// carry, and admits a tow only where that drive holds.
//
// Hand-drawn quadratic edges with their MinRadius measured as the builder measures it
// (GuidelineGeom::TightestRadius), so the per-edge lock gate is live and every curve here
// clears the rig's 575.6 uu lock on its own. No per-sample clearances: this is the fold half.

namespace WholeRouteTowFixture
{
	/**
	 * Leg of each 90 degree quadratic, uu: tightest radius 0.707 x 830 = 587, clearing the rig's
	 * 575.6 lock by 11 uu - a curve the rig is admitted to on its own, as the reshaped balloons'
	 * 642-666 uu pieces were.
	 */
	constexpr double Leg = 830.0;

	FGuidelineNodeId Node(URoadNetwork& Net, double X, double Y)
	{
		return Net.AddGuidelineNode(FVector2D(X, Y), /*bDerived=*/false);
	}

	/** One way, A to B, through Control (the midpoint for a straight), MinRadius measured. */
	FGuidelineEdgeId Edge(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B, const FVector2D* Control = nullptr)
	{
		FGuidelineEdge Edge;
		Edge.A = A;
		Edge.B = B;
		const FVector2D PA = Net.GetGuidelineNode(A)->Position;
		const FVector2D PB = Net.GetGuidelineNode(B)->Position;
		Edge.Control = Control != nullptr ? *Control : (PA + PB) * 0.5;
		Edge.MinRadius = Control != nullptr ? GuidelineGeom::TightestRadius(PA, Edge.Control, PB) : 0.0;
		Edge.AllowedTraffic = FTrafficMask::All();
		Edge.Direction = EGuidelineDir::AToB;
		Edge.bDerived = false;
		return Net.AddGuidelineEdge(MoveTemp(Edge));
	}

	/** Every edge a network's graph needs to name, and the two ends of the drive. */
	struct FGraph
	{
		URoadNetwork* Net = nullptr;
		FGuidelineNodeId Start;
		FGuidelineNodeId Goal;
		/** The quarters, in driving order. */
		TArray<FGuidelineEdgeId> Curves;
		/** The straight laid before quarter GapBefore, when there is one. */
		FGuidelineEdgeId Gap;
	};

	/**
	 * Adds one quarter to Net from From (at At, heading Heading, unit): Leg along the heading to
	 * the control point, Leg to the side - left for Hand +1, right for -1. At and Heading move on.
	 */
	FGuidelineNodeId Quarter(URoadNetwork& Net, FGuidelineNodeId From, FVector2D& At, FVector2D& Heading, int32 Hand,
		FGuidelineEdgeId& OutEdge)
	{
		const FVector2D Side = FVector2D(-Heading.Y, Heading.X) * static_cast<double>(Hand);
		const FVector2D Control = At + Heading * Leg;
		At = Control + Side * Leg;
		Heading = Side;
		const FGuidelineNodeId To = Node(Net, At.X, At.Y);
		OutEdge = Edge(Net, From, To, &Control);
		return To;
	}

	/**
	 * Straight in along +X to (0,0), then a quarter per entry of Hands (+1 left, -1 right), with
	 * Gap uu of straight laid before quarter GapBefore (INDEX_NONE: none), and 4000 uu out.
	 * {+1, +1} is a U, {+1, -1} an S, {+1, +1, +1} three quarters of a loop - a balloon's turn.
	 */
	FGraph Bends(const TArray<int32>& Hands, int32 GapBefore = INDEX_NONE, double Gap = 0.0)
	{
		FGraph Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *Out.Net;
		Out.Start = Node(Net, -4000.0, 0.0);
		FGuidelineNodeId From = Node(Net, 0.0, 0.0);
		Edge(Net, Out.Start, From);
		FVector2D At(0.0, 0.0);
		FVector2D Heading(1.0, 0.0);
		int32 Index = 0;
		for (const int32 Hand : Hands)
		{
			if (Index++ == GapBefore)
			{
				At += Heading * Gap;
				const FGuidelineNodeId To = Node(Net, At.X, At.Y);
				Out.Gap = Edge(Net, From, To);
				From = To;
			}
			From = Quarter(Net, From, At, Heading, Hand, Out.Curves.AddDefaulted_GetRef());
		}
		At += Heading * 4000.0;
		Out.Goal = Node(Net, At.X, At.Y);
		Edge(Net, From, Out.Goal);
		return Out;
	}

	FRoutePlan Route(const URoadNetwork& Net, FGuidelineNodeId From, FGuidelineNodeId To, const FVehicle* Vehicle,
		ETraversalClass Class = ETraversalClass::GroundVehicle, double Wingspan = 0.0)
	{
		FRouteQuery Query = FRouteQuery::For(ERouteErrand::GraphProbe, From, To, Wingspan, Class);
		if (Vehicle != nullptr)
		{
			Query.WithVehicle(*Vehicle);
		}
		return RouteSearch::Find(Net, Query);
	}

	/** The edge's own samples, as Judge traces them. */
	TArray<FVector2D> Samples(const URoadNetwork& Net, FGuidelineEdgeId Id)
	{
		const FGuidelineEdge* Edge = Net.GetGuidelineEdge(Id);
		TArray<FVector2D> Out;
		GuidelineGeom::Sample(Net.GetGuidelineNode(Edge->A)->Position, Edge->Control, Net.GetGuidelineNode(Edge->B)->Position, Out);
		return Out;
	}

	/** Whether the agent, dispatched on Plan and driven in frames of Dt, jack-knifes before it parks. */
	bool AgentFolds(const FRoutePlan& Plan, const FVehicle& Vehicle, double Dt, double& OutTravelled)
	{
		FRoadAgent Agent;
		Agent.StartDrive(Plan, Vehicle);
		FAgentMotion Motion;
		EAgentEvent Event = EAgentEvent::None;
		const int32 Frames = FMath::CeilToInt32(600.0 / Dt);
		for (int32 Frame = 0; Frame < Frames && Event != EAgentEvent::Parked && Agent.GetJackknifedLink() == INDEX_NONE; ++Frame)
		{
			Agent.Advance(Dt, Motion, Event);
		}
		OutTravelled = Agent.Follower.Travelled;
		return Agent.GetJackknifedLink() != INDEX_NONE;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWholeRouteFoldRefusedTest, "Airside.Model.Tow.WholeRouteFoldRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWholeRouteFoldRefusedTest::RunTest(const FString& Parameters)
{
	// Scoped per test, not per file: this module is a UNITY build, and a file-scope using would
	// leak Node/Edge/Route into whichever test file is compiled after this one.
	using namespace WholeRouteTowFixture;
	// THREE SAME-HAND QUARTERS BACK TO BACK - three quarters of a loop, the turn a dead-end
	// balloon makes. Each quarter holds the rig from a straight start (47 deg); each starts with
	// the trailer already swung by the last, and the three together fold it. (Two - a U - hold
	// at 75 deg: Airside.Model.Tow.WholeRouteVerdictIsTheAgents lists the shapes and angles.)
	//
	// NOT AN S, which the brief proposed: an S RELIEVES the trailer. After a left quarter the
	// trailer lags right of the cab; the right quarter swings the cab back toward it, so the
	// hitch angle falls through zero instead of adding up. Traced in the 2026-09-25 Python
	// prototype (worst 43 deg on the S against 47 on one quarter alone) and asserted below.
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const FGraph Loop = Bends({ 1, 1, 1 });

	for (const FGuidelineEdgeId Curve : Loop.Curves)
	{
		const FGuidelineEdge* Edge = Loop.Net->GetGuidelineEdge(Curve);
		TestTrue(*FString::Printf(TEXT("each quarter clears the lock on its own (%.0f uu vs %.0f)"),
			Edge->MinRadius, Rig.Chassis.TightestFollowableRadius()), VehicleFit::Fits(*Edge, Rig, *Loop.Net));
		TArray<double> Inner, Outer;
		TestTrue(TEXT("and one curve's own trace, laid straight at its start, holds the trailer"),
			VehicleSweep::Trace(VehicleFit::BodyOf(Rig), Samples(*Loop.Net, Curve), Inner, Outer));
	}

	RouteSearch::ResetTowCheckCountForTest();
	const FRoutePlan Plan = Route(*Loop.Net, Loop.Start, Loop.Goal, &Rig);
	AddInfo(FString::Printf(TEXT("three quarters: result %d, %s"), static_cast<int32>(Plan.Result), *Plan.RejectedBy.Describe()));
	TestEqual(TEXT("the rig is refused the three quarters: TooNarrow, as any route it does not fit"),
		static_cast<int32>(Plan.Result), static_cast<int32>(ERouteResult::TooNarrow));
	TestTrue(TEXT("on the whole-route rule"), Plan.RejectedBy.bWholeRoute);
	TestEqual(TEXT("because its trailer folds"),
		static_cast<int32>(Plan.RejectedBy.Refusal), static_cast<int32>(EFitRefusal::TrailerFolds));
	TestEqual(TEXT("link 0, the semi-trailer"), Plan.RejectedBy.Link, 0);
	TestTrue(TEXT("past square"), Plan.RejectedBy.Radians > VehicleSweep::MaxHitchRadians);
	TestTrue(TEXT("on the THIRD quarter, the one the cab is driving when the trailer goes - the first two hold it"),
		Plan.RejectedEdge == Loop.Curves.Last());
	TestTrue(TEXT("and the reason says where"),
		Plan.RejectedBy.Describe().StartsWith(TEXT("trailer folds at guideline node")));
	TestEqual(TEXT("the check ran once: with the fold's edge excluded no route was left to judge"),
		RouteSearch::TowCheckCountForTest(), 1);

	// THE S: the same quarters, the second turned the other way. Admitted, and with margin.
	const FGraph S = Bends({ 1, -1 });
	const FRoutePlan SPlan = Route(*S.Net, S.Start, S.Goal, &Rig);
	if (TestTrue(TEXT("the S is admitted: opposite curves relieve the trailer, they do not fold it"), SPlan.IsValid()))
	{
		const FFitVerdict Verdict = VehicleFit::JudgePlan(SPlan, Rig, *S.Net);
		AddInfo(FString::Printf(TEXT("S: worst hitch %.1f deg"), FMath::RadiansToDegrees(Verdict.Radians)));
		TestTrue(TEXT("with its worst hitch angle well short of square"), Verdict.Radians < FMath::DegreesToRadians(70.0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWholeRouteRetriesTest, "Airside.Model.Tow.WholeRouteRetriesRoundAFold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWholeRouteRetriesTest::RunTest(const FString& Parameters)
{
	using namespace WholeRouteTowFixture;
	// THREE QUARTERS AND A WAY ROUND THEM: after the first two quarters, the third straight
	// away (the shortest route, which folds), or 800 uu of straight first and then the third -
	// the gap lets the trailer come back toward line, and the drive holds (75 deg). Both lanes
	// meet again at one goal, so the search prefers the fold and must be sent round it.
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FGuidelineNodeId Start = Node(*Net, -4000.0, 0.0);
	const FGuidelineNodeId P0 = Node(*Net, 0.0, 0.0);
	Edge(*Net, Start, P0);
	FVector2D At(0.0, 0.0);
	FVector2D Heading(1.0, 0.0);
	FGuidelineEdgeId First, SecondQuarter;
	const FGuidelineNodeId P1 = Quarter(*Net, P0, At, Heading, 1, First);
	const FGuidelineNodeId P2 = Quarter(*Net, P1, At, Heading, 1, SecondQuarter);

	FVector2D TightAt = At;
	FVector2D TightHeading = Heading;
	FGuidelineEdgeId TightQuarter;
	const FGuidelineNodeId P3 = Quarter(*Net, P2, TightAt, TightHeading, 1, TightQuarter);

	const double Gap = 800.0;
	FVector2D WideAt = At + Heading * Gap;
	FVector2D WideHeading = Heading;
	const FGuidelineNodeId P2b = Node(*Net, WideAt.X, WideAt.Y);
	const FGuidelineEdgeId Straight = Edge(*Net, P2, P2b);
	FGuidelineEdgeId RelaxedQuarter;
	const FGuidelineNodeId P3b = Quarter(*Net, P2b, WideAt, WideHeading, 1, RelaxedQuarter);

	const FVector2D GoalAt = (TightAt + WideAt) * 0.5 + TightHeading * 6000.0;
	const FGuidelineNodeId Goal = Node(*Net, GoalAt.X, GoalAt.Y);
	Edge(*Net, P3, Goal);
	Edge(*Net, P3b, Goal);

	const FRoutePlan Unjudged = Route(*Net, Start, Goal, nullptr);
	TestTrue(TEXT("unjudged, the shortest route takes the tight quarter"),
		Unjudged.Steps.ContainsByPredicate([&](const FRouteStep& Step) { return Step.Edge == TightQuarter; }));

	RouteSearch::ResetTowCheckCountForTest();
	const FRoutePlan Plan = Route(*Net, Start, Goal, &Rig);
	if (!TestTrue(TEXT("the rig is routed: a way round exists"), Plan.IsValid())) { return false; }
	TestFalse(TEXT("not over the quarter it folds on"),
		Plan.Steps.ContainsByPredicate([&](const FRouteStep& Step) { return Step.Edge == TightQuarter; }));
	TestTrue(TEXT("but by the straight that lets the trailer come back in line"),
		Plan.Steps.ContainsByPredicate([&](const FRouteStep& Step) { return Step.Edge == Straight; }));
	// THE SHARED ENTRY SURVIVES: the fold's own edge was excluded, not the quarter where the
	// angle began to build, which both routes need (RouteSearch.cpp, CheckWholeRouteTow).
	TestTrue(TEXT("through the entry quarters both routes share"),
		Plan.Steps.ContainsByPredicate([&](const FRouteStep& Step) { return Step.Edge == First; })
		&& Plan.Steps.ContainsByPredicate([&](const FRouteStep& Step) { return Step.Edge == SecondQuarter; }));
	TestTrue(TEXT("longer than the one that folds"), Plan.Length > Unjudged.Length);
	TestEqual(TEXT("two whole-route checks: the fold, then the way round"), RouteSearch::TowCheckCountForTest(), 2);

	double Travelled = 0.0;
	TestFalse(TEXT("and the agent drives the way round without folding"),
		AgentFolds(Plan, Rig, 1.0 / 30.0, Travelled));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWholeRouteSkipsRigidTest, "Airside.Model.Tow.WholeRouteSkipsRigidAndAircraft",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWholeRouteSkipsRigidTest::RunTest(const FString& Parameters)
{
	using namespace WholeRouteTowFixture;
	// BIT-IDENTICAL ROUTING FOR EVERYTHING WITHOUT A TOW: the check is never run, so neither a
	// rigid vehicle's route nor an aircraft's can have moved. Measured on the three quarters that refuse the
	// rig, where a check that ran would have something to say.
	const FGraph Loop = Bends({ 1, 1, 1 });
	const FVehicle Bowser = UAirsideSettings::ResolveDefaultVehicle();
	TestFalse(TEXT("the bowser is rigid"), Bowser.HasTrailer());

	const FRoutePlan Bare = Route(*Loop.Net, Loop.Start, Loop.Goal, nullptr);
	RouteSearch::ResetTowCheckCountForTest();
	const FRoutePlan Truck = Route(*Loop.Net, Loop.Start, Loop.Goal, &Bowser);
	const FRoutePlan Plane = Route(*Loop.Net, Loop.Start, Loop.Goal, nullptr, ETraversalClass::Aircraft, 1100.0);
	TestEqual(TEXT("no whole-route check ran for the bowser or the aircraft"), RouteSearch::TowCheckCountForTest(), 0);

	for (const FRoutePlan* Plan : { &Truck, &Plane })
	{
		const TCHAR* Who = Plan == &Truck ? TEXT("bowser") : TEXT("aircraft");
		if (!TestTrue(*FString::Printf(TEXT("the %s drives the three quarters"), Who), Plan->IsValid())) { continue; }
		bool bSame = Plan->Steps.Num() == Bare.Steps.Num() && Plan->Polyline == Bare.Polyline && Plan->Length == Bare.Length;
		for (int32 Index = 0; bSame && Index < Plan->Steps.Num(); ++Index)
		{
			bSame = Plan->Steps[Index].Edge == Bare.Steps[Index].Edge;
		}
		TestTrue(*FString::Printf(TEXT("the %s takes exactly the route an ungated query gets, bitwise"), Who), bSame);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWholeRouteVerdictIsTheAgentsTest, "Airside.Model.Tow.WholeRouteVerdictIsTheAgents",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWholeRouteVerdictIsTheAgentsTest::RunTest(const FString& Parameters)
{
	using namespace WholeRouteTowFixture;
	// ONE EVALUATOR: the router's whole-route verdict IS whether the agent it dispatches folds.
	// Plan for plan, over shapes that fold and shapes that hold - one quarter to a full loop, the
	// S, a balloon's out-round-and-back, and three quarters with a straight before the third
	// that grows until the fold becomes a hold (between 0 and 200 uu, measured 2026-09-25).
	//
	// TWO CADENCES. Frames of exactly one tow sub-step are the check's own steps: the agent
	// then takes BITWISE the check's walk, and must fold at the same route distance, not merely
	// agree yes or no. Frames of 1/30 s are the game's: four sub-steps a frame, a finer walk of
	// the same pursuit, and the verdicts must still agree.
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	const double SubStep = VehicleFit::TowSubStepSeconds(Rig.Chassis);

	struct FCase { const TCHAR* Name; TArray<int32> Hands; int32 GapBefore; double Gap; };
	const FCase Cases[] = {
		{ TEXT("one quarter"), { 1 }, INDEX_NONE, 0.0 },
		{ TEXT("U"), { 1, 1 }, INDEX_NONE, 0.0 },
		{ TEXT("S"), { 1, -1 }, INDEX_NONE, 0.0 },
		{ TEXT("three quarters"), { 1, 1, 1 }, INDEX_NONE, 0.0 },
		{ TEXT("three quarters, 200 uu gap before the third"), { 1, 1, 1 }, 2, 200.0 },
		{ TEXT("three quarters, 400 uu gap before the third"), { 1, 1, 1 }, 2, 400.0 },
		{ TEXT("three quarters, 800 uu gap before the third"), { 1, 1, 1 }, 2, 800.0 },
		{ TEXT("three quarters, 1600 uu gap before the third"), { 1, 1, 1 }, 2, 1600.0 },
		{ TEXT("full loop"), { 1, 1, 1, 1 }, INDEX_NONE, 0.0 },
		{ TEXT("balloon shape"), { -1, 1, 1, 1, -1 }, INDEX_NONE, 0.0 },
	};
	int32 Folds = 0;
	int32 Holds = 0;
	for (const FCase& Case : Cases)
	{
		const FGraph Graph = Bends(Case.Hands, Case.GapBefore, Case.Gap);
		// UNJUDGED, so a plan that folds still exists to compare against.
		const FRoutePlan Plan = Route(*Graph.Net, Graph.Start, Graph.Goal, nullptr);
		if (!TestTrue(*FString::Printf(TEXT("%s: a plan"), Case.Name), Plan.IsValid())) { continue; }

		const FFitVerdict Verdict = VehicleFit::JudgePlan(Plan, Rig, *Graph.Net);
		const bool bRouterFolds = Verdict.Refusal == EFitRefusal::TrailerFolds;
		double StepTravelled = 0.0;
		double FrameTravelled = 0.0;
		const bool bAgentFoldsAtSubStep = AgentFolds(Plan, Rig, SubStep, StepTravelled);
		const bool bAgentFoldsAtFrame = AgentFolds(Plan, Rig, 1.0 / 30.0, FrameTravelled);
		AddInfo(FString::Printf(TEXT("%s: router %s (%.1f deg, %.1f uu); agent at sub-step %s (%.1f uu), at 1/30 s %s (%.1f uu)"),
			Case.Name, bRouterFolds ? TEXT("FOLDS") : TEXT("holds"), FMath::RadiansToDegrees(Verdict.Radians), Verdict.Along,
			bAgentFoldsAtSubStep ? TEXT("folds") : TEXT("holds"), StepTravelled,
			bAgentFoldsAtFrame ? TEXT("folds") : TEXT("holds"), FrameTravelled));

		TestEqual(*FString::Printf(TEXT("%s: the router's verdict is the agent's, at the check's own step"), Case.Name),
			bRouterFolds, bAgentFoldsAtSubStep);
		if (bRouterFolds && bAgentFoldsAtSubStep)
		{
			TestEqual(*FString::Printf(TEXT("%s: and it folds at the same route distance, bitwise"), Case.Name),
				Verdict.Along, StepTravelled);
		}
		TestEqual(*FString::Printf(TEXT("%s: and the agent at the game's 1/30 s frames agrees"), Case.Name),
			bRouterFolds, bAgentFoldsAtFrame);
		(bRouterFolds ? Folds : Holds) += 1;
	}
	// THE COST, measured not asserted: the longest plan here judged 20 times, per check and per
	// metre of route. A wall-clock bound would be a flaky test; the number is for the report.
	{
		const FGraph Graph = Bends({ -1, 1, 1, 1, -1 });
		const FRoutePlan Plan = Route(*Graph.Net, Graph.Start, Graph.Goal, nullptr);
		const double Began = FPlatformTime::Seconds();
		for (int32 Repeat = 0; Repeat < 20; ++Repeat)
		{
			VehicleFit::JudgePlan(Plan, Rig, *Graph.Net);
		}
		const double Each = (FPlatformTime::Seconds() - Began) / 20.0;
		AddInfo(FString::Printf(TEXT("cost: %.3f ms per whole-route check over %.0f m (%.1f us per metre)"),
			Each * 1000.0, Plan.Length / 100.0, Each * 1.0e6 / (Plan.Length / 100.0)));
	}

	// NOT VACUOUS: a set where every plan held (or every one folded) would agree for any check.
	TestTrue(TEXT("the set holds at least one fold"), Folds > 0);
	TestTrue(TEXT("and at least one route that holds"), Holds > 0);
	return true;
}

#endif
