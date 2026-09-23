#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Entities/AircraftType.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/TrafficOccupancy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A star exactly like ChooseStandMultiGoalTest.cpp's FStarFixture (own copy, own name -
	 * the tests module is a UNITY build, and two anonymous-namespace helpers of the SAME name
	 * in different files collide once concatenated; see AirsideTestFixtures.h's top comment).
	 * From at the origin, one guideline edge straight to each stand or depot's own pose node,
	 * so an edge's cached Length IS the Euclidean distance AddStand/AddDepot was asked for.
	 */
	struct FAdmissionStar
	{
		URoadNetwork* Net = nullptr;
		FGuidelineNodeId From;

		/** A stand DistanceUu from From along Direction, carrying DesignWingspan - 0.0
		 *  (default) is "unknown", PlaceEntity's own default before this task and still what
		 *  every OTHER stand fixture in this module passes. */
		FGuidelineNodeId AddStand(double DistanceUu, const FVector2D& Direction, double DesignWingspan = 0.0)
		{
			UEntityDefinition* Definition = UEntityDefinition::MakeStandTransient();
			const FVector2D At = Net->GetGuidelineNode(From)->Position + Direction.GetSafeNormal() * DistanceUu;
			const FEntityInstanceId Id = Net->PlaceEntity(Definition, Definition->Anchors, At, 0.0, DesignWingspan);
			const FGuidelineNodeId Pose = Net->GetEntity(Id)->PoseNode;
			TestGraph::Join(*Net, From, Pose);
			return Pose;
		}

		/** A fuel depot DistanceUu from From along Direction - its pose node is joined exactly
		 *  like a stand's, but FEntityInstance::IsDepot (PoseRole::Fuel, captured from the
		 *  definition here, since Model/ cannot read it live) must keep it out of ChooseStand's
		 *  Candidates - Airside.Model.StandChoice.DepotNeverCandidate's whole point. */
		FGuidelineNodeId AddDepot(double DistanceUu, const FVector2D& Direction)
		{
			UEntityDefinition* Definition = UEntityDefinition::MakeFuelDepotTransient();
			const FVector2D At = Net->GetGuidelineNode(From)->Position + Direction.GetSafeNormal() * DistanceUu;
			const FEntityInstanceId Id = Net->PlaceEntity(Definition, Definition->Anchors, At, 0.0,
				/*DesignWingspan=*/0.0, Definition->PoseRole, Definition->Trucks);
			const FGuidelineNodeId Pose = Net->GetEntity(Id)->PoseNode;
			TestGraph::Join(*Net, From, Pose);
			return Pose;
		}
	};

	FAdmissionStar BuildAdmissionStar()
	{
		FAdmissionStar Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		Out.From = TestGraph::Node(*Out.Net, 0.0, 0.0);
		return Out;
	}

	/** The King Air 350's published wingspan - 57 ft 11 in / 17.65 m - literal because, unlike
	 *  the 737-800 below, no builder in this codebase authors one; Code B (under 24 m). */
	constexpr double KingAirWingspanUu = 1765.0;

	/** The 737-800's own wingspan, off the SAME builder IcaoCodeTest and StandLayoutTest read
	 *  it from (UAircraftType::Build737) rather than typed a third time here - Code C. */
	double Boeing737WingspanUu()
	{
		UAircraftType* B738 = NewObject<UAircraftType>(GetTransientPackage());
		UAircraftType::Build737(B738);
		return B738->Airframe().Wingspan;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTest,
	"Airside.Model.ArrivalPlanner.SkipsHeldStand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTest::RunTest(const FString& Parameters)
{
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	const FGuidelineNodeId PoseA = A.Pose(A.Stands[0]);
	const FGuidelineNodeId PoseB = A.Pose(A.Stands[1]);
	if (!TestTrue(TEXT("both stands linked"), PoseA.IsSet() && PoseB.IsSet())) { return false; }
	const FAirframe Piper = TestAirframes::Piper();

	// Planner level: with the first choice held by agent 7, the plan goes to the other; with
	// both held, NoFreeStand.
	FTrafficOccupancy Occ;
	const FArrivalPlan Free = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with both free"), Free.IsValid())) { return false; }
	const FGuidelineNodeId First = Free.TaxiIn.Steps.Last().To;

	auto Hold = [&](FGuidelineNodeId Node, int32 Agent)
	{
		FTrafficClaim C; C.AgentId = Agent; C.Resource = FTrafficResource::OfNode(Node); C.Rank = 10;
		FTrafficClaim B; Occ.TryClaim(C, B);
	};
	Hold(First, 7);
	const FArrivalPlan Other = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	if (!TestTrue(TEXT("plans with one held"), Other.IsValid())) { return false; }
	TestTrue(TEXT("and goes to the OTHER stand"), Other.TaxiIn.Steps.Last().To != First);
	Hold(Other.TaxiIn.Steps.Last().To, 8);
	const FArrivalPlan None = ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ);
	TestEqual(TEXT("both held is NoFreeStand"), None.Why, EArrivalRefusal::NoFreeStand);
	TestTrue(TEXT("and it has words"), !ArrivalPlanner::DescribeRefusal(None).IsEmpty());

	// Unreachable is still NoRouteToStand: remove both stands.
	A.Net->RemoveEntity(A.Stands[0]);
	A.Net->RemoveEntity(A.Stands[1]);
	TestGraph::Rebuild(*A.Net);
	TestEqual(TEXT("no stands at all is NoRouteToStand, not NoFreeStand"),
		ArrivalPlanner::Plan(*A.Net, A.Threshold, Piper, &Occ).Why, EArrivalRefusal::NoRouteToStand);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTwoArrivalsTest,
	"Airside.Model.Traffic.TwoArrivalsTwoStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTwoArrivalsTest::RunTest(const FString& Parameters)
{
	// THE REPORT: "I called in 2 aircraft, they both went to the same stand." Through the
	// model, end to end: the second is dispatched once the first has vacated the runway (the
	// runway claim would refuse it earlier, for its own reason), and gets the other stand.
	// A third, once the second has vacated too, is refused for want of a stand.
	const FTestAirport A = FTestAirport::Build(TestAirframes::Piper(), { .StandCount = 2 });
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FAirframe Piper = TestAirframes::Piper();

	const int32 First = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(TEXT("first dispatched"), First > 0)) { return false; }
	// UNTIL THE RUNWAY IS CLEAR, not merely until Taxiing: an aircraft that has just vacated
	// still holds the strip under the crossing rule until its tail is off it, and a landing
	// asked in that window is refused as RunwayOccupied - its own reason, not this test's.
	auto RunwayFree = [&]()
	{
		for (const FTrafficClaim& C : Traffic->GetOccupancy().GetClaims())
		{
			if (C.Resource.Kind == ETrafficResourceKind::Surface) { return false; }
		}
		return true;
	};
	if (!TestTrue(TEXT("first vacates and clears the runway"), RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(First); return P && P->Phase == EAgentPhase::Taxiing && RunwayFree(); }))) { return false; }

	TArray<EArrivalRefusal> Refusals;
	Traffic->OnArrivalRefused.AddLambda([&](EArrivalRefusal Why) { Refusals.Add(Why); });
	const int32 Second = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	if (!TestTrue(FString::Printf(TEXT("second dispatched (refusals: %d)"), Refusals.Num()), Second > 0)) { return false; }
	TestTrue(TEXT("two aircraft, two stands"),
		Traffic->FindAgent(First)->GoalNode != Traffic->FindAgent(Second)->GoalNode);

	if (!TestTrue(TEXT("second vacates and clears the runway"), RunUntil(*Traffic, *A.Net, 300.0,
		[&]() { const FRoadAgent* P = Traffic->FindAgent(Second); return P && P->Phase == EAgentPhase::Taxiing && RunwayFree(); }))) { return false; }
	const int32 Third = Traffic->DispatchArrival(*A.Net, A.Threshold, Piper, 1.0);
	TestEqual(TEXT("a third is refused"), Third, 0);
	TestTrue(TEXT("for want of a free stand"), Refusals.Num() > 0 && Refusals.Last() == EArrivalRefusal::NoFreeStand);
	return true;
}

