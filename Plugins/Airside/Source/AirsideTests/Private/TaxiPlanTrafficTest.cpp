#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/TaxiPlanning.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Spec 2026-10-02 (space-time taxi planning) §4, PR 2: the plans FLOWN - order enforced in the claim pass, the arrival
// and departure clearances, arrivals first. World-free: a NewObject network and a NewObject UGroundTraffic, ticked.

namespace
{
	/** A stand-in for traffic these tests do not fly: windows booked under this holder hold ground and nothing else. */
	constexpr int32 TaxiStandIn = 900;

	/** One authored lane - authored so nothing derived sweeps it away. */
	FGuidelineEdgeId TaxiLane(URoadNetwork& Net, FGuidelineNodeId A, FGuidelineNodeId B)
	{
		TestGraph::FJoinOptions Options;
		Options.bDerived = false;
		return TestGraph::Join(Net, A, B, Options);
	}

	FTaxiRequest TaxiRequest(FGuidelineNodeId Start, FGuidelineNodeId Goal, double DepartAt)
	{
		FTaxiRequest Out;
		Out.Start = Start;
		Out.Goal = Goal;
		Out.Errand = ERouteErrand::ArrivalTaxiIn;
		Out.DepartAt = DepartAt;
		return Out;
	}

	double DistanceTo(const UGroundTraffic& Traffic, int32 Id, const FVector2D& At)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(Id);
		return Agent != nullptr ? FVector2D::Distance(Agent->LastMotion.Position, At) : TNumericLimits<double>::Max();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanOwnedTest, "Airside.Model.TaxiPlan.PlanningIsTheTrafficsOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanOwnedTest::RunTest(const FString& Parameters)
{
	// THE SEAM: UGroundTraffic makes its planning owner in PostInitProperties. A Transient pointer is reset to the
	// CDO's on a duplication (PIE duplicates the level), so a duplicate must make its own - not share the source's, and
	// not be left with the CDO's (memory: transient subobject pointers reset on duplication).
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	TestNotNull(TEXT("a live traffic model has a planning owner"), Traffic->GetTaxiPlanning());
	TestTrue(TEXT("its own"), Traffic->GetTaxiPlanning() != nullptr && Traffic->GetTaxiPlanning()->GetOuter() == Traffic);
	TestNull(TEXT("the CDO has none"), GetDefault<UGroundTraffic>()->GetTaxiPlanning());
	UGroundTraffic* Copy = DuplicateObject<UGroundTraffic>(Traffic, GetTransientPackage());
	TestTrue(TEXT("a duplicate makes its own"), Copy->GetTaxiPlanning() != nullptr
		&& Copy->GetTaxiPlanning() != Traffic->GetTaxiPlanning() && Copy->GetTaxiPlanning()->GetOuter() == Copy);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanNotMyTurnTest, "Airside.Model.TaxiPlan.NotMyTurn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanNotMyTurnTest::RunTest(const FString& Parameters)
{
	//                 D (0, 30000)
	//                 |
	//   A (-30000) ---J (0,0)--- B (30000)
	//                 |
	//                 C (0, -12000)
	// X goes A to B, Y goes C to D; both cross J. X is planned FIRST and Y booked through J after it - but Y starts much
	// nearer J and is let go at once. The claim pass alone would let Y through first (nobody is there yet); the ORDER
	// holds Y until X has gone: "not my turn: waiting for X".
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 30000.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 0.0, -12000.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 0.0, 30000.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	TaxiLane(*Net, A, J);
	TaxiLane(*Net, J, B);
	TaxiLane(*Net, C, J);
	TaxiLane(*Net, J, D);

	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	if (!TestNotNull(TEXT("a planning owner"), Planning))
	{
		return false;
	}

	const FTaxiPlan PlanX = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, B, 0.0));
	const int32 X = PlanX.IsPlanned() ? Traffic->DispatchAgent(Net, PlanX.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("X planned, dispatched and booked"), X > 0
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiOut, PlanX, ETaxiClearanceStage::Moving)))
	{
		return false;
	}
	// Y's plan sets off at 30 s, so it reaches J after X - but Y is dispatched NOW, and nothing physical stops it
	// driving straight to J and through it long before X gets there. Only the order does.
	const FTaxiPlan PlanY = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(C, D, 30.0));
	TestTrue(TEXT("Y, planned second, is booked through J after X"), PlanY.IsPlanned() && PlanX.Legs.Num() > 0
		&& PlanY.Legs.Num() > 0 && PlanY.Legs[0].Reach > PlanX.Legs[0].Reach);
	const int32 Y = PlanY.IsPlanned() ? Traffic->DispatchAgent(Net, PlanY.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("Y dispatched and booked"), Y > 0
		&& Planning->Book(*Net, Y, ETaxiClearanceKind::TaxiOut, PlanY, ETaxiClearanceStage::Moving)))
	{
		return false;
	}

	const FVector2D JAt = Net->GetGuidelineNode(J)->Position;
	double XAtJ = -1.0;
	double YAtJ = -1.0;
	bool bYWaitedForX = false;
	double Now = 0.0;
	for (int32 Tick = 0; Tick < 30 * 300 && (XAtJ < 0.0 || YAtJ < 0.0); ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		Now += 1.0 / 30.0;
		if (const FRoadAgent* YAgent = Traffic->FindAgent(Y))
		{
			bYWaitedForX |= YAgent->GetWaitingOn() == X;
		}
		XAtJ = (XAtJ < 0.0 && DistanceTo(*Traffic, X, JAt) < 500.0) ? Now : XAtJ;
		YAtJ = (YAtJ < 0.0 && DistanceTo(*Traffic, Y, JAt) < 500.0) ? Now : YAtJ;
	}
	TestTrue(TEXT("Y waited ON X - the order's refusal names who it waits for"), bYWaitedForX);
	TestTrue(FString::Printf(TEXT("X reached J first (%.1f s), Y after (%.1f s) - as planned, not as near"), XAtJ, YAtJ),
		XAtJ > 0.0 && YAtJ > XAtJ);

	// RELEASED BEHIND THE TAIL: once both are well past, nothing of theirs is left on J.
	for (int32 Tick = 0; Tick < 30 * 60; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	TestEqual(TEXT("J's windows released as each tail cleared it"),
		Planning->GetTable().WindowsOn(FTaxiResource::Node(J)).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanArrivalsFirstTest, "Airside.Model.TaxiPlan.ArrivalRevokesUnstartedDeparture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanArrivalsFirstTest::RunTest(const FString& Parameters)
{
	// ARRIVALS FIRST (spec ruling 2): a departure booked and still on its stand - not yet pushing - may lose its plan
	// to an arrival that cannot otherwise be planned. A moving one never: its plan is what everyone behind it waits on.
	// The departure is a stand-in holding the arrival's EXIT for ever (a departure bound for that runway entry).
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Piper);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const FArrivalPlan Landing = ArrivalPlanner::Plan(*Airport.Net, Airport.Threshold, Piper, &Traffic->GetOccupancy());
	if (!TestNotNull(TEXT("a planning owner"), Planning) || !TestTrue(TEXT("the field takes an arrival"), Landing.IsValid()))
	{
		return false;
	}

	FTaxiPlan Hold;
	Hold.Result = ETaxiPlanResult::Planned;
	Hold.Passes = { { FTaxiResource::Node(Landing.Exit), { TaxiStandIn, 0.0, FTaxiReservations::Forever } } };
	TestTrue(TEXT("a departure on its stand is booked through the exit"),
		Planning->Book(*Airport.Net, TaxiStandIn, ETaxiClearanceKind::TaxiOut, Hold, ETaxiClearanceStage::Booked));
	TestEqual(TEXT("the arrival could be cleared - by revoking it"),
		Traffic->TaxiInRefusal(*Airport.Net, Landing, Piper), EArrivalRefusal::None);
	const int32 Arrival = Traffic->DispatchArrival(*Airport.Net, Airport.Threshold, Piper, 0.0);
	TestTrue(TEXT("the arrival is dispatched"), Arrival > 0);
	TestNull(TEXT("the unstarted departure's plan is revoked"), Planning->Find(TaxiStandIn));
	TestTrue(TEXT("and the arrival's is booked"), Planning->Find(Arrival) != nullptr);
	bool bStandInLeft = false;
	for (const FTaxiWindow& Window : Planning->GetTable().WindowsOn(FTaxiResource::Node(Landing.Exit)))
	{
		bStandInLeft |= Window.Holder == TaxiStandIn;
	}
	TestFalse(TEXT("nothing of it is left on the exit"), bStandInLeft);

	// A MOVING ONE NEVER. A fresh field, the same exit held by one already under way.
	const FTestAirport Second = FTestAirport::Build(Piper);
	UGroundTraffic* Busy = NewObject<UGroundTraffic>(GetTransientPackage());
	FGroundTrafficTestAccess BusyAccess(*Busy);
	const FArrivalPlan Again = ArrivalPlanner::Plan(*Second.Net, Second.Threshold, Piper, &Busy->GetOccupancy());
	FTaxiPlan Moving = Hold;
	Moving.Passes = { { FTaxiResource::Node(Again.Exit), { TaxiStandIn, 0.0, FTaxiReservations::Forever } } };
	TestTrue(TEXT("a departure already moving is booked through the exit"), BusyAccess.TaxiPlanning() != nullptr
		&& BusyAccess.TaxiPlanning()->Book(*Second.Net, TaxiStandIn, ETaxiClearanceKind::TaxiOut, Moving, ETaxiClearanceStage::Moving));
	TestEqual(TEXT("the arrival keeps holding - no taxi plan"), Busy->TaxiInRefusal(*Second.Net, Again, Piper),
		EArrivalRefusal::NoTaxiPlan);
	AddExpectedMessage(TEXT("taxi"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	TestEqual(TEXT("and is not dispatched"), Busy->DispatchArrival(*Second.Net, Second.Threshold, Piper, 0.0), 0);
	TestTrue(TEXT("the moving departure keeps its plan"), Busy->GetTaxiPlanning() != nullptr
		&& Busy->GetTaxiPlanning()->Find(TaxiStandIn) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanPushGatedTest, "Airside.Model.TaxiPlan.PushbackGatedOnPlan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanPushGatedTest::RunTest(const FString& Parameters)
{
	// PUSHBACK IS GATED ON A PLAN (spec §1): a departure's push ground is its plan's first window. Something holds the
	// junction it pushes through for two minutes: the departure is cleared (booked) to push once it frees - and stays on
	// its stand until then, rather than pushing into it. PushbackDepartTest's shape: runway B, junction J, stand A, arm E.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();
	const FRoadNodeId RA = Net->AddNode(FVector2D(-50000.0, 0.0));
	const FRoadNodeId RM = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId RB = Net->AddNode(FVector2D(50000.0, 0.0));
	Net->AddStraightSegment(RA, RM, Runway);
	Net->AddStraightSegment(RM, RB, Runway);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, -10000.0);
	const FGuidelineNodeId A = TestGraph::Node(*Net, 0.0, -20000.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 20000.0, -10000.0);
	TaxiLane(*Net, A, J);
	TaxiLane(*Net, J, B);
	TaxiLane(*Net, J, E);

	// Taxied in from the runway, so it parks facing away from its way out: a push.
	const int32 Id = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, B, A, ETraversalClass::Aircraft),
		TestAirframes::Piper(), ETraversalClass::Aircraft, 0.0);
	for (int32 I = 0; I < 20000 && Id > 0 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++I)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("an aircraft parks facing away from its way out"), Id > 0 && Traffic->FindAgent(Id)->Phase == EAgentPhase::Parked))
	{
		return false;
	}

	FGroundTrafficTestAccess Access(*Traffic);
	const double Now = Traffic->GetSimSeconds();
	const double FreeAt = Now + 120.0;
	const TArray<FTaxiPass> Busy = { { FTaxiResource::Node(J), { TaxiStandIn, Now, FreeAt } } };
	TestTrue(TEXT("something holds the junction for two minutes"), Access.TaxiPlanning() != nullptr
		&& Access.TaxiPlanning()->BookPassesForTest(Busy));

	TestEqual(TEXT("the departure is cleared to push LATER - refused for now as push ground"),
		Traffic->DepartAgent(Id, *Net), EDepartureRefusal::PushbackBlocked);
	const FTaxiClearance* Cleared = Traffic->GetTaxiPlanning() != nullptr ? Traffic->GetTaxiPlanning()->Find(Id) : nullptr;
	TestTrue(TEXT("booked, not moving"), Cleared != nullptr && Cleared->Stage == ETaxiClearanceStage::Booked);
	TestTrue(TEXT("its push timed after the junction frees"), Cleared != nullptr && Cleared->Plan.PushAt >= FreeAt);

	double PushedAt = -1.0;
	for (int32 I = 0; I < 30 * 240 && PushedAt < 0.0; ++I)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		// THE STAND-IN PASSES at FreeAt: as traffic tracked by the planner is released when its tail clears, so is this.
		if (Traffic->GetSimSeconds() >= FreeAt)
		{
			Access.TaxiPlanning()->Drop(TaxiStandIn, nullptr);
		}
		if (Traffic->FindAgent(Id) != nullptr && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked)
		{
			PushedAt = Traffic->GetSimSeconds();
		}
	}
	TestTrue(FString::Printf(TEXT("it pushed (at %.1f s), and not before the junction freed (%.1f s)"), PushedAt, FreeAt),
		PushedAt >= FreeAt);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanMovingNotRevokedTest, "Airside.Model.TaxiPlan.StartedDepartureIsNeverRevoked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanMovingNotRevokedTest::RunTest(const FString& Parameters)
{
	// RULING 2 ON THE REAL PATH (review of #528 finding 1): a departure whose push DepartAgent has just started is Moving
	// AT ONCE - not at the next tick's Track - because the arrival queue dispatches BETWEEN ticks: an arrival in the same
	// frame revoked a departure already rolling back off its stand, and the order behind it lost what it waited on.
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Piper, { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const int32 First = Traffic->DispatchArrival(*Airport.Net, Airport.Threshold, Piper, 0.0);
	if (!TestNotNull(TEXT("a planning owner"), Planning) || !TestTrue(TEXT("the first arrival is dispatched"), First > 0))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 900 && Traffic->FindAgent(First)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	if (!TestTrue(TEXT("it lands and parks"), Traffic->FindAgent(First)->Phase == EAgentPhase::Parked))
	{
		return false;
	}

	if (!TestEqual(TEXT("ordered off, it starts at once (nothing else is booked)"), Traffic->DepartAgent(First, *Airport.Net),
		EDepartureRefusal::None))
	{
		return false;
	}
	const FTaxiClearance* Cleared = Planning->Find(First);
	TestTrue(TEXT("started: its clearance is Moving in the same call, before any tick"), Cleared != nullptr
		&& Cleared->Stage == ETaxiClearanceStage::Moving);
	TestFalse(TEXT("so it can no longer be revoked"), Planning->Revoke(First, 999));

	// AND THE ARRIVAL QUEUE'S OWN CALL, in the same frame: whatever it is told, the moving departure keeps its plan.
	const int32 Second = Traffic->DispatchArrival(*Airport.Net, Airport.Threshold, Piper, 0.0);
	TestTrue(FString::Printf(TEXT("the departure keeps its plan through a same-frame arrival (dispatched as %d)"), Second),
		Planning->Find(First) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanCommittedTest, "Airside.Model.TaxiPlan.CommittedMoveIsNotReordered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanCommittedTest::RunTest(const FString& Parameters)
{
	// AN EARLY AIRCRAFT INSIDE ITS STOPPING DISTANCE KEEPS ITS TURN (review of #528 finding 2). X, planned to cross J at
	// two minutes, is let go at once - early. As it closes on J, past the point it could stop short, Y is planned through
	// J from now: the gap before X's late window used to let Y be booked AHEAD of it there, the order then refused X with
	// no room to stop (the claim pass floors the stop at 0), and X halted on J's approach - its claims blocking Y, Y's turn
	// blocking it: an order/occupancy cycle. Now X's move is committed when its window reaches it, its windows there
	// pulled to now, and Y is planned behind it.
	//                 D (0, 30000)
	//                 |
	//   A (-30000) ---J (0,0)--- B (30000)
	//                 |
	//                 C (0, -12000)
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 30000.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 0.0, -12000.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 0.0, 30000.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	TaxiLane(*Net, A, J);
	TaxiLane(*Net, J, B);
	TaxiLane(*Net, C, J);
	TaxiLane(*Net, J, D);

	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	if (!TestNotNull(TEXT("a planning owner"), Planning))
	{
		return false;
	}
	const FTaxiPlan PlanX = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, B, 120.0));
	const int32 X = PlanX.IsPlanned() ? Traffic->DispatchAgent(Net, PlanX.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("X planned late, let go now"), X > 0
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiOut, PlanX, ETaxiClearanceStage::Moving)))
	{
		return false;
	}

	const FVector2D JAt = Net->GetGuidelineNode(J)->Position;
	const FVector2D BAt = Net->GetGuidelineNode(B)->Position;
	const FVector2D DAt = Net->GetGuidelineNode(D)->Position;
	const double G = Traffic->Rules.GapFor(ETraversalClass::Aircraft);
	const double F = Traffic->Rules.FootprintFor(ETraversalClass::Aircraft);
	const double Decel = FMath::Max(Piper.Chassis.Ground.Taxi.Decel, 1.0);
	int32 Y = 0;
	double XAtJ = -1.0;
	double YAtJ = -1.0;
	bool bXStoppedShort = false;
	bool bXWaitedForY = false;
	bool bBothHome = false;
	for (int32 Tick = 0; Tick < 30 * 600 && !bBothHome; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const double Now = Traffic->GetSimSeconds();
		const FRoadAgent* XAgent = Traffic->FindAgent(X);
		if (XAgent == nullptr)
		{
			break;
		}
		const double ToJ = FVector2D::Distance(XAgent->GroundPosition(), JAt);
		const double Speed = XAgent->SpeedAlongPlan();
		if (Y == 0 && XAtJ < 0.0 && ToJ > F * 0.5 && ToJ < Speed * Speed / (2.0 * Decel) + G)
		{
			// INSIDE ITS STOPPING DISTANCE OF J: Y planned through J from now, and let go.
			const FTaxiPlan PlanY = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(C, D, Now));
			Y = PlanY.IsPlanned() ? Traffic->DispatchAgent(Net, PlanY.Route, Piper, ETraversalClass::Aircraft, 0.0) : -1;
			if (Y > 0)
			{
				Planning->Book(*Net, Y, ETaxiClearanceKind::TaxiOut, PlanY, ETaxiClearanceStage::Moving, FRoutePlan(), Now);
			}
		}
		if (Y > 0 && XAtJ < 0.0)
		{
			bXStoppedShort |= FMath::Abs(Speed) < 1.0;
			bXWaitedForY |= XAgent->GetWaitingOn() == Y;
		}
		XAtJ = (XAtJ < 0.0 && ToJ < 500.0) ? Now : XAtJ;
		YAtJ = (Y > 0 && YAtJ < 0.0 && DistanceTo(*Traffic, Y, JAt) < 500.0) ? Now : YAtJ;
		bBothHome = Y > 0 && DistanceTo(*Traffic, X, BAt) < 500.0 && DistanceTo(*Traffic, Y, DAt) < 500.0;
	}
	if (!TestTrue(TEXT("Y was planned and let go while X was inside its stopping distance of J"), Y > 0))
	{
		return false;
	}
	TestFalse(TEXT("X never waited on Y once committed"), bXWaitedForY);
	TestFalse(TEXT("X never stopped short of J once committed"), bXStoppedShort);
	TestTrue(FString::Printf(TEXT("X crossed J first (%.1f s), Y after (%.1f s)"), XAtJ, YAtJ), XAtJ > 0.0 && YAtJ > XAtJ);
	TestTrue(TEXT("both reached their goals - no order/occupancy cycle"), bBothHome);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
