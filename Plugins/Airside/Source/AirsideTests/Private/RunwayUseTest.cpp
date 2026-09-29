#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Content/AirsideSettings.h"
#include "Build/AnchorLink.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/RoadNetwork.h"
#include "Model/TrafficOccupancy.h"
#include "Profiles/RoadProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TWO RUNWAYS, BOTH USED (samples/2runways.png, 2026-09-29): every arrival went to the runway
 * nearest the longest strip's threshold and every departure to the shortest taxi, so the second
 * runway sat empty. Now each planner asks every runway its ERunwayUse allows and prefers a free
 * one, then one the player dedicated, then the shorter taxi.
 */
namespace RunwayUseTest
{
	/**
	 * Two parallel strips drawn along +X - A at y 0, B at y -40000 - each split at one exit, and
	 * ONE straight taxiway joining the two exits. The stands sit beside it nearer A (y -10000 and
	 * -16000), so with nothing held A is always the shorter taxi: whatever makes B win in a test
	 * is the rule under test, never geometry. FTestAirport's own sizing (exit at 1.2 N, far end at
	 * 3 N, N the landing distance) and its stand placement, doubled.
	 */
	struct FTwoRunways
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId A;
		FRoadSegmentId B;
		TArray<FEntityInstanceId> Stands;

		FGuidelineNodeId Pose(int32 Index) const
		{
			const FEntityInstance* E = Net->GetEntity(Stands[Index]);
			return E != nullptr ? E->PoseNode : FGuidelineNodeId();
		}
		static bool IsA(const FRunwayEnd& End) { return FMath::Abs(End.Threshold.Y) < 1.0; }
		static bool IsB(const FRunwayEnd& End) { return FMath::Abs(End.Threshold.Y + 40000.0) < 1.0; }

		void SetUse(FRoadSegmentId Seed, ERunwayUse Use) const
		{
			FRunwayFacts Facts = Net->RunwayFactsFor(Seed);
			Facts.Use = Use;
			Net->SetRunwayFacts(Seed, Facts);
		}
		void Hold(FTrafficOccupancy& Occupancy, FRoadSegmentId Seed, int32 AgentId) const
		{
			for (const FTrafficResource& Surface : Net->RunwaySurfaces(Seed))
			{
				Occupancy.Assert(FTrafficClaim::Make(AgentId, Surface, /*bOccupied*/ true, 2));
			}
		}

		static FTwoRunways Build(const FAirframe& Airframe)
		{
			FTwoRunways Out;
			Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
			const double Needed = FLandingRun::RequiredLandingDistance(
				Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach) * FLandingRun::LandingMargin;
			const double ExitX = Needed * 1.2;
			const double FarX = Needed * 3.0;
			URoadProfile* Runway = TestProfiles::Runway();
			URoadProfile* Taxiway = TestProfiles::Taxiway();

			auto Strip = [&](double Y, FRoadNodeId& OutExit)
			{
				const FRoadNodeId Threshold = Out.Net->AddNode(FVector2D(0.0, Y));
				OutExit = Out.Net->AddNode(FVector2D(ExitX, Y));
				const FRoadNodeId Far = Out.Net->AddNode(FVector2D(FarX, Y));
				const FRoadSegmentId Seed = TestGraph::Lay(*Out.Net, Threshold, OutExit, Runway);
				TestGraph::Lay(*Out.Net, OutExit, Far, Runway);
				return Seed;
			};
			FRoadNodeId ExitA, ExitB;
			Out.A = Strip(0.0, ExitA);
			Out.B = Strip(-40000.0, ExitB);
			const FRoadNodeId Middle = Out.Net->AddNode(FVector2D(ExitX, -20000.0));
			TestGraph::Lay(*Out.Net, ExitA, Middle, Taxiway);
			TestGraph::Lay(*Out.Net, Middle, ExitB, Taxiway);
			TestGraph::Derive(*Out.Net);

			// FACING EAST, so the lead-in casts west onto the taxiway - FTestAirport::Build's reason.
			for (const double Y : { -10000.0, -16000.0 })
			{
				UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
				Out.Stands.Add(Out.Net->PlaceEntity(Stand, Stand->Anchors, FVector2D(ExitX + 9000.0, Y), 0.0));
			}
			FAnchorLink::Build(*Out.Net, UAirsideSettings::ResolveLargestServiceVehicle());
			return Out;
		}
	};
}

using RunwayUseTest::FTwoRunways;