// TASK 4: admission - the smallest ICAO-letter stand that fits, then the nearest of those -
// replacing "nearest regardless of size". Big stands are kept for big aircraft (GDD "smallest
// free stand that fits"): a King Air (Code B) must not be sent past a farther Code B stand to
// a nearer Code E one that a widebody may need later.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceSmallestLetterBeatsNearerTest,
	"Airside.Model.StandChoice.SmallestLetterBeatsNearer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceSmallestLetterBeatsNearerTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	const FGuidelineNodeId StandE = Star.AddStand(1000.0, FVector2D(0.0, 1.0), 6000.0); // Code E, nearer
	const FGuidelineNodeId StandB = Star.AddStand(5000.0, FVector2D(1.0, 0.0), KingAirWingspanUu); // Code B, farther

	FAirframe KingAir;
	KingAir.Wingspan = KingAirWingspanUu;
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, KingAir, nullptr, 0);

	TestEqual(TEXT("big stands are kept for big aircraft (GDD 'smallest free stand that fits')"), Chosen, StandB);
	TestTrue(TEXT("not the nearer stand sized for a wider aircraft"), Chosen != StandE);
	return true;
}

// The same shape, with the Code B stand HELD - falls through to the Code E stand rather than
// refusing outright, since a bigger-than-needed stand is still admitted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceFallsThroughToBiggerTest,
	"Airside.Model.StandChoice.FallsThroughToBigger",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceFallsThroughToBiggerTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	const FGuidelineNodeId StandB = Star.AddStand(1000.0, FVector2D(1.0, 0.0), KingAirWingspanUu);
	const FGuidelineNodeId StandE = Star.AddStand(2000.0, FVector2D(0.0, 1.0), 6000.0);

	FTrafficOccupancy Occ;
	FTrafficClaim Claim; Claim.AgentId = 7; Claim.Resource = FTrafficResource::OfNode(StandB); Claim.Rank = 10;
	FTrafficClaim Bumped; Occ.TryClaim(Claim, Bumped);

	FAirframe KingAir;
	KingAir.Wingspan = KingAirWingspanUu;
	bool bSawHeld = false;
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, KingAir, &Occ, 0, nullptr, &bSawHeld);

	TestEqual(TEXT("the Code B stand is held, so the King Air takes the bigger free one"), Chosen, StandE);
	TestTrue(TEXT("and the held stand it skipped is reported"), bSawHeld);
	return true;
}

