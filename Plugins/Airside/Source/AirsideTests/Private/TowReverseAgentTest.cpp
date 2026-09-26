#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Misc/AutomationTest.h"
#include "Model/AgentMotion.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoadAgent.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficRules.h"
#include "Model/Vehicle.h"
#include "Model/VehicleFit.h"

#if WITH_DEV_AUTOMATION_TESTS

// A TOW REVERSING INSIDE A ROUTE (spec 2026-09-26 §2), driven through a bare FRoadAgent: approach
// forwards, back round a 90 degree bay on a reverse leg, drive out forwards - the shape every
// reverse turn lays. Hand-built plans, so the test pins the agent and not the builder.

namespace TowReverseAgentTest
{
	void AddStraight(TArray<FVector2D>& Out, const FVector2D& From, const FVector2D& To, double Spacing = 50.0)
	{
		const double Length = FVector2D::Distance(From, To);
		const int32 Count = FMath::Max(1, FMath::CeilToInt32(Length / Spacing));
		for (int32 K = Out.Num() > 0 && Out.Last().Equals(From, 0.01) ? 1 : 0; K <= Count; ++K)
		{
			Out.Add(FMath::Lerp(From, To, double(K) / Count));
		}
	}

	/** Appends a quarter circle about Centre from angle A0 to A1 (radians), 30 pieces. */
	void AddArc(TArray<FVector2D>& Out, const FVector2D& Centre, double Radius, double A0, double A1)
	{
		for (int32 K = 1; K <= 30; ++K)
		{
			const double A = FMath::Lerp(A0, A1, K / 30.0);
			Out.Add(Centre + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Radius);
		}
	}

	/**
	 * Approach east along y=0 to (6000,0); reverse west, then round R into a bay running +Y from
	 * x = BayX; exit forwards south along x = BayX. Returns the plan with the middle step flagged.
	 */
	FRoutePlan BayPlan(double Radius, bool bExit = true)
	{
		const double ArcStartX = 6000.0 - 1400.0 - 1000.0;
		const double BayX = ArcStartX - Radius;
		TArray<FVector2D> Approach, Back, Exit;
		AddStraight(Approach, FVector2D(0.0, 0.0), FVector2D(6000.0, 0.0));
		AddStraight(Back, FVector2D(6000.0, 0.0), FVector2D(ArcStartX, 0.0));
		// Centre at (ArcStartX, Radius): from angle -90 deg (the arc start, below the centre)
		// round through 180 deg (due west of it), turning from -X travel to +Y travel.
		AddArc(Back, FVector2D(ArcStartX, Radius), Radius, -UE_HALF_PI, -UE_PI);
		const FVector2D End(BayX, Radius + 2500.0);
		AddStraight(Back, Back.Last(), End);
		AddStraight(Exit, End, FVector2D(BayX, -3000.0));

		FRoutePlan Plan;
		Plan.Result = ERouteResult::Found;
		TArray<FVector2D> All = Approach;
		const int32 ApproachEnd = All.Num() - 1;
		All.Append(Back.GetData() + 1, Back.Num() - 1);
		const int32 BackEnd = All.Num() - 1;
		if (bExit)
		{
			All.Append(Exit.GetData() + 1, Exit.Num() - 1);
		}
		Plan.Polyline = All;
		double Along = 0.0;
		TArray<double> Distances = { 0.0 };
		for (int32 K = 1; K < All.Num(); ++K)
		{
			Along += FVector2D::Distance(All[K - 1], All[K]);
			Distances.Add(Along);
		}
		Plan.Length = Along;
		for (const TPair<int32, bool>& Cut : { TPair<int32, bool>(ApproachEnd, false), TPair<int32, bool>(BackEnd, true),
			TPair<int32, bool>(All.Num() - 1, false) })
		{
			if (!Plan.Steps.IsEmpty() && Cut.Key <= Plan.Steps.Last().EndVertex)
			{
				continue;   // no exit: the reverse is the plan's last step
			}
			FRouteStep Step;
			Step.EndVertex = Cut.Key;
			Step.EndDistance = Distances[Cut.Key];
			Step.bReverseLeg = Cut.Value;
			Plan.Steps.Add(Step);
		}
		return Plan;
	}

