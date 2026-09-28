#include "CoreMinimal.h"
#include "AirsideTestFixtures.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/RoadNetwork.h"
#include "Model/RunwayQuery.h"
#include "Profiles/RoadProfile.h"
#include "Solve/RunwayDesignator.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE RUNWAY IN USE AS DATA (spec 2026-09-28-runway-in-use, rulings 1 and 5): a runway
 * remembers ONE direction, as a designator, and every edit that reshapes the strip keeps it.
 *
 * North is +X in RunwayDesignator, so a strip drawn from -X toward +X is 36 and its
 * reciprocal 18; drawn along +Y it is 09/27.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseFactsTest,
	"Airside.Model.RunwayInUse.Facts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseFactsTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	URoadProfile* Runway = TestProfiles::Runway();

	// DRAWN from -X toward +X: the take-off heads +X, runway 36. The unset default would be
	// the LOWER designator, 18, so this tells "drawn" apart from "default".
	const FRoadNodeId T = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId F = Net->AddNode(FVector2D(100000.0, 0.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(T, F, Runway);
	TestEqual(TEXT("drawn -X to +X, the runway in use is 36 (the draw direction, not the lower 18)"),
		Net->RunwayFactsFor(Strip).InUse, 36);

	// AN EXTENSION drawn BACK toward the strip (new piece A at the far end, B beyond) takes the
	// CHAIN's direction. Drawn from a new node toward F it would otherwise be 18.
	const FRoadNodeId Beyond = Net->AddNode(FVector2D(140000.0, 0.0));
	const FRoadSegmentId Extension = Net->AddStraightSegment(Beyond, F, Runway);
	TestEqual(TEXT("the extension shares the chain"), Net->RunwayChain(Strip).Num(), 2);
	TestEqual(TEXT("an extension drawn the other way keeps the strip's 36, not its own 18"),
		Net->RunwayFactsFor(Extension).InUse, 36);

	// A SPLIT (an exit cut in) keeps it on both halves.
	const FRoadNodeId Mid = Net->SplitSegment(Strip, FVector2D(50000.0, 0.0));
	TestTrue(TEXT("split"), Mid.IsSet());
	for (const FRoadSegmentId& Member : Net->RunwayChain(Extension))
	{
		TestEqual(TEXT("every piece after the split still says 36"), Net->RunwayFactsFor(Member).InUse, 36);
	}

	// THE RESOLVER: whichever end a query happened to land on, InUseEnd points 36 (+X).
	FRunwayEnd AtWest, AtEast;
	TestTrue(TEXT("found from the west end"), Net->NearestRunwayThreshold(FVector2D(-5000.0, 0.0), AtWest));
	TestTrue(TEXT("found from the east end"), Net->NearestRunwayThreshold(FVector2D(150000.0, 0.0), AtEast));
	TestTrue(TEXT("the raw queries disagree - that is the bug InUseEnd exists for"),
		AtWest.Direction.X * AtEast.Direction.X < 0.0);
	const FRunwayEnd FromWest = Net->InUseEnd(AtWest);
	const FRunwayEnd FromEast = Net->InUseEnd(AtEast);
	TestEqual(TEXT("from the west, in use is 36"), RunwayDesignator::Designate(FromWest.Direction), 36);
	TestEqual(TEXT("from the east, in use is STILL 36"), RunwayDesignator::Designate(FromEast.Direction), 36);
	TestTrue(TEXT("and it is the same threshold either way"), FromWest.Threshold.Equals(FromEast.Threshold, 1.0));

	// FLIPPED through the facts: the resolver follows.
	FRunwayFacts Facts = Net->RunwayFactsFor(Extension);
	Facts.InUse = 18;
	TestTrue(TEXT("set"), Net->SetRunwayFacts(Extension, Facts));
	TestEqual(TEXT("flipped, in use is 18"), RunwayDesignator::Designate(Net->InUseEnd(AtWest).Direction), 18);

	// ROTATED: drag the far end round ~30 degrees. The stored 18 is no longer either end's
	// exact designator, and the NEARER end still wins.
	TestTrue(TEXT("dragged"), Net->SetNodePosition(Beyond, FVector2D(120000.0, 70000.0)));
	FRunwayEnd Rotated;
	TestTrue(TEXT("still a runway"), Net->NearestRunwayThreshold(FVector2D(-5000.0, 0.0), Rotated));
	const int32 Now = RunwayDesignator::Designate(Net->InUseEnd(Rotated).Direction);
	TestTrue(FString::Printf(TEXT("rotated, the in-use end is the one heading roughly 18 (got %d)"), Now),
		Now >= 18 && Now <= 23);
	return true;
}

/**
 * InUse 0 is a runway saved before the field existed. It resolves to the LOWER designator,
 * whichever end the caller held - deterministic, never "whatever end the query was nearest",
 * which is the rule that caused samples/deadlock.png.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseUnsetTest,
	"Airside.Model.RunwayInUse.UnsetIsLowerDesignator",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseUnsetTest::RunTest(const FString& Parameters)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 0.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(0.0, 100000.0));
	const FRoadSegmentId Strip = Net->AddStraightSegment(A, B, TestProfiles::Runway());
	TestEqual(TEXT("drawn along +Y: 09"), Net->RunwayFactsFor(Strip).InUse, 9);

	// A SURFACE RECLASSIFY with a fresh struct (InUse 0) keeps the direction - review focus 2.
	FRunwayFacts Grass;
	Grass.Surface = EPavement::Grass;
	Net->SetRunwayFacts(Strip, Grass);
	TestEqual(TEXT("reclassified to grass, the runway in use is still 09"), Net->RunwayFactsFor(Strip).InUse, 9);
	TestEqual(TEXT("and it IS grass now"), Net->RunwayFactsFor(Strip).Surface, EPavement::Grass);

	TestTrue(TEXT("cleared, as a pre-field level loads"), FRoadNetworkTestAccess(*Net).ClearRunwayInUseForTest(Strip));
	FRunwayFacts Legacy = Net->RunwayFactsFor(Strip);

	FRunwayEnd NearA, NearB;
	Net->NearestRunwayThreshold(FVector2D(0.0, -5000.0), NearA);
	Net->NearestRunwayThreshold(FVector2D(0.0, 105000.0), NearB);
	TestEqual(TEXT("unset, from A: the lower designator, 09"), RunwayDesignator::Designate(Net->InUseEnd(NearA).Direction), 9);
	TestEqual(TEXT("unset, from B: still 09"), RunwayDesignator::Designate(Net->InUseEnd(NearB).Direction), 9);

	// And set to the OTHER end, the high one wins - so the lower is a default, not a bias.
	Legacy.InUse = 27;
	Net->SetRunwayFacts(Strip, Legacy);
	TestEqual(TEXT("set to 27, from A: 27"), RunwayDesignator::Designate(Net->InUseEnd(NearA).Direction), 27);
	return true;
}

namespace
{
	/** Point the strip Seed belongs to at Designator. */
	void UseRunway(URoadNetwork& Net, FRoadSegmentId Seed, int32 Designator)
	{
		FRunwayFacts Facts = Net.RunwayFactsFor(Seed);
		Facts.InUse = Designator;
		Net.SetRunwayFacts(Seed, Facts);
	}
}