/**
 * AN ARRIVAL TAKES THE FREE RUNWAY. Nothing held: A, the shorter taxi. A held: B - it used to be
 * refused, waiting on A with B empty. Both held: refused as busy, and IsRunwayBusy - what the
 * queue asks - agrees at every step, or a queued flight would wait with a runway free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseArrivalFreeTest, "Airside.Model.RunwayUse.ArrivalsTakeTheFreeRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseArrivalFreeTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	const FVector2D Focus(-1000.0, 0.0);   // A's threshold - the old rule's only runway
	FTrafficOccupancy Occupancy;

	const FArrivalPlan Free = ArrivalPlanner::Plan(*F.Net, Focus, Airframe, &Occupancy);
	if (!TestTrue(FString::Printf(TEXT("fixture: an arrival plans (%s)"), *ArrivalPlanner::DescribeRefusal(Free)), Free.IsValid())) { return false; }
	TestTrue(TEXT("nothing held: A, the shorter taxi"), FTwoRunways::IsA(Free.End));
	TestFalse(TEXT("and the queue sees a free runway"), ArrivalPlanner::IsRunwayBusy(*F.Net, Focus, &Occupancy));

	F.Hold(Occupancy, F.A, 90);
	const FArrivalPlan OnB = ArrivalPlanner::Plan(*F.Net, Focus, Airframe, &Occupancy);
	TestTrue(FString::Printf(TEXT("A held: it still plans (%s)"), *ArrivalPlanner::DescribeRefusal(OnB)), OnB.IsValid());
	TestTrue(TEXT("on B"), FTwoRunways::IsB(OnB.End));
	TestFalse(TEXT("and the queue does not wait for A"), ArrivalPlanner::IsRunwayBusy(*F.Net, Focus, &Occupancy));

	F.Hold(Occupancy, F.B, 91);
	const FArrivalPlan Neither = ArrivalPlanner::Plan(*F.Net, Focus, Airframe, &Occupancy);
	TestEqual(TEXT("both held: refused as busy - the refusal that clears"), Neither.Why, EArrivalRefusal::RunwayOccupied);
	TestTrue(TEXT("and the queue agrees both are busy"), ArrivalPlanner::IsRunwayBusy(*F.Net, Focus, &Occupancy));
	return true;
}

/**
 * A DEPARTURE TAKES THE FREE RUNWAY. Nothing held: A, the shorter taxi. A held: B - every
 * departure used to go to A whatever was on it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseDepartureFreeTest, "Airside.Model.RunwayUse.DeparturesTakeTheFreeRunway",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseDepartureFreeTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	FTrafficOccupancy Occupancy;

	const FDeparturePlan Free = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft, &Occupancy);
	if (!TestTrue(FString::Printf(TEXT("fixture: a departure plans (%s)"), *DeparturePlanner::Describe(Free)), Free.IsValid())) { return false; }
	TestTrue(TEXT("nothing held: A, the shorter taxi"), FTwoRunways::IsA(Free.End));

	F.Hold(Occupancy, F.A, 90);
	const FDeparturePlan OnB = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft, &Occupancy);
	TestTrue(FString::Printf(TEXT("A held: it plans (%s)"), *DeparturePlanner::Describe(OnB)), OnB.IsValid());
	TestTrue(TEXT("from B"), FTwoRunways::IsB(OnB.End));
	return true;
}

/**
 * THE PLAYER'S MODES SEGREGATE, against geometry: A is always the shorter taxi, so every B here
 * is the setting speaking. Then the modes swapped, so neither answer is a fixture accident.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseModesTest, "Airside.Model.RunwayUse.ModesSegregate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseModesTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	const FVector2D Focus(-1000.0, 0.0);

	F.SetUse(F.A, ERunwayUse::DeparturesOnly);
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	const FArrivalPlan Land = ArrivalPlanner::Plan(*F.Net, Focus, Airframe);
	TestTrue(FString::Printf(TEXT("A departures, B arrivals: an arrival plans (%s)"), *ArrivalPlanner::DescribeRefusal(Land)), Land.IsValid());
	TestTrue(TEXT("and lands on B, though A is the shorter taxi"), FTwoRunways::IsB(Land.End));
	const FDeparturePlan Leave = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
	TestTrue(TEXT("a departure leaves from A"), Leave.IsValid() && FTwoRunways::IsA(Leave.End));

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	TestTrue(TEXT("swapped: lands on A"), FTwoRunways::IsA(ArrivalPlanner::Plan(*F.Net, Focus, Airframe).End));
	const FDeparturePlan Swapped = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
	TestTrue(TEXT("and leaves from B, though A is the shorter taxi"), Swapped.IsValid() && FTwoRunways::IsB(Swapped.End));

	// A DEDICATED RUNWAY BEATS A MIXED ONE, free against free: A mixed and nearer, B set to arrivals.
	F.SetUse(F.A, ERunwayUse::Mixed);
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	TestTrue(TEXT("A mixed, B arrivals: lands on B, the runway the player dedicated"),
		FTwoRunways::IsB(ArrivalPlanner::Plan(*F.Net, Focus, Airframe).End));
	return true;
}

/**
 * A FIELD WITH NO RUNWAY FOR ONE KIND says it is the SETTING, not a missing runway - the fix is
 * the card, not the build tool.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseNoneTest, "Airside.Model.RunwayUse.OneWayFieldNamesTheSetting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseNoneTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);

	F.SetUse(F.A, ERunwayUse::DeparturesOnly);
	F.SetUse(F.B, ERunwayUse::DeparturesOnly);
	const FArrivalPlan Land = ArrivalPlanner::Plan(*F.Net, FVector2D(-1000.0, 0.0), Airframe);
	TestEqual(TEXT("every runway departures only: no arrival runway"), Land.Why, EArrivalRefusal::NoArrivalRunway);
	TestTrue(TEXT("and the sentence names the setting"), ArrivalPlanner::DescribeRefusal(Land).Contains(TEXT("departures only")));

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	F.SetUse(F.B, ERunwayUse::ArrivalsOnly);
	const FDeparturePlan Leave = DeparturePlanner::PlanAny(*F.Net, F.Pose(0), Airframe, ETraversalClass::Aircraft);
	TestEqual(TEXT("every runway arrivals only: no departure runway"), Leave.Why, EDepartureRefusal::NoDepartureRunway);
	TestTrue(TEXT("and the sentence names the setting"), DeparturePlanner::Describe(Leave).Contains(TEXT("arrivals only")));
	return true;
}

/**
 * A RECLASSIFY KEEPS THE MODE: the runway tool writes a fresh FRunwayFacts (Use Unset) to change
 * a surface, and a player's "arrivals only" must survive it - InUse's rule. And a runway never
 * set reads Mixed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseKeptTest, "Airside.Model.RunwayUse.ReclassifyKeepsTheMode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseKeptTest::RunTest(const FString& Parameters)
{
	const FTwoRunways F = FTwoRunways::Build(TestAirframes::Piper());
	TestEqual(TEXT("a runway never set reads mixed"),
		RunwayUse::Resolve(F.Net->RunwayFactsFor(F.A).Use), ERunwayUse::Mixed);

	F.SetUse(F.A, ERunwayUse::ArrivalsOnly);
	FRunwayFacts Grass;
	Grass.Surface = EPavement::Grass;
	TestTrue(TEXT("a surface-only reclassify is accepted"), F.Net->SetRunwayFacts(F.A, Grass));
	TestEqual(TEXT("the surface changed"), F.Net->RunwayFactsFor(F.A).Surface, EPavement::Grass);
	TestEqual(TEXT("and the mode did not"), F.Net->RunwayFactsFor(F.A).Use, ERunwayUse::ArrivalsOnly);
	return true;
}

/**
 * THE COMPOSITION: two arrivals dispatched back to back through UGroundTraffic land on TWO
 * runways - the first's claim (raised at dispatch) is what sends the second to B. The shape of
 * samples/2runways.png, where the second waited for the first's runway.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRunwayUseTwoArrivalsTest, "Airside.Model.RunwayUse.TwoArrivalsLandOnTwoRunways",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayUseTwoArrivalsTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTwoRunways F = FTwoRunways::Build(Airframe);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FVector2D Focus(-1000.0, 0.0);

	const int32 First = Traffic->DispatchArrival(*F.Net, Focus, Airframe, 1.0);
	const int32 Second = Traffic->DispatchArrival(*F.Net, Focus, Airframe, 1.0);
	if (!TestTrue(TEXT("the first is dispatched"), First > 0) || !TestTrue(TEXT("and the second - not refused as busy"), Second > 0))
	{
		return false;
	}
	const FRoadAgent* One = Traffic->FindAgent(First);
	const FRoadAgent* Two = Traffic->FindAgent(Second);
	if (!TestNotNull(TEXT("both exist"), One) || !TestNotNull(TEXT("both exist"), Two)) { return false; }
	TestTrue(TEXT("the first lands on A"), One->RunwayHeld.Contains(F.A));
	TestTrue(TEXT("the second on B"), Two->RunwayHeld.Contains(F.B));
	return true;
}

#endif
