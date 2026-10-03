#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Entities/EntityDefinition.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/InspectFacts.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanRetimeTest, "Airside.Model.TaxiPlan.LateAircraftIsRetimed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanRetimeTest::RunTest(const FString& Parameters)
{
	// SPEC §4's DELAY: lag one aircraft 60 s - the order holds, the other waits, nothing deadlocks, and the re-time fires.
	// X and Y cross at J (NotMyTurn's cross), X booked first; X is held still for 60 s from the start. Y, booked behind X
	// at J, waits for it however early it gets there; X's remaining windows are moved later once it runs more than the
	// knob late, and Y's with them - same order, later times.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 30000.0, 0.0);
	const FGuidelineNodeId C = TestGraph::Node(*Net, 0.0, -30000.0);
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
	const bool bX = X > 0 && Planning->Book(*Net, X, ETaxiClearanceKind::TaxiOut, PlanX, ETaxiClearanceStage::Moving);
	const FTaxiPlan PlanY = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(C, D, 0.0));
	const int32 Y = PlanY.IsPlanned() ? Traffic->DispatchAgent(Net, PlanY.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("X and Y planned, dispatched and booked, Y behind X at J"), bX && Y > 0
		&& Planning->Book(*Net, Y, ETaxiClearanceKind::TaxiOut, PlanY, ETaxiClearanceStage::Moving)
		&& PlanY.Legs.Num() > 0 && PlanX.Legs.Num() > 0 && PlanY.Legs[0].Reach > PlanX.Legs[0].Reach))
	{
		return false;
	}
	Planning->DelayForTest(X, 60.0);
	const double XWas = PlanX.Arrival;
	const double YWas = PlanY.Arrival;

	const FVector2D JAt = Net->GetGuidelineNode(J)->Position;
	double XAtJ = -1.0;
	double YAtJ = -1.0;
	double XRetimed = 0.0;
	double YRetimed = 0.0;
	for (int32 Tick = 0; Tick < 30 * 400 && (XAtJ < 0.0 || YAtJ < 0.0); ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		const double Now = Traffic->GetSimSeconds();
		if (const FTaxiClearance* Cx = Planning->Find(X))
		{
			XRetimed = FMath::Max(XRetimed, Cx->Plan.Arrival - XWas);
		}
		if (const FTaxiClearance* Cy = Planning->Find(Y))
		{
			YRetimed = FMath::Max(YRetimed, Cy->Plan.Arrival - YWas);
		}
		XAtJ = (XAtJ < 0.0 && DistanceTo(*Traffic, X, JAt) < 500.0) ? Now : XAtJ;
		YAtJ = (YAtJ < 0.0 && DistanceTo(*Traffic, Y, JAt) < 500.0) ? Now : YAtJ;
	}
	TestTrue(FString::Printf(TEXT("X re-timed later (by %.0f s)"), XRetimed), XRetimed >= 30.0);
	TestTrue(FString::Printf(TEXT("and Y, booked behind it, with it (by %.0f s)"), YRetimed), YRetimed >= 15.0);
	TestTrue(FString::Printf(TEXT("the order held: X through J first (%.0f s), Y after (%.0f s)"), XAtJ, YAtJ),
		XAtJ > 60.0 && YAtJ > XAtJ);
	TestEqual(TEXT("nobody deadlocked"), Traffic->GetDeadlockLogLinesForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanReplanKeepsTest, "Airside.Model.TaxiPlan.ResolverReplanKeepsAPlan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanReplanKeepsTest::RunTest(const FString& Parameters)
{
	// A ROUTE CHANGED UNDER A PLANNED AIRCRAFT IS RE-PLANNED, not dropped (taxi planning PR 3; PR 2 dropped it to
	// unplanned). The resolver's own splice (ReplanAt, the call it makes of a cycle member) sends X round by E instead of
	// straight on to D; at the next tick X holds a plan along the new route.
	//   A ---- B ---- D
	//           \    /
	//             E
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 30000.0, 0.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 15000.0, -15000.0);
	TaxiLane(*Net, A, B);
	const FGuidelineEdgeId BD = TaxiLane(*Net, B, D);
	TaxiLane(*Net, B, E);
	const FGuidelineEdgeId ED = TaxiLane(*Net, E, D);

	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const FTaxiPlan Plan = Planning != nullptr ? Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, D, 0.0)) : FTaxiPlan();
	const int32 X = Plan.IsPlanned() ? Traffic->DispatchAgent(Net, Plan.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("X planned straight on, dispatched and booked"), X > 0 && Plan.Route.Steps.Num() == 2
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiOut, Plan, ETaxiClearanceStage::Moving)))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 5; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("the resolver's splice sends it round by E"), Access.ReplanAt(X, *Net, 1, BD)))
	{
		return false;
	}
	Traffic->Advance(1.0 / 30.0, Net);
	const FTaxiClearance* Now = Planning->Find(X);
	TestNull(TEXT("not unplanned"), Planning->FindUnplanned(X));
	TestTrue(TEXT("still planned - along the new route, round by E"), Now != nullptr && Now->Plan.Route.Steps.Num() == 3
		&& Now->Plan.Route.Steps.Last().Edge == ED);
	TestTrue(TEXT("its windows on the new route booked, none left on the old"),
		Planning->GetTable().WindowsOn(FTaxiResource::Edge(ED)).Num() == 1
		&& Planning->GetTable().WindowsOn(FTaxiResource::Edge(BD)).Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanLayoutEditTest, "Airside.Model.TaxiPlan.LayoutEditReplans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanLayoutEditTest::RunTest(const FString& Parameters)
{
	// SPEC §4's EDIT: the layout changes under a taxiing aircraft. Every guideline handle is reallocated by the rebuild, so
	// no window survives; the aircraft is re-planned along the route the re-resolve left it on - nothing booked on a dead
	// handle. When no plan fits (refused on demand here) it taxis UNPLANNED, flagged as a layout edit's, until a retry
	// gives it a plan again.
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Piper, { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const int32 Id = Traffic->DispatchArrival(*Airport.Net, Airport.Threshold, Piper, 0.0);
	if (!TestNotNull(TEXT("a planning owner"), Planning) || !TestTrue(TEXT("an arrival"), Id > 0))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 600 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Taxiing; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	for (int32 Tick = 0; Tick < 30 * 3; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	if (!TestTrue(TEXT("it lands and taxis in on its plan"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Taxiing
		&& Planning->Find(Id) != nullptr))
	{
		return false;
	}

	// AN EDIT ELSEWHERE: a stand placed - a topology rebuild that reallocates every guideline handle.
	auto Edit = [&Airport, Traffic](const FVector2D& At)
	{
		UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
		Airport.Net->PlaceEntity(Stand, Stand->Anchors, At, 0.0);
		TestGraph::Rebuild(*Airport.Net);
		Traffic->OnGraphRebuilt(*Airport.Net);
	};
	Edit(Airport.ExitAt + FVector2D(60000.0, -30000.0));
	const FTaxiClearance* After = Planning->Find(Id);
	const FRoadAgent* Agent = Traffic->FindAgent(Id);
	TestTrue(TEXT("re-planned through the rebuild"), After != nullptr && Agent != nullptr);
	if (After != nullptr && Agent != nullptr)
	{
		TestEqual(TEXT("along the route it now drives"), After->Plan.Route.Steps.Num(), Agent->PlanInProgress().Steps.Num());
		bool bAllLive = After->Plan.Passes.Num() > 0;
		for (const FTaxiPass& Pass : After->Plan.Passes)
		{
			bAllLive &= Pass.Resource.Kind == ETaxiResourceKind::Edge ? Airport.Net->GetGuidelineEdge(Pass.Resource.EdgeId) != nullptr
				: Airport.Net->GetGuidelineNode(Pass.Resource.NodeId) != nullptr;
		}
		TestTrue(TEXT("every window on a live handle"), bAllLive);
	}

	// NO PLAN FITS: unplanned, as a layout edit's - and back on a plan once one does.
	Planning->bRefuseReplansForTest = true;
	Edit(Airport.ExitAt + FVector2D(60000.0, -60000.0));
	const FTaxiUnplanned* Lost = Planning->FindUnplanned(Id);
	TestTrue(TEXT("refused a plan, it taxis unplanned - flagged as the edit's"), Lost != nullptr
		&& Lost->Cause == ETaxiUnplanned::LayoutEdit && Planning->Find(Id) == nullptr);
	FAgentFacts Facts;
	TestTrue(TEXT("the inspector carries the fact"), InspectFacts::DescribeAgent(*Traffic, Airport.Net, Id, Facts)
		&& !Facts.TaxiUnplanned.IsEmpty());
	Planning->bRefuseReplansForTest = false;
	for (int32 Tick = 0; Tick < 30 * 3; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	TestNull(TEXT("retried when it could be, it holds a plan again"), Planning->FindUnplanned(Id));
	TestNotNull(TEXT("planned"), Planning->Find(Id));
	for (int32 Tick = 0; Tick < 30 * 600 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	TestEqual(TEXT("and parks"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanExitTest, "Airside.Model.TaxiPlan.ArrivalNeverHeldOnTheExit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanExitTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #528 FINDING 4: an arrival vacates rolling and may not wait (bMayWaitAtStart false) - its first move is
	// off the runway. Something booked AHEAD of it there that runs late (a stand-in that never releases, here) used to hold
	// it by order on the exit - on the runway - which the late one might itself need: order against runway. The first
	// move after the exit is never refused by the order; the claim pass's own spacing still keeps it off a body.
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
	const double Vacate = FLandingRun::SecondsToVacate(Landing.End, Piper, Landing.VacateAt);
	const FTaxiPlan Preview = Planning->Plan(*Airport.Net, Piper, Traffic->Rules, ArrivalPlanner::TaxiInRequest(Landing, Vacate));
	if (!TestTrue(TEXT("a taxi-in plan"), Preview.IsPlanned() && Preview.Route.Steps.Num() > 0))
	{
		return false;
	}
	// AHEAD OF IT ON ITS FIRST EDGE, ending before its window there begins - and never released. GOING ITS WAY: the
	// exemption is for one ahead of it the same way (or on a node); one coming the other way still holds it (below).
	const FTaxiResource First = FTaxiResource::Edge(Preview.Route.Steps[0].Edge);
	double FirstFrom = Vacate;
	ETaxiWay FirstWay = ETaxiWay::Any;
	for (const FTaxiPass& Pass : Preview.Passes)
	{
		FirstWay = Pass.Resource == First ? Pass.Window.Way : FirstWay;
		FirstFrom = Pass.Resource == First ? FMath::Min(FirstFrom, Pass.Window.From) : FirstFrom;
	}
	const TArray<FTaxiPass> Late = { { First, { TaxiStandIn, 0.0, FMath::Max(1.0, FirstFrom - 1.0), FirstWay } } };
	if (!TestTrue(TEXT("a late stand-in booked ahead of it there"), Planning->BookPassesForTest(Late)))
	{
		return false;
	}
	const int32 Id = Traffic->DispatchArrival(*Airport.Net, Airport.Threshold, Piper, 0.0);
	if (!TestTrue(TEXT("dispatched behind it"), Id > 0))
	{
		return false;
	}
	bool bHeldByIt = false;
	for (int32 Tick = 0; Tick < 30 * 600 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
		bHeldByIt |= Traffic->FindAgent(Id)->GetWaitingOn() == TaxiStandIn;
	}
	TestFalse(TEXT("never held on the exit for the late one"), bHeldByIt);
	TestEqual(TEXT("it leaves the runway and parks"), Traffic->FindAgent(Id)->Phase, EAgentPhase::Parked);

	// REVIEW OF #534 FINDING 7: ONE COMING THE OTHER WAY still holds it. Booked ahead of it on its first edge head-on, late
	// and waiting at the far end, it would drive onto that edge on its turn while the exempt arrival drove onto it from the
	// runway: a head-on mid-edge, permanent. Better held at the exit than that.
	const FTestAirport Second = FTestAirport::Build(Piper);
	UGroundTraffic* Opposed = NewObject<UGroundTraffic>(GetTransientPackage());
	FGroundTrafficTestAccess OpposedAccess(*Opposed);
	UTaxiPlanning* OpposedPlanning = OpposedAccess.TaxiPlanning();
	const FArrivalPlan Again = ArrivalPlanner::Plan(*Second.Net, Second.Threshold, Piper, &Opposed->GetOccupancy());
	const FTaxiPlan Shown = OpposedPlanning != nullptr && Again.IsValid()
		? OpposedPlanning->Plan(*Second.Net, Piper, Opposed->Rules, ArrivalPlanner::TaxiInRequest(Again, Vacate)) : FTaxiPlan();
	if (!TestTrue(TEXT("a second field's taxi-in plan"), Shown.IsPlanned() && Shown.Route.Steps.Num() > 0))
	{
		return false;
	}
	const FTaxiResource Exit = FTaxiResource::Edge(Shown.Route.Steps[0].Edge);
	const ETaxiWay Against = Shown.Route.Steps[0].bReversed ? ETaxiWay::AToB : ETaxiWay::BToA;
	const TArray<FTaxiPass> HeadOn = { { Exit, { TaxiStandIn, 0.0, FMath::Max(1.0, FirstFrom - 1.0), Against } } };
	if (!TestTrue(TEXT("one booked ahead of it there the other way"), OpposedPlanning->BookPassesForTest(HeadOn)))
	{
		return false;
	}
	const int32 Held = Opposed->DispatchArrival(*Second.Net, Second.Threshold, Piper, 0.0);
	if (!TestTrue(TEXT("dispatched behind it"), Held > 0))
	{
		return false;
	}
	bool bHeldByOpposer = false;
	for (int32 Tick = 0; Tick < 30 * 600 && !bHeldByOpposer; ++Tick)
	{
		Opposed->Advance(1.0 / 30.0, Second.Net);
		bHeldByOpposer |= Opposed->FindAgent(Held)->GetWaitingOn() == TaxiStandIn;
	}
	TestTrue(TEXT("held at the exit for the one coming the other way"), bHeldByOpposer);
	OpposedPlanning->Drop(TaxiStandIn, nullptr);
	for (int32 Tick = 0; Tick < 30 * 600 && Opposed->FindAgent(Held)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Opposed->Advance(1.0 / 30.0, Second.Net);
	}
	TestEqual(TEXT("and parks once it has passed"), Opposed->FindAgent(Held)->Phase, EAgentPhase::Parked);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanFollowerTest, "Airside.Model.TaxiPlan.ReplanKeepsTheFollowerBehind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanFollowerTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 1: X leads Y down AB. X stops for a while (a test delay), Y closes up behind it, and X's route
	// changes (the resolver's splice). The re-plan placed X round the table as it stood: its time at B fell after Y's, and
	// its window on AB - the edge it stands on - after Y's too. X then waited for Y at B by order, and Y could not pass X:
	// permanent. Now whoever is not physically ahead of X on the edge it is on is re-planned BEHIND it.
	//                 F (0, 30000)
	//                 |
	//   A (-30000) ---B (0,0)--- D (30000)
	//                  \        /
	//                   E (15000, -15000)
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 30000.0, 0.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 15000.0, -15000.0);
	const FGuidelineNodeId F = TestGraph::Node(*Net, 0.0, 30000.0);
	const FGuidelineEdgeId AB = TaxiLane(*Net, A, B);
	const FGuidelineEdgeId BD = TaxiLane(*Net, B, D);
	TaxiLane(*Net, B, E);
	TaxiLane(*Net, E, D);
	TaxiLane(*Net, B, F);

	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const FTaxiPlan PlanX = Planning != nullptr ? Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, D, 0.0)) : FTaxiPlan();
	const int32 X = PlanX.IsPlanned() ? Traffic->DispatchAgent(Net, PlanX.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("X planned, dispatched and booked"), X > 0
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiOut, PlanX, ETaxiClearanceStage::Moving)))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 8; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	const double Then = Traffic->GetSimSeconds();
	const FTaxiPlan PlanY = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, F, Then));
	const int32 Y = PlanY.IsPlanned() ? Traffic->DispatchAgent(Net, PlanY.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("Y planned behind X down AB, dispatched and booked"), Y > 0
		&& Planning->Book(*Net, Y, ETaxiClearanceKind::TaxiOut, PlanY, ETaxiClearanceStage::Moving, FRoutePlan(), Then)
		&& Planning->GetTable().WindowsOn(FTaxiResource::Edge(AB)).Num() == 2
		&& Planning->GetTable().WindowsOn(FTaxiResource::Edge(AB))[0].Holder == X))
	{
		return false;
	}
	Planning->DelayForTest(X, Then + 12.0);
	for (int32 Tick = 0; Tick < 30 * 8; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("the resolver's splice sends X round by E"), Access.ReplanAt(X, *Net, 1, BD)))
	{
		return false;
	}
	Traffic->Advance(1.0 / 30.0, Net);
	TestTrue(TEXT("X re-planned along its new route"), Planning->Find(X) != nullptr && Planning->FindUnplanned(X) == nullptr);
	auto IndexOn = [Planning](const FTaxiResource& R, int32 Holder)
	{
		return Planning->GetTable().WindowsOn(R).IndexOfByPredicate([Holder](const FTaxiWindow& W) { return W.Holder == Holder; });
	};
	TestEqual(TEXT("on AB, the edge both stand on, X - ahead - is first"), IndexOn(FTaxiResource::Edge(AB), X), 0);
	const int32 YAtB = IndexOn(FTaxiResource::Node(B), Y);
	TestTrue(TEXT("and at B, X is before Y"), IndexOn(FTaxiResource::Node(B), X) != INDEX_NONE
		&& (YAtB == INDEX_NONE || IndexOn(FTaxiResource::Node(B), X) < YAtB));

	const FVector2D DAt = Net->GetGuidelineNode(D)->Position;
	const FVector2D FAt = Net->GetGuidelineNode(F)->Position;
	bool bXWaitedOnY = false;
	bool bBothHome = false;
	for (int32 Tick = 0; Tick < 30 * 600 && !bBothHome; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		if (const FRoadAgent* XAgent = Traffic->FindAgent(X))
		{
			bXWaitedOnY |= XAgent->GetWaitingOn() == Y;
		}
		bBothHome = DistanceTo(*Traffic, X, DAt) < 500.0 && DistanceTo(*Traffic, Y, FAt) < 500.0;
	}
	TestFalse(TEXT("X never waited on Y, stuck behind it"), bXWaitedOnY);
	TestTrue(TEXT("both reached their goals - no jam"), bBothHome);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanMidRouteArrivalTest, "Airside.Model.TaxiPlan.ReplannedArrivalIsOrderedMidRoute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanMidRouteArrivalTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 3: an arrival's FIRST MOVE - off the runway - is never refused by the order (#528 finding 4).
	// "First" was read as MoveStarts[0], and a re-plan along the live route makes that the step the aircraft is ON,
	// anywhere along its taxi: its next node was then entered with no regard for who was booked there first. The
	// exemption is now a fact of the clearance, set only where a plan starts at the runway exit.
	//   A ---- B ---- D
	//           \    /
	//             E
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId D = TestGraph::Node(*Net, 30000.0, 0.0);
	const FGuidelineNodeId E = TestGraph::Node(*Net, 15000.0, -15000.0);
	TaxiLane(*Net, A, B);
	const FGuidelineEdgeId BD = TaxiLane(*Net, B, D);
	TaxiLane(*Net, B, E);
	TaxiLane(*Net, E, D);

	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const FTaxiPlan Plan = Planning != nullptr ? Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, D, 0.0)) : FTaxiPlan();
	const int32 X = Plan.IsPlanned() ? Traffic->DispatchAgent(Net, Plan.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("an arrival's taxi in, booked"), X > 0
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiIn, Plan, ETaxiClearanceStage::Moving)))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 5; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestTrue(TEXT("its route changes mid-way (the resolver's splice)"), Access.ReplanAt(X, *Net, 1, BD)))
	{
		return false;
	}
	Traffic->Advance(1.0 / 30.0, Net);
	const FTaxiClearance* Replanned = Planning->Find(X);
	double XAtB = -1.0;
	for (const FTaxiWindow& W : Planning->GetTable().WindowsOn(FTaxiResource::Node(B)))
	{
		XAtB = W.Holder == X ? W.From : XAtB;
	}
	const double Now = Traffic->GetSimSeconds();
	if (!TestTrue(FString::Printf(TEXT("re-planned, due at B at %.0f s (now %.0f s)"), XAtB, Now), Replanned != nullptr && XAtB > Now + 1.0))
	{
		return false;
	}
	// AHEAD OF IT AT B, and never released: the order must hold it short of B.
	const TArray<FTaxiPass> Ahead = { { FTaxiResource::Node(B), { TaxiStandIn, Now, XAtB - 0.5 } } };
	if (!TestTrue(TEXT("a stand-in booked ahead of it at B"), Planning->BookPassesForTest(Ahead)))
	{
		return false;
	}
	bool bWaited = false;
	for (int32 Tick = 0; Tick < 30 * 120 && !bWaited; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
		bWaited |= Traffic->FindAgent(X)->GetWaitingOn() == TaxiStandIn;
	}
	TestTrue(TEXT("mid-route, it waits its turn at B like anyone"), bWaited);
	Planning->Drop(TaxiStandIn, nullptr);
	const FVector2D DAt = Net->GetGuidelineNode(D)->Position;
	for (int32 Tick = 0; Tick < 30 * 300 && DistanceTo(*Traffic, X, DAt) >= 500.0; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	TestTrue(TEXT("and goes on once it is its turn"), DistanceTo(*Traffic, X, DAt) < 500.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanBookedStandTest, "Airside.Model.TaxiPlan.RebuildReholdsABookedStandFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanBookedStandTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 4: P, a departure on stand S, is booked to push later; X taxis in to S, booked to arrive after
	// P has gone. A layout edit re-made the MOVING plans first: X re-booked S - for ever, from when it can get there - and
	// P's stand could not then be held, so P's next plan could never fit before X, and X could never reach a stand P's body
	// stood on. The parked are re-held first now: they are where they are.
	//   C (-30000, 0) ---- J (0, 0)
	//                      |
	//                      S (0, -20000)
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId C = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId J = TestGraph::Node(*Net, 0.0, 0.0);
	const FGuidelineNodeId S = TestGraph::Node(*Net, 0.0, -20000.0);
	TaxiLane(*Net, C, J);
	TaxiLane(*Net, J, S);

	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const int32 P = Traffic->DispatchAgent(Net, TestGraph::Probe(*Net, J, S, ETraversalClass::Aircraft), Piper,
		ETraversalClass::Aircraft, 0.0);
	for (int32 I = 0; I < 30 * 300 && P > 0 && Traffic->FindAgent(P)->Phase != EAgentPhase::Parked; ++I)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	if (!TestNotNull(TEXT("a planning owner"), Planning)
		|| !TestTrue(TEXT("P parks on S"), P > 0 && Traffic->FindAgent(P)->Phase == EAgentPhase::Parked))
	{
		return false;
	}
	const double Now = Traffic->GetSimSeconds();
	FTaxiPlan Push;
	Push.Result = ETaxiPlanResult::Planned;
	Push.PushAt = Now + 1000.0;
	Push.Passes = { { FTaxiResource::Node(S), { P, Now, Now + 120.0 } } };
	TestTrue(TEXT("P booked to push later"), Planning->Book(*Net, P, ETaxiClearanceKind::TaxiOut, Push, ETaxiClearanceStage::Booked,
		FRoutePlan(), Now));
	const FTaxiPlan Inbound = Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(C, S, Now));
	const int32 X = Inbound.IsPlanned() ? Traffic->DispatchAgent(Net, Inbound.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("X planned to S after P, dispatched and booked"), X > 0
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiIn, Inbound, ETaxiClearanceStage::Moving, FRoutePlan(), Now)
		&& Planning->GetTable().WindowsOn(FTaxiResource::Node(S)).Num() == 2
		&& Planning->GetTable().WindowsOn(FTaxiResource::Node(S))[0].Holder == P))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 2; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}

	Traffic->OnGraphRebuilt(*Net);
	const TConstArrayView<FTaxiWindow> OnS = Planning->GetTable().WindowsOn(FTaxiResource::Node(S));
	TestTrue(TEXT("P, on the stand, holds it first after the rebuild"), OnS.Num() > 0 && OnS[0].Holder == P);
	bool bXAhead = false;
	for (const FTaxiWindow& W : OnS)
	{
		if (W.Holder == P)
		{
			break;
		}
		bXAhead |= W.Holder == X;
	}
	TestFalse(TEXT("X is never booked onto S ahead of P"), bXAhead);
	TestTrue(TEXT("X is still accounted for - planned after P, or unplanned and retried"), Planning->Find(X) != nullptr
		|| Planning->FindUnplanned(X) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanReplannedEntryTest, "Airside.Model.TaxiPlan.ReplannedDepartureFreesItsEntry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanReplannedEntryTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 5: a departure's plan holds its runway entry TaxiPlanEntryHold after it gets there, not for ever
	// (the 80/h starvation). A re-plan - a layout edit, a resolver splice - booked the entry for ever again, and every taxi-in
	// through that entry waited for the departure to take off.
	const FAirframe Piper = TestAirframes::Piper();
	const FTestAirport Airport = FTestAirport::Build(Piper, { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const int32 Id = Traffic->DispatchArrival(*Airport.Net, Airport.Threshold, Piper, 0.0);
	if (!TestNotNull(TEXT("a planning owner"), Planning) || !TestTrue(TEXT("an arrival"), Id > 0))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 900 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Parked; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	if (!TestTrue(TEXT("it parks"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Parked)
		|| !TestEqual(TEXT("ordered off, it goes at once"), Traffic->DepartAgent(Id, *Airport.Net), EDepartureRefusal::None))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 300 && Traffic->FindAgent(Id)->Phase != EAgentPhase::Taxiing; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Airport.Net);
	}
	auto EntryHeldUntil = [Planning, Id]()
	{
		const FTaxiClearance* Cleared = Planning->Find(Id);
		double Until = -1.0;
		if (Cleared != nullptr && Cleared->Plan.Route.Steps.Num() > 0)
		{
			for (const FTaxiWindow& W : Planning->GetTable().WindowsOn(FTaxiResource::Node(Cleared->Plan.Route.Steps.Last().To)))
			{
				Until = W.Holder == Id ? FMath::Max(Until, W.To) : Until;
			}
		}
		return Until;
	};
	if (!TestTrue(TEXT("taxiing out to its entry on a plan that holds it a while"), Traffic->FindAgent(Id)->Phase == EAgentPhase::Taxiing
		&& EntryHeldUntil() > 0.0 && EntryHeldUntil() < FTaxiReservations::Forever))
	{
		return false;
	}

	// AN EDIT ELSEWHERE: every guideline handle reallocated, every plan re-made.
	UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
	Airport.Net->PlaceEntity(Stand, Stand->Anchors, Airport.ExitAt + FVector2D(60000.0, -30000.0), 0.0);
	TestGraph::Rebuild(*Airport.Net);
	Traffic->OnGraphRebuilt(*Airport.Net);
	const double Until = EntryHeldUntil();
	TestTrue(FString::Printf(TEXT("re-planned, it still holds its entry a while, not for ever (until %.0f s)"), Until),
		Until > 0.0 && Until < FTaxiReservations::Forever);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanVacateTimeTest, "Airside.Model.TaxiPlan.RetimeKeepsAnArrivalsVacateTime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanVacateTimeTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 6: a re-time cascade moves an arrival's plan later with everyone behind the late one - its
	// windows and its PushAt (when it leaves the exit). Its LANDING does not move: a re-plan while it is still on final
	// planned the exit from the shifted PushAt, a time it would not be there. The flown vacate time is kept apart.
	// (Making the arrival's windows immovable instead - the late one swapped behind it, or the re-time refused - was
	// measured on the headline test and jammed at 80/h either way, 2026-10-03.)
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FTaxiResource R = FTaxiResource::Node(Net->AddGuidelineNode(FVector2D(0.0, 0.0)));
	UTaxiPlanning* Planning = NewObject<UTaxiPlanning>(GetTransientPackage());
	Planning->SetHeadway(5.0);
	FTaxiPlan Late;
	Late.Result = ETaxiPlanResult::Planned;
	Late.Passes = { { R, { 1, 0.0, 20.0 } } };
	FTaxiPlan Landing = Late;
	Landing.Passes = { { R, { 2, 22.0, 30.0 } } };
	Landing.PushAt = 22.0;
	TArray<int32> Revoked;
	if (!TestTrue(TEXT("a late aircraft and an arrival behind it, booked"),
		Planning->Book(*Net, 1, ETaxiClearanceKind::TaxiOut, Late, ETaxiClearanceStage::Moving)
		&& Planning->BookArrival(*Net, 2, Landing, {}, 0.0, Revoked)))
	{
		return false;
	}
	TestTrue(TEXT("the late one re-timed, the arrival moved behind it"), Planning->Retime(1, 0.0, 15.0, 10.0));
	const FTaxiClearance* Arrival = Planning->Find(2);
	TestTrue(TEXT("its plan moved later"), Arrival != nullptr && Arrival->Plan.PushAt > 22.0);
	TestTrue(TEXT("its vacate time did not"), Arrival != nullptr && Arrival->VacateAt == 22.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanRebuildOrderTest, "Airside.Model.TaxiPlan.RebuildOrderIsTopological",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanRebuildOrderTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 8: a rebuild re-plans in the table's ORDER - each holder after everyone booked ahead of it on
	// any resource - which is not the order of earliest windows: 3 starts first (on R5) yet is behind 4 on R4. And the
	// pairs CAN form a cycle across resources (1 ahead of 2 on R1, 2 ahead of 1 on R2 - legal, one timeline), which the
	// order breaks by earliest window rather than looping or dropping anyone.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	auto Node = [Net](double X) { return FTaxiResource::Node(Net->AddGuidelineNode(FVector2D(X, 0.0))); };
	const FTaxiResource R1 = Node(0.0);
	const FTaxiResource R2 = Node(10000.0);
	const FTaxiResource R4 = Node(20000.0);
	const FTaxiResource R5 = Node(30000.0);
	UTaxiPlanning* Planning = NewObject<UTaxiPlanning>(GetTransientPackage());
	auto BookOf = [Planning, Net](int32 Holder, TArray<FTaxiPass> Passes)
	{
		FTaxiPlan Plan;
		Plan.Result = ETaxiPlanResult::Planned;
		Plan.Passes = MoveTemp(Passes);
		return Planning->Book(*Net, Holder, ETaxiClearanceKind::TaxiOut, Plan, ETaxiClearanceStage::Moving);
	};
	TestTrue(TEXT("booked"), BookOf(4, { { R4, { 4, 10.0, 20.0 } } })
		&& BookOf(3, { { R4, { 3, 20.0, 30.0 } }, { R5, { 3, 0.0, 5.0 } } })
		&& BookOf(1, { { R1, { 1, 0.0, 10.0 } }, { R2, { 1, 20.0, 30.0 } } })
		&& BookOf(2, { { R1, { 2, 10.0, 20.0 } }, { R2, { 2, 5.0, 15.0 } } }));
	TArray<int32> Order;
	TMap<int32, FTaxiClearance> Cleared;
	Planning->TakeAllForRebuild(Order, Cleared);
	TestEqual(TEXT("every holder once"), Order.Num(), 4);
	TestTrue(TEXT("4 before 3 - ahead of it on R4, though 3's first window is earlier"),
		Order.IndexOfByKey(4) != INDEX_NONE && Order.IndexOfByKey(4) < Order.IndexOfByKey(3));
	TestTrue(TEXT("the cycle between 1 and 2 broken, each placed once"), Order.Contains(1) && Order.Contains(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanUnplannedClockTest, "Airside.Model.TaxiPlan.UnplannedRetriedOnTheClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanUnplannedClockTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 10: an unplanned aircraft was retried only when the table MOVED. A window that simply runs out
	// moves nothing - and the plan it blocked fits from then on. It is retried on the clock too, however still the table.
	UTaxiPlanning* Planning = NewObject<UTaxiPlanning>(GetTransientPackage());
	Planning->MarkUnplanned(7, ETaxiUnplanned::LayoutEdit, TEXT("a test"), ETaxiClearanceKind::TaxiIn, ERouteErrand::ArrivalTaxiIn);
	Planning->NoteUnplannedTried(7, 0.0);
	TestEqual(TEXT("not again at once, the table unmoved"), Planning->UnplannedDueRetry(1.5).Num(), 0);
	TestTrue(TEXT("but again on the clock"), Planning->UnplannedDueRetry(10.5).Contains(7));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTaxiPlanUnplannedFrameTest, "Airside.Model.TaxiPlan.UnplannedChangeIsAnnouncedInItsFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTaxiPlanUnplannedFrameTest::RunTest(const FString& Parameters)
{
	// REVIEW OF #534 FINDING 11: OnTaxiUnplannedChanged is DiffFreedom's - driven here through Advance, not broadcast by hand.
	// And the retry ran AFTER the diff, so an aircraft that regained its plan was announced a frame late, in a second
	// broadcast: the frame that saw it change says so.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FGuidelineNodeId A = TestGraph::Node(*Net, -30000.0, 0.0);
	const FGuidelineNodeId B = TestGraph::Node(*Net, 30000.0, 0.0);
	TaxiLane(*Net, A, B);
	const FAirframe Piper = TestAirframes::Piper();
	FGroundTrafficTestAccess Access(*Traffic);
	UTaxiPlanning* Planning = Access.TaxiPlanning();
	const FTaxiPlan Plan = Planning != nullptr ? Planning->Plan(*Net, Piper, Traffic->Rules, TaxiRequest(A, B, 0.0)) : FTaxiPlan();
	const int32 X = Plan.IsPlanned() ? Traffic->DispatchAgent(Net, Plan.Route, Piper, ETraversalClass::Aircraft, 0.0) : 0;
	if (!TestTrue(TEXT("planned, dispatched, booked"), X > 0
		&& Planning->Book(*Net, X, ETaxiClearanceKind::TaxiIn, Plan, ETaxiClearanceStage::Moving)))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 30 * 2; ++Tick)
	{
		Traffic->Advance(1.0 / 30.0, Net);
	}
	int32 Heard = 0;
	const FDelegateHandle Handle = Traffic->OnTaxiUnplannedChanged.AddLambda([&Heard]() { ++Heard; });
	Planning->MarkUnplanned(X, ETaxiUnplanned::RouteChanged, TEXT("a test"), ETaxiClearanceKind::TaxiIn, ERouteErrand::ArrivalTaxiIn);
	Traffic->Advance(1.0 / 30.0, Net);
	TestNull(TEXT("retried in that frame, it holds a plan again"), Planning->FindUnplanned(X));
	TestEqual(TEXT("one broadcast, in that frame"), Heard, 1);
	Traffic->Advance(1.0 / 30.0, Net);
	TestEqual(TEXT("and none a frame late"), Heard, 1);
	Traffic->OnTaxiUnplannedChanged.Remove(Handle);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