// A stand smaller than the aircraft is never chosen, held or not - it is not a candidate at
// all, the same way an unreachable one never was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceTooSmallNeverChosenTest,
	"Airside.Model.StandChoice.TooSmallNeverChosen",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceTooSmallNeverChosenTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	Star.AddStand(500.0, FVector2D(1.0, 0.0), KingAirWingspanUu); // Code B - the only stand

	FAirframe B738;
	B738.Wingspan = Boeing737WingspanUu(); // Code C
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, B738, nullptr, 0);

	TestFalse(TEXT("a Code B stand never fits a Code C aircraft"), Chosen.IsSet());
	return true;
}

// Admission compares LETTERS, not the raw captured span: a legacy stand carries the A320's
// span (3410, an older figure - not the 3580 BuildA320 authors today), and a 737-800 (3580)
// still fits it because both numbers fall in Code C's band.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceLegacySpanStillAdmitsCTest,
	"Airside.Model.StandChoice.LegacySpanStillAdmitsC",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceLegacySpanStillAdmitsCTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	const FGuidelineNodeId Stand = Star.AddStand(500.0, FVector2D(1.0, 0.0), 3410.0); // Code C, legacy figure

	FAirframe B738;
	B738.Wingspan = Boeing737WingspanUu(); // Code C, 3580
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, B738, nullptr, 0);

	TestEqual(TEXT("admission compares letters; legacy stands carry the A320's span, not the letter's"), Chosen, Stand);
	return true;
}

// A stand nobody measured (DesignWingspan == 0) is admitted regardless of the aircraft - the
// legacy behaviour every fixture in this module relied on before this task, unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceUnknownSpanAdmittedTest,
	"Airside.Model.StandChoice.UnknownSpanAdmitted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceUnknownSpanAdmittedTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	const FGuidelineNodeId Stand = Star.AddStand(500.0, FVector2D(1.0, 0.0)); // DesignWingspan defaults to 0.0

	FAirframe B738;
	B738.Wingspan = Boeing737WingspanUu();
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, B738, nullptr, 0);

	TestEqual(TEXT("an unmeasured stand admits anything, as it always did"), Chosen, Stand);
	return true;
}

// Nothing on the field is wide enough. IcaoCode::LetterForWingspan clamps to F for anything
// past E's band (its own doc comment: "anything wider F") rather than refusing outright, so
// this is not a dedicated early-return - it is the ordinary "too small" skip applied when the
// aircraft's own letter is F and nothing on the field is F or wider. Proven with a KNOWN,
// narrower stand (not an unmeasured one, which the rule above admits regardless) so the skip
// is the thing under test, not the unknown-span carve-out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceWiderThanFNowhereTest,
	"Airside.Model.StandChoice.WiderThanFNowhere",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceWiderThanFNowhereTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	Star.AddStand(500.0, FVector2D(1.0, 0.0), 6000.0); // Code E - known, but narrower than F

	FAirframe TooWide;
	TooWide.Wingspan = 9000.0; // wider than any real airframe this codebase ships
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, TooWide, nullptr, 0);

	TestFalse(TEXT("nowhere on the field is wide enough, and nothing crashes finding that out"), Chosen.IsSet());
	return true;
}

// A depot's pose node is never a candidate, no matter how much nearer it sits than the one
// real stand - FEntityInstance::IsStand excludes it before the search ever runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStandChoiceDepotNeverCandidateTest,
	"Airside.Model.StandChoice.DepotNeverCandidate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FStandChoiceDepotNeverCandidateTest::RunTest(const FString& Parameters)
{
	FAdmissionStar Star = BuildAdmissionStar();
	const FGuidelineNodeId Depot = Star.AddDepot(500.0, FVector2D(1.0, 0.0));   // nearer
	const FGuidelineNodeId Stand = Star.AddStand(2000.0, FVector2D(0.0, 1.0)); // farther, the only real candidate

	const FAirframe Piper = TestAirframes::Piper();
	const FGuidelineNodeId Chosen = ArrivalPlanner::ChooseStand(*Star.Net, Star.From, Piper, nullptr, 0);

	TestEqual(TEXT("the farther stand is chosen"), Chosen, Stand);
	TestTrue(TEXT("never the nearer depot"), Chosen != Depot);
	return true;
}

#endif