/**
 * A DEPARTURE TAKES OFF FROM THE END IN USE, and only that end, whichever gives the shorter
 * taxi. Asked once per direction on the SAME airport from the SAME stand: the old rule
 * (shortest taxi over both ends) answers both identically, so one of the two assertions
 * below fails under it - which is what makes this measure the rule rather than a value.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseDepartTest,
	"Airside.Model.RunwayInUse.DepartsFromTheEndInUse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseDepartTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport A = FTestAirport::Build(Airframe);
	const FGuidelineNodeId Stand = A.Pose(A.Stands[0]);
	TestEqual(TEXT("fixture: drawn threshold-first along +X, in use 36"), A.Net->RunwayFactsFor(A.ThresholdSegment).InUse, 36);

	for (const int32 Wanted : { 36, 18 })
	{
		UseRunway(*A.Net, A.ThresholdSegment, Wanted);
		const FDeparturePlan Plan = DeparturePlanner::PlanAny(*A.Net, Stand, Airframe, ETraversalClass::Aircraft);
		if (!TestTrue(FString::Printf(TEXT("in use %02d: a departure plans (%s)"), Wanted, *DeparturePlanner::Describe(Plan)), Plan.IsValid()))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("in use %02d: it rolls %02d"), Wanted, Wanted),
			RunwayDesignator::Designate(Plan.End.Direction), Wanted);
	}
	return true;
}

/**
 * A LANDING COMES IN OVER THE END IN USE, wherever the flight's approach focus is. The focus
 * sits past the FAR end, where nearest-end landed it 18; in use 36 it lands 36, over the
 * drawn threshold. Then the mirror: focus before the drawn threshold, in use 18.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseLandTest,
	"Airside.Model.RunwayInUse.LandsOverTheEndInUse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseLandTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport A = FTestAirport::Build(Airframe);
	FRunwayEnd Near;
	A.Net->NearestRunwayThreshold(A.Threshold, Near);
	const FVector2D BeyondFarEnd = Near.FarEnd() + Near.Direction * 1000.0;

	UseRunway(*A.Net, A.ThresholdSegment, 36);
	const FArrivalPlan In36 = ArrivalPlanner::Plan(*A.Net, BeyondFarEnd, Airframe);
	TestEqual(TEXT("focus past the far end, in use 36: lands 36"), RunwayDesignator::Designate(In36.End.Direction), 36);
	TestTrue(TEXT("over the drawn threshold"), In36.End.Threshold.Equals(A.Threshold, 1.0));
	TestTrue(FString::Printf(TEXT("and the plan is whole: %s"), *ArrivalPlanner::DescribeRefusal(In36)), In36.IsValid());

	UseRunway(*A.Net, A.ThresholdSegment, 18);
	const FArrivalPlan In18 = ArrivalPlanner::Plan(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	TestEqual(TEXT("focus before the drawn threshold, in use 18: lands 18"), RunwayDesignator::Designate(In18.End.Direction), 18);
	return true;
}

/**
 * NO EXIT AHEAD IN THE DIRECTION IN USE REFUSES (ruling 3) - and the sentence names the
 * direction and says the OTHER way would work. samples/deadlock.png's shape: the one exit near
 * one end. FTestAirport's far end is pulled in to 1.6 N (N = the landing distance), so landing
 * 36 the exit at 1.2 N is well down the strip, and landing 18 it is 0.4 N in - behind the
 * aircraft before it has slowed. The strip's own dead-end node is then the only "exit" left,
 * which reaches no stand: NoRouteToStand, not NoExit, and that is the refusal the player
 * actually meets, so it is the one asserted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseNoExitTest,
	"Airside.Model.RunwayInUse.NoExitAheadNamesTheFix",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseNoExitTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FTestAirport A = FTestAirport::Build(Airframe);
	URoadNetwork& Net = *A.Net;
	FRunwayEnd Drawn;
	Net.NearestRunwayThreshold(A.Threshold, Drawn);
	const double N = Drawn.Length / 3.0;
	for (int32 Index = 0; Index < Net.GetNodes().Num(); ++Index)
	{
		const FRoadNodeId Id = Net.NodeIdAt(Index);
		if (Id.IsSet() && Net.GetNodes()[Index].Position.Equals(Drawn.FarEnd(), 1.0))
		{
			Net.SetNodePosition(Id, FVector2D(1.6 * N, 0.0));
		}
	}
	TestGraph::Rebuild(Net);

	UseRunway(Net, A.ThresholdSegment, 36);
	const FArrivalPlan Ahead = ArrivalPlanner::Plan(Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	TestTrue(FString::Printf(TEXT("landing 36 the exit is ahead and the plan is whole: %s"), *ArrivalPlanner::DescribeRefusal(Ahead)),
		Ahead.IsValid());

	UseRunway(Net, A.ThresholdSegment, 18);
	const FArrivalPlan Behind = ArrivalPlanner::Plan(Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	const FString Why = ArrivalPlanner::DescribeRefusal(Behind);
	TestTrue(FString::Printf(TEXT("landing 18, the exit behind the touchdown is no exit: %s"), *Why),
		Behind.Why == EArrivalRefusal::NoExit || Behind.Why == EArrivalRefusal::NoRouteToStand);
	TestTrue(TEXT("the planner saw the other way would serve"), Behind.bOtherEndWouldServe);
	TestTrue(TEXT("names the direction refused"), Why.Contains(TEXT("landing 18")));
	TestTrue(TEXT("names the direction that works, and the fix"), Why.Contains(TEXT("Landing 36 would reach a stand - change the runway in use")));
	TestTrue(TEXT("the reason-only NoExit sentence offers the flip too"),
		ArrivalPlanner::DescribeRefusal(EArrivalRefusal::NoExit).Contains(TEXT("change the runway in use")));

	// And NOT on a field where the other way is no better: with no stand at all, neither
	// direction serves, so the sentence must not send the player to flip a runway for nothing.
	Net.RemoveEntity(A.Stands[0]);
	TestGraph::Rebuild(Net);
	const FArrivalPlan Nowhere = ArrivalPlanner::Plan(Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe);
	TestFalse(TEXT("no stand either way: no flip advice"), Nowhere.bOtherEndWouldServe);
	return true;
}

namespace
{
	/**
	 * THE AIRPORT samples/deadlock.png SHOULD HAVE BEEN, laid so a runway in use makes a loop:
	 * an ENTRY connector near the 36 threshold, an EXIT connector half way down, a parallel
	 * taxiway joining their feet, and two stands on an apron stub off it. Landing 36 the first
	 * usable exit is the downstream one (the entry sits before the aircraft has slowed); taking
	 * off 36 the first entry with roll enough is the upstream one. Arrivals and departures then
	 * go ROUND the loop the same way instead of meeting on one connector.
	 */
	struct FLoopAirport
	{
		URoadNetwork* Net = nullptr;
		FRoadSegmentId Strip;
		FVector2D Threshold = FVector2D::ZeroVector;
		TArray<FEntityInstanceId> Stands;
	};

	FLoopAirport BuildLoopAirport(const FAirframe& Airframe)
	{
		FLoopAirport Out;
		Out.Net = NewObject<URoadNetwork>(GetTransientPackage());
		URoadNetwork& Net = *Out.Net;
		const double N = FLandingRun::RequiredLandingDistance(Airframe.Chassis.Ground, Airframe.Climb, Airframe.Approach)
			* FLandingRun::LandingMargin;
		URoadProfile* Runway = TestProfiles::Runway();
		URoadProfile* Taxiway = TestProfiles::Taxiway();
		constexpr double Down = 20000.0;

		const FRoadNodeId T = Net.AddNode(FVector2D(0.0, 0.0));
		const FRoadNodeId En = Net.AddNode(FVector2D(0.15 * N, 0.0));
		const FRoadNodeId Ex = Net.AddNode(FVector2D(1.5 * N, 0.0));
		const FRoadNodeId F = Net.AddNode(FVector2D(3.0 * N, 0.0));
		Out.Strip = TestGraph::Lay(Net, T, En, Runway);
		TestGraph::Lay(Net, En, Ex, Runway);
		TestGraph::Lay(Net, Ex, F, Runway);
		const FRoadNodeId EnFoot = Net.AddNode(FVector2D(0.15 * N, -Down));
		const FRoadNodeId ExFoot = Net.AddNode(FVector2D(1.5 * N, -Down));
		TestGraph::Lay(Net, En, EnFoot, Taxiway);
		TestGraph::Lay(Net, Ex, ExFoot, Taxiway);
		// THE APRON OFF THE PARALLEL, not off a connector: a stand on the exit connector
		// pushes back up it toward the runway and holds there, on the arrivals' only way in
		// (measured 2026-09-28 - a pushback of the whole 20206 uu taxi-in; a pushback-planner
		// question, not a runway-direction one, and not what this test is about).
		const FRoadNodeId Mid = Net.AddNode(FVector2D(0.8 * N, -Down));
		const FRoadNodeId Apron = Net.AddNode(FVector2D(0.8 * N, -Down - 20000.0));
		TestGraph::Lay(Net, EnFoot, Mid, Taxiway);
		TestGraph::Lay(Net, Mid, ExFoot, Taxiway);
		TestGraph::Lay(Net, Mid, Apron, Taxiway);
		TestGraph::Derive(Net);

		// FTestAirport's placement: east of a north-south taxiway, facing east, 6000 apart.
		for (int32 Index = 0; Index < 2; ++Index)
		{
			UEntityDefinition* Stand = UEntityDefinition::MakeStandTransient();
			Out.Stands.Add(Net.PlaceEntity(Stand, Stand->Anchors,
				FVector2D(0.8 * N, -Down) + FVector2D(9000.0, -8000.0 - 6000.0 * Index), 0.0));
		}
		FAnchorLink::Build(Net, UAirsideSettings::ResolveLargestServiceVehicle());
		return Out;
	}

	/** Advance until Done() or Limit seconds; true when Done() held. */
	template <typename FDone>
	bool AdvanceUntil(UGroundTraffic& Traffic, const URoadNetwork& Net, double Limit, FDone&& Done)
	{
		for (double Clock = 0.0; Clock < Limit; Clock += 0.05)
		{
			Traffic.Advance(0.05, &Net);
			if (Done()) { return true; }
		}
		return false;
	}

	bool IsParked(const UGroundTraffic& Traffic, int32 Id)
	{
		const FRoadAgent* Agent = Traffic.FindAgent(Id);
		return Agent != nullptr && Agent->Phase == EAgentPhase::Parked;
	}
}