	struct FFrame
	{
		EAgentPhase Phase = EAgentPhase::Taxiing;
		FVector2D Position = FVector2D::ZeroVector;
		double Heading = 0.0;
		TArray<FVector2D> Axles;
	};

	/** Drives the agent at 20 Hz until it parks or Cap frames pass, recording every frame. */
	TArray<FFrame> Drive(FRoadAgent& Agent, int32 Cap = 20000)
	{
		TArray<FFrame> Frames;
		FAgentMotion Motion;
		EAgentEvent Event = EAgentEvent::None;
		for (int32 Tick = 0; Tick < Cap && Agent.Phase != EAgentPhase::Parked; ++Tick)
		{
			Agent.Advance(0.05, Motion, Event);
			FFrame& Frame = Frames.AddDefaulted_GetRef();
			Frame.Phase = Agent.Phase;
			Frame.Position = Motion.Position;
			Frame.Heading = Motion.Heading;
			Frame.Axles = Agent.TowAxles;
		}
		return Frames;
	}

	FRoadAgent MakeAgent(const FRoutePlan& Plan, const FVehicle& Vehicle)
	{
		FRoadAgent Agent;
		Agent.StartDrive(Plan, Vehicle);
		Agent.Class = ETraversalClass::GroundVehicle;
		FTrafficRules Rules;
		Rules.ServiceReverseSpeed = 100.0;
		Agent.StampRules(Rules, 10.0);
		return Agent;
	}

	/** The biggest one-frame jump of the pose or any axle beyond what 0.05 s of driving covers. */
	double WorstJump(const TArray<FFrame>& Frames, int32 From, int32 To, double Allowed)
	{
		double Worst = 0.0;
		for (int32 K = FMath::Max(1, From); K <= To && K < Frames.Num(); ++K)
		{
			Worst = FMath::Max(Worst, FVector2D::Distance(Frames[K].Position, Frames[K - 1].Position) - Allowed);
			for (int32 A = 0; A < Frames[K].Axles.Num() && A < Frames[K - 1].Axles.Num(); ++A)
			{
				Worst = FMath::Max(Worst, FVector2D::Distance(Frames[K].Axles[A], Frames[K - 1].Axles[A]) - Allowed);
			}
		}
		return Worst;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseMovesChainTest, "Airside.Model.TowReverse.TowReverseMovesTheChain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseMovesChainTest::RunTest(const FString& Parameters)
{
	// THE FROZEN CHAIN (spec 2026-09-24, "The reverse chain") is what this deletes: every
	// reversing frame at 1 m/s moves the rearmost axle, and the route completes.
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() })
	{
		FRoadAgent Agent = TowReverseAgentTest::MakeAgent(TowReverseAgentTest::BayPlan(1500.0), Vehicle);
		const TArray<TowReverseAgentTest::FFrame> Frames = TowReverseAgentTest::Drive(Agent);
		const FString Name = Vehicle.TypeCode.ToString();
		int32 Reversing = 0;
		int32 Frozen = 0;
		for (int32 K = 1; K < Frames.Num(); ++K)
		{
			if (Frames[K].Phase == EAgentPhase::Reversing && Frames[K - 1].Phase == EAgentPhase::Reversing)
			{
				++Reversing;
				Frozen += FVector2D::Distance(Frames[K].Axles.Last(), Frames[K - 1].Axles.Last()) < 0.5 ? 1 : 0;
			}
		}
		TestTrue(*(Name + TEXT(": it reversed")), Reversing > 100);
		TestEqual(*(Name + TEXT(": no reversing frame leaves the rearmost axle still")), Frozen, 0);
		TestEqual(*(Name + TEXT(": drove the route out and parked")), Agent.Phase, EAgentPhase::Parked);
		TestEqual(*(Name + TEXT(": never jack-knifed")), Agent.GetJackknifedLink(), (int32)INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseNoJumpTest, "Airside.Model.TowReverse.TowReverseHandoverHasNoJump",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseNoJumpTest::RunTest(const FString& Parameters)
{
	// ARMING AND DRIVING ON ARE NOT SNAPS. On both handover frames - forward into the reverse, the
	// reverse into forward - no part of the vehicle moves further than a frame of driving covers
	// (1 m/s reverse, taxi cap forward; 2 uu of slack for the chain's own sub-steps). Review
	// Focus 1: the approach arrives crawling, the reverse starts from rest.
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() })
	{
		FRoadAgent Agent = TowReverseAgentTest::MakeAgent(TowReverseAgentTest::BayPlan(1500.0), Vehicle);
		const TArray<TowReverseAgentTest::FFrame> Frames = TowReverseAgentTest::Drive(Agent);
		const double Allowed = Vehicle.Chassis.Ground.Taxi.SpeedCap * 0.05 + 2.0;
		const FString Name = Vehicle.TypeCode.ToString();
		int32 Handovers = 0;
		for (int32 K = 1; K < Frames.Num(); ++K)
		{
			if (Frames[K].Phase != Frames[K - 1].Phase && Frames[K].Phase != EAgentPhase::Parked)
			{
				++Handovers;
				const double Jump = TowReverseAgentTest::WorstJump(Frames, K, K + 1, Allowed);
				TestTrue(FString::Printf(TEXT("%s: handover at frame %d jumps %.1f uu beyond a frame's travel"), *Name, K, Jump), Jump <= 0.0);
			}
		}
		TestEqual(*(Name + TEXT(": two handovers, into and out of the reverse")), Handovers, 2);
		const double Whole = TowReverseAgentTest::WorstJump(Frames, 1, Frames.Num() - 1, Allowed);
		TestTrue(FString::Printf(TEXT("%s: no frame anywhere jumps (worst %.1f uu beyond travel)"), *Name, Whole), Whole <= 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseRefusalTest, "Airside.Model.TowReverse.TowReverseRefusalStalls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseRefusalTest::RunTest(const FString& Parameters)
{
	// A BAY TOO TIGHT TO BACK INTO stops the rig at the leg's start rather than driving the
	// reverse leg forwards - the crab the rigid arm already refuses.
	FRoadAgent Agent = TowReverseAgentTest::MakeAgent(TowReverseAgentTest::BayPlan(250.0), UAirsideSettings::ResolveRigVehicle());
	const TArray<TowReverseAgentTest::FFrame> Frames = TowReverseAgentTest::Drive(Agent, 3000);
	TestEqual(TEXT("still taxiing - never entered the reverse"), Agent.Phase, EAgentPhase::Taxiing);
	TestFalse(TEXT("nothing armed"), Agent.TowReverse.IsArmed());
	TestTrue(TEXT("stopped"), Agent.Follower.Speed < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowReverseRigidTest, "Airside.Model.TowReverse.RigidStillUsesReverseRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowReverseRigidTest::RunTest(const FString& Parameters)
{
	// A RIGID VEHICLE is untouched: the bowser on the same route arms FReverseRun, never the tow run.
	FRoadAgent Agent = TowReverseAgentTest::MakeAgent(TowReverseAgentTest::BayPlan(1500.0), UAirsideSettings::ResolveDefaultVehicle());
	FAgentMotion Motion;
	EAgentEvent Event = EAgentEvent::None;
	bool bSawReverse = false;
	for (int32 Tick = 0; Tick < 20000 && Agent.Phase != EAgentPhase::Parked; ++Tick)
	{
		Agent.Advance(0.05, Motion, Event);
		if (Agent.Phase == EAgentPhase::Reversing)
		{
			bSawReverse = true;
			TestTrue(TEXT("FReverseRun carries the leg"), Agent.Reverse.Plan.IsValid());
			TestFalse(TEXT("the tow run is never armed"), Agent.TowReverse.IsArmed());
			break;
		}
	}
	TestTrue(TEXT("the bowser reversed"), bSawReverse);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowWholeRouteReverseTest, "Airside.Model.Tow.WholeRouteJudgesTheReverse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowWholeRouteReverseTest::RunTest(const FString& Parameters)
{
	// THE ROUTER JUDGES WHAT THE AGENT PLAYS (spec §2): a bay both tows can back into is admitted,
	// and the agent then drives the admitted route to the end without a refusal or a fold; a bay
	// too tight is refused as ReverseUnsolvable, naming TowReverse's reason. Hand-built plans
	// carry no edge handles, so no clearance is judged - the reverse is the whole question here.
	URoadNetwork* Network = NewObject<URoadNetwork>();
	for (const FVehicle& Vehicle : { UAirsideSettings::ResolveRigVehicle(), UAirsideSettings::ResolveUtilityTowVehicle() })
	{
		const FString Name = Vehicle.TypeCode.ToString();
		const FRoutePlan Good = TowReverseAgentTest::BayPlan(1500.0);
		const FFitVerdict Admitted = VehicleFit::JudgePlan(Good, Vehicle, *Network);
		TestTrue(*(Name + TEXT(": R1500 bay admitted - ") + Admitted.Describe()), Admitted.Fits());

		FRoadAgent Agent = TowReverseAgentTest::MakeAgent(Good, Vehicle);
		TowReverseAgentTest::Drive(Agent);
		TestEqual(*(Name + TEXT(": the agent drives the admitted route out")), Agent.Phase, EAgentPhase::Parked);
		TestEqual(*(Name + TEXT(": without a fold")), Agent.GetJackknifedLink(), (int32)INDEX_NONE);
	}
	const FFitVerdict Refused = VehicleFit::JudgePlan(TowReverseAgentTest::BayPlan(250.0), UAirsideSettings::ResolveRigVehicle(), *Network);
	TestEqual(TEXT("an R250 bay refused as a reverse"), Refused.Refusal, EFitRefusal::ReverseUnsolvable);
	TestFalse(TEXT("the refusal carries TowReverse's reason"), Refused.Reason.IsEmpty());
	TestTrue(*(TEXT("and says so: ") + Refused.Describe()), Refused.Describe().Contains(TEXT("reverse")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowWholeRouteTrailingReverseTest, "Airside.Model.Tow.WholeRouteJudgesATrailingReverse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowWholeRouteTrailingReverseTest::RunTest(const FString& Parameters)
{
	// Review Focus 5: a route whose GOAL is the bay - the reverse is its last step, nothing after
	// it. Judged (a tight one is still refused), and no index runs past the end.
	URoadNetwork* Network = NewObject<URoadNetwork>();
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	TestTrue(TEXT("a trailing R1500 reverse is admitted"), VehicleFit::JudgePlan(TowReverseAgentTest::BayPlan(1500.0, false), Rig, *Network).Fits());
	TestEqual(TEXT("a trailing R250 reverse is refused"),
		VehicleFit::JudgePlan(TowReverseAgentTest::BayPlan(250.0, false), Rig, *Network).Refusal, EFitRefusal::ReverseUnsolvable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTowJudgeReverseEdgeTest, "Airside.Model.Tow.JudgeSaysNothingForAReverseEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTowJudgeReverseEdgeTest::RunTest(const FString& Parameters)
{
	// A reverse edge with a tight measured radius is NOT refused on the forward lock - backing is
	// judged by JudgePlan's solve. The same edge unflagged is refused, which is the control.
	URoadNetwork* Network = NewObject<URoadNetwork>();
	FGuidelineEdge Edge;
	Edge.Width = 1000.0;
	Edge.MinRadius = 50.0;
	Edge.bReverseLeg = true;
	const FVehicle Rig = UAirsideSettings::ResolveRigVehicle();
	TestTrue(TEXT("reverse edge: nothing said"), VehicleFit::Judge(Edge, Rig, *Network).Fits());
	Edge.bReverseLeg = false;
	TestEqual(TEXT("control: the same edge forwards is tighter than the lock"),
		VehicleFit::Judge(Edge, Rig, *Network).Refusal, EFitRefusal::TighterThanLock);
	return true;
}

#endif