/**
 * THE DEADLOCK REGRESSION (samples/deadlock.png): a departure sent while the next arrival is
 * landing. Measured every tick, for every agent: nothing is ever armed to roll against the
 * runway in use - the shape that put two aircraft nose to nose. And both complete: the
 * departure leaves, the arrival parks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseTrafficFlowTest,
	"Airside.Model.RunwayInUse.ArrivalsAndDeparturesFlowOneWay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseTrafficFlowTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FLoopAirport A = BuildLoopAirport(Airframe);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	const FVector2D Focus = A.Threshold - FVector2D(1000.0, 0.0);

	const int32 First = Traffic->DispatchArrival(*A.Net, Focus, Airframe, 1.0);
	if (!TestTrue(TEXT("the first arrival is dispatched"), First > 0)) { return false; }
	if (!TestTrue(TEXT("and parks"), AdvanceUntil(*Traffic, *A.Net, 900.0, [&] { return IsParked(*Traffic, First); }))) { return false; }

	const int32 Second = Traffic->DispatchArrival(*A.Net, Focus, Airframe, 1.0);
	const EDepartureRefusal Sent = Traffic->DepartAgent(First, *A.Net);
	if (!TestTrue(TEXT("the second arrival is dispatched"), Second > 0)) { return false; }
	if (!TestEqual(TEXT("the first aircraft is sent to depart"), Sent, EDepartureRefusal::None)) { return false; }

	bool bEverAgainst = false;
	bool bEverArmed = false;
	FString Against;
	const bool bDone = AdvanceUntil(*Traffic, *A.Net, 1800.0, [&]
	{
		for (const int32 Id : { First, Second })
		{
			const FRoadAgent* Agent = Traffic->FindAgent(Id);
			if (Agent == nullptr || !Agent->bDepartureArmed) { continue; }
			bEverArmed = true;
			const int32 Rolls = RunwayDesignator::Designate(Agent->DepartureOrder.End.Direction);
			if (Rolls != 36 && !bEverAgainst)
			{
				bEverAgainst = true;
				Against = FString::Printf(TEXT("agent %d armed to roll %02d"), Id, Rolls);
			}
		}
		return Traffic->FindAgent(First) == nullptr && IsParked(*Traffic, Second);
	});
	TestTrue(TEXT("the departure was armed at some point"), bEverArmed);
	TestFalse(FString::Printf(TEXT("nothing was ever armed against the runway in use (%s)"), *Against), bEverAgainst);
	TestTrue(TEXT("no deadlock: the departure left and the arrival parked"), bDone);
	return true;
}

/**
 * A FLIP REACHES THE NEXT DEPARTURE, AND ONLY THE NEXT (ruling 2). Flip to 18 before sending:
 * it rolls 18. Then, armed, flip back to 36 and rebuild the traffic's plans - the re-arm keeps
 * the 18 it was planned on, rather than turning round on the entry it has already chosen.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRunwayInUseFlipTest,
	"Airside.Model.RunwayInUse.FlipReachesOnlyNewPlans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FRunwayInUseFlipTest::RunTest(const FString& Parameters)
{
	const FAirframe Airframe = TestAirframes::Piper();
	const FLoopAirport A = BuildLoopAirport(Airframe);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());

	const int32 Id = Traffic->DispatchArrival(*A.Net, A.Threshold - FVector2D(1000.0, 0.0), Airframe, 1.0);
	if (!TestTrue(TEXT("lands and parks"), Id > 0
		&& AdvanceUntil(*Traffic, *A.Net, 900.0, [&] { return IsParked(*Traffic, Id); }))) { return false; }

	UseRunway(*A.Net, A.Strip, 18);
	if (!TestEqual(TEXT("sent to depart after the flip"), Traffic->DepartAgent(Id, *A.Net), EDepartureRefusal::None)) { return false; }
	if (!TestTrue(TEXT("armed"), AdvanceUntil(*Traffic, *A.Net, 300.0, [&]
		{ const FRoadAgent* P = Traffic->FindAgent(Id); return P != nullptr && P->bDepartureArmed; }))) { return false; }
	TestEqual(TEXT("planned after the flip, it rolls 18"),
		RunwayDesignator::Designate(Traffic->FindAgent(Id)->DepartureOrder.End.Direction), 18);

	UseRunway(*A.Net, A.Strip, 36);
	Traffic->OnGraphRebuilt(*A.Net);
	const FRoadAgent* After = Traffic->FindAgent(Id);
	if (TestTrue(TEXT("still armed after the rebuild"), After != nullptr && After->bDepartureArmed))
	{
		TestEqual(TEXT("flipped back while armed: the plan it holds still rolls 18"),
			RunwayDesignator::Designate(After->DepartureOrder.End.Direction), 18);
	}
	return true;
}

#endif
