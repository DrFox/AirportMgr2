#include "CoreMinimal.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "InspectorWidget.h"
#include "Misc/AutomationTest.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/InspectFacts.h"
#include "Model/JobBoard.h"
#include "Model/RoadEntity.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/TaxiwayStrip.h"
#include "Model/TrafficOccupancy.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "Tool/Selection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * OPS BATCH 3 PR E - the inspector's cards stop describing themselves every tick. Each card's Describe now runs when
 * something it reads moves (FInspectorCardKey, and the aircraft card's three lookups), so each input gets a test that
 * moves IT ALONE and asserts the card says so: a key that missed that input shows stale text, and the test goes red.
 */
namespace InspectorCacheTest
{
	/** A world, the actor, FTestAirport laid onto the actor's own network (one runway, one taxiway, one stand), and
	 *  the panel. The network is derived by the fixture; nothing here goes through the facade unless it says so. */
	struct FRig
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = nullptr;
		URoadNetwork* Net = nullptr;
		FTestAirport Field;
		UInspectorWidget* Panel = nullptr;

		bool Ok() const { return Actor != nullptr && Net != nullptr && Panel != nullptr && Field.Stands.Num() > 0; }

		FRig()
		{
			Actor = TestWorld.Actor;
			if (Actor == nullptr)
			{
				return;
			}
			Actor->PlaceNode(FVector2D(-300000.0, -300000.0));   // a network, for the fixture to build onto
			Net = Actor->Network;
			Field = FTestAirport::Build(UAirsideSettings::ResolveDefaultAirframe(), FTestAirportOptions(), Net);
			Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
		}

		int32 SegmentWhere(TFunctionRef<bool(FRoadSegmentId)> Pred) const
		{
			for (int32 Index = 0; Index < Net->GetSegments().Num(); ++Index)
			{
				const FRoadSegmentId Id = Net->SegmentIdAt(Index);
				if (Id.IsSet() && Pred(Id))
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}
		int32 RunwaySegment() const { return SegmentWhere([this](FRoadSegmentId Id) { return Net->IsRunwaySegment(Id); }); }
		int32 TaxiwaySegment() const { return SegmentWhere([this](FRoadSegmentId Id) { return TaxiwayStrip::HasStrip(*Net, Id); }); }

		FSelection Select(ESelectionKind Kind, int32 Id) const { FSelection S; S.Kind = Kind; S.Id = Id; return S; }
		FGuidelineNodeId StandPose() const { return Net->GetEntity(Field.Stands[0])->PoseNode; }
		UGroundTraffic& Traffic() const { return *Actor->GetTraffic()->GetModel(); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheQuietTest, "AirportMgr.Inspector.Cache.QuietCardsDescribeOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheQuietTest::RunTest(const FString&)
{
	using namespace InspectorCacheTest;
	FRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const TPair<ESelectionKind, int32> Cards[] = {
		{ ESelectionKind::Stand, Rig.Field.Stands[0].Index },
		{ ESelectionKind::Runway, Rig.RunwaySegment() },
		{ ESelectionKind::Taxiway, Rig.TaxiwaySegment() } };
	for (const TPair<ESelectionKind, int32>& Card : Cards)
	{
		const FSelection Sel = Rig.Select(Card.Key, Card.Value);
		const int32 Before = Rig.Panel->CardDescribeCountForTest();
		for (int32 Frame = 0; Frame < 30; ++Frame)
		{
			Rig.Actor->Tick(1.0f / 30.0f);
			Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
		}
		TestTrue(FString::Printf(TEXT("card %d shows"), static_cast<int32>(Card.Key)), Rig.Panel->IsShownForTest());
		TestEqual(FString::Printf(TEXT("card %d: 30 quiet frames describe it once"), static_cast<int32>(Card.Key)),
			Rig.Panel->CardDescribeCountForTest() - Before, 1);
	}

	// A RUNWAY CARD READS NO OCCUPANCY: a hold elsewhere does not recompose it.
	const FSelection Runway = Rig.Select(ESelectionKind::Runway, Rig.RunwaySegment());
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Runway);
	const int32 Before = Rig.Panel->CardDescribeCountForTest();
	Rig.Traffic().HoldStand(-7, Rig.StandPose());
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Runway);
	TestEqual(TEXT("a stand held moves nothing on the runway's card"), Rig.Panel->CardDescribeCountForTest() - Before, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheStandHoldTest, "AirportMgr.Inspector.Cache.StandSeesAHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheStandHoldTest::RunTest(const FString&)
{
	// OccupancyRevision: an accepted flight's hold, made between ticks.
	using namespace InspectorCacheTest;
	FRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const FSelection Sel = Rig.Select(ESelectionKind::Stand, Rig.Field.Stands[0].Index);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestEqual(TEXT("empty to start"), Rig.Panel->StatusForTest(), FString(TEXT("Empty")));
	Rig.Traffic().HoldStand(-7, Rig.StandPose());
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestTrue(FString::Printf(TEXT("held: the card says so ('%s')"), *Rig.Panel->StatusForTest()),
		Rig.Panel->StatusForTest().Contains(TEXT("Reserved")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheStandChurnTest, "AirportMgr.Inspector.Cache.StandSeesChurn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheStandChurnTest::RunTest(const FString&)
{
	// StandHoldChangeCount: a body the claim pass puts on the pose node - no revision moves for it.
	using namespace InspectorCacheTest;
	FRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const FSelection Sel = Rig.Select(ESelectionKind::Stand, Rig.Field.Stands[0].Index);
	Rig.Traffic().Advance(0.05, Rig.Net);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestEqual(TEXT("empty to start"), Rig.Panel->StatusForTest(), FString(TEXT("Empty")));
	const uint32 Revision = Rig.Traffic().OccupancyRevision();
	Rig.Traffic().OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Rig.StandPose()), /*bOccupied*/ true, 2));
	Rig.Traffic().Advance(0.05, Rig.Net);
	if (!TestEqual(TEXT("no revision moved - the case under test"), Rig.Traffic().OccupancyRevision(), Revision)) { return false; }
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestTrue(FString::Printf(TEXT("a body on the stand: the card names it ('%s')"), *Rig.Panel->StatusForTest()),
		Rig.Panel->StatusForTest().Contains(TEXT("#77")));
	Rig.Traffic().OccupancyForTest().ReleaseAll(77);
	Rig.Traffic().Advance(0.05, Rig.Net);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestEqual(TEXT("and it rolls off: empty again"), Rig.Panel->StatusForTest(), FString(TEXT("Empty")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheStandLineTest, "AirportMgr.Inspector.Cache.StandSeesALine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheStandLineTest::RunTest(const FString&)
{
	// GuidelineRevision: a stand with no line to it, then a line - nothing else moves.
	using namespace InspectorCacheTest;
	FRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
	const FEntityInstanceId Lonely = Rig.Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(200000.0, 200000.0), 0.0,
		3600.0, StandDef->PoseRole, StandDef->Trucks);
	const FGuidelineNodeId Pose = Rig.Net->GetEntity(Lonely)->PoseNode;
	if (!TestTrue(TEXT("a stand with a pose"), Pose.IsSet())) { return false; }
	const FSelection Sel = Rig.Select(ESelectionKind::Stand, Lonely.Index);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestTrue(TEXT("no line reaches it"), Rig.Panel->FactsForTest().Contains(TEXT("NOT reachable")));

	const uint32 Edit = Rig.Net->GetEditRevision();
	const FGuidelineNodeId Far = TestGraph::Node(*Rig.Net, 210000.0, 200000.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Rig.Net, Pose, Far, Options);
	if (!TestEqual(TEXT("the line moved no edit revision - the case under test"), Rig.Net->GetEditRevision(), Edit)) { return false; }
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestTrue(FString::Printf(TEXT("a line: reachable ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(TEXT("Reachable by taxiway")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheRunwayMoveTest, "AirportMgr.Inspector.Cache.RunwaySeesAMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheRunwayMoveTest::RunTest(const FString&)
{
	// EditRevision: a drag lengthens the strip - the guideline graph stays behind until the drag ends.
	using namespace InspectorCacheTest;
	FRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const int32 Segment = Rig.RunwaySegment();
	const FSelection Sel = Rig.Select(ESelectionKind::Runway, Segment);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	const FString Was = Rig.Panel->FactsForTest();
	const FRoadSegment* Piece = Rig.Net->GetSegment(Rig.Net->SegmentIdAt(Segment));
	if (!TestNotNull(TEXT("the runway segment"), Piece)) { return false; }
	const FRoadNodeId Threshold = Piece->A;
	const uint32 Guideline = Rig.Net->GetGuidelineRevision();
	Rig.Net->SetNodePosition(Threshold, Rig.Net->GetNode(Threshold)->Position - FVector2D(20000.0, 0.0));
	if (!TestEqual(TEXT("no guideline revision moved - the case under test"), Rig.Net->GetGuidelineRevision(), Guideline)) { return false; }
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestNotEqual(FString::Printf(TEXT("200 m longer: the card's length moves ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest(), Was);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheRunwayFactsTest, "AirportMgr.Inspector.Cache.RunwaySeesItsFacts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheRunwayFactsTest::RunTest(const FString&)
{
	// THROUGH THE FACADE, as the player sets them: a facts change moves no EditRevision, and the card sees it only
	// because the facade's Topology rebuild moves GuidelineRevision. This is the fact FInspectorCardKey (and the held
	// taxi out's gate, and the Land panel) lean on; if the facade stops rebuilding for facts, this goes red.
	using namespace InspectorCacheTest;
	FRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	const int32 Segment = Rig.RunwaySegment();
	const FSelection Sel = Rig.Select(ESelectionKind::Runway, Segment);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestFalse(TEXT("mixed to start"), Rig.Panel->FactsForTest().Contains(TEXT("Arrivals")));
	const uint32 Edit = Rig.Net->GetEditRevision();
	const uint32 Guideline = Rig.Net->GetGuidelineRevision();
	FRunwayFacts Facts = Rig.Net->RunwayFactsFor(Rig.Net->SegmentIdAt(Segment));
	Facts.Use = ERunwayUse::ArrivalsOnly;
	if (!TestTrue(TEXT("the facade takes the facts"), Rig.Actor->SetRunwayFacts(Segment, Facts))) { return false; }
	TestEqual(TEXT("no edit revision moved"), Rig.Net->GetEditRevision(), Edit);
	TestNotEqual(TEXT("the facade's rebuild moved the guideline revision"), Rig.Net->GetGuidelineRevision(), Guideline);
	Rig.Panel->RefreshWith(nullptr, Rig.Actor, Sel);
	TestTrue(FString::Printf(TEXT("the card says arrivals only ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(TEXT("Arrivals")));
	return true;
}

namespace InspectorCacheTest
{
	/** A fuel depot on its own line, far from the field, and a runtime whose job board and clock the card reads. */
	struct FDepotRig : FRig
	{
		UOpsRuntime* Runtime = nullptr;
		FEntityInstanceId Depot;

		FDepotRig()
		{
			if (!Ok())
			{
				return;
			}
			UEntityDefinition* DepotDef = UEntityDefinition::MakeFuelDepotTransient();
			Depot = Net->PlaceEntity(DepotDef, DepotDef->Anchors, FVector2D(-200000.0, 200000.0), 0.0, 0.0,
				DepotDef->PoseRole, DepotDef->Trucks);
			const FGuidelineNodeId Pose = Depot.IsSet() ? Net->GetEntity(Depot)->PoseNode : FGuidelineNodeId();
			if (Pose.IsSet())
			{
				TestGraph::FJoinOptions Options;
				Options.bDerived = false;
				TestGraph::Join(*Net, Pose, TestGraph::Node(*Net, -190000.0, 200000.0), Options);
			}
			Runtime = NewObject<UOpsRuntime>(GetTransientPackage());
		}

		/** Game seconds forward, in real steps the clock turns into game time. */
		void AdvanceGame(double Seconds)
		{
			USimClock& Clock = *Runtime->GetClock();
			const double Until = Clock.Now() + Seconds;
			for (int32 Step = 0; Step < 100000 && Clock.Now() < Until; ++Step) { Clock.Advance(0.5 / FMath::Max(Clock.TimeScale(), 1e-6)); }
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheDepotMinuteTest, "AirportMgr.Inspector.Cache.DepotDescribedOncePerMinute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheDepotMinuteTest::RunTest(const FString&)
{
	// THE MINUTE: a quiet board describes once a game minute - and the figures it shows are the minute's.
	using namespace InspectorCacheTest;
	FDepotRig Rig;
	if (!TestTrue(TEXT("the rig, with a depot"), Rig.Ok() && Rig.Depot.IsSet())) { return false; }
	UJobBoard& Board = *Rig.Runtime->GetJobBoard();
	FServiceVehicle& Bowser = Board.AddVehicleForTest(TEXT("FUEL"), Rig.Depot, EServiceVehicleState::ToJob, 9700.0);
	FServiceJob& Job = Board.AddJobForTest(1, EServiceJobState::Underway, EServiceRefusal::None, 0);
	Job.PromisedFinish = 3600.0;
	Bowser.CurrentJob = Job.Id;

	const FSelection Sel = Rig.Select(ESelectionKind::Stand, Rig.Depot.Index);
	Rig.AdvanceGame(1.0);
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	if (!TestTrue(FString::Printf(TEXT("the depot card lists its job ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(TEXT("stand")))) { return false; }
	const int32 Before = Rig.Panel->DepotDescribeCountForTest();
	const int64 Minute = FMath::FloorToInt64(Rig.Runtime->GetClock()->Now() / 60.0);
	for (int32 Frame = 0; Frame < 20 && FMath::FloorToInt64((Rig.Runtime->GetClock()->Now() + 1.0) / 60.0) == Minute; ++Frame)
	{
		Rig.AdvanceGame(1.0);
		Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	}
	TestEqual(TEXT("within one game minute: no describe"), Rig.Panel->DepotDescribeCountForTest() - Before, 0);
	const FString Was = Rig.Panel->FactsForTest();
	Rig.AdvanceGame(60.0);
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	TestEqual(TEXT("the next minute: one describe"), Rig.Panel->DepotDescribeCountForTest() - Before, 1);
	TestNotEqual(TEXT("and the promise counts down"), Rig.Panel->FactsForTest(), Was);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheDepotBoardTest, "AirportMgr.Inspector.Cache.DepotSeesTheBoard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheDepotBoardTest::RunTest(const FString&)
{
	// UJobBoard::Revision: a vehicle joins the depot within one game minute.
	using namespace InspectorCacheTest;
	FDepotRig Rig;
	if (!TestTrue(TEXT("the rig, with a depot"), Rig.Ok() && Rig.Depot.IsSet())) { return false; }
	const FSelection Sel = Rig.Select(ESelectionKind::Stand, Rig.Depot.Index);
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	TestEqual(TEXT("no jobs to start"), Rig.Panel->StatusForTest(), FString(TEXT("No jobs")));
	FServiceVehicle& Bowser = Rig.Runtime->GetJobBoard()->AddVehicleForTest(TEXT("FUEL"), Rig.Depot, EServiceVehicleState::Idle, 9700.0);
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	TestTrue(FString::Printf(TEXT("the vehicle is listed at once ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(FString::Printf(TEXT("#%d"), Bowser.Id)));
	return true;
}

namespace InspectorCacheTest
{
	/** An aircraft taxiing on a line of its own, and a runtime whose flight board owns a flight for it. */
	struct FAircraftRig
	{
		FAirsideTestWorld TestWorld;
		ARoadNetworkActor* Actor = nullptr;
		UInspectorWidget* Panel = nullptr;
		UOpsRuntime* Runtime = nullptr;
		UFlight* Flight = nullptr;
		int32 Id = 0;

		FAircraftRig()
		{
			Actor = TestWorld.Actor;
			if (Actor == nullptr)
			{
				return;
			}
			Actor->PlaceNode(FVector2D(-100000.0, -100000.0));
			URoadNetwork& Net = *Actor->Network;
			const FGuidelineNodeId A = TestGraph::Node(Net, 0.0, 0.0);
			const FGuidelineNodeId B = TestGraph::Node(Net, 200000.0, 0.0);
			TestGraph::FJoinOptions Options;
			Options.bDerived = false;
			TestGraph::Join(Net, A, B, Options);
			if (!Actor->DispatchAgent(TestGraph::Probe(Net, A, B, ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))
			{
				return;
			}
			Id = Actor->GetTraffic()->GetNewestAgentId();
			Runtime = NewObject<UOpsRuntime>(GetTransientPackage());
			Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->AgentId = Id;
			Flight->Phase = EFlightPhase::TaxiIn;
			Flight->AcceptedAt = 0.0;
			Flight->ContractSeconds = 3.0 * 3600.0;
			Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);
			Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
		}

		bool Ok() const { return Panel != nullptr && Flight != nullptr && Id > 0; }
		FSelection Sel() const { FSelection S; S.Kind = ESelectionKind::Aircraft; S.Id = Id; return S; }
		void Frame() { Actor->Tick(1.0f / 30.0f); Panel->RefreshWith(Runtime, Actor, Sel()); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheAircraftLookupsTest, "AirportMgr.Inspector.Cache.AircraftLookupsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheAircraftLookupsTest::RunTest(const FString&)
{
	// A TAXIING AIRCRAFT recomposes its card every frame (its speed moves); its flight, fuel line and turnaround do not
	// move with it. Then each one's own input moves, and only then is it asked again.
	using namespace InspectorCacheTest;
	FAircraftRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	for (int32 Frame = 0; Frame < 30; ++Frame) { Rig.Frame(); }
	TestEqual(TEXT("30 frames: the flight looked up once"), Rig.Panel->FlightLookupCountForTest(), 1);
	TestEqual(TEXT("the fuel line asked once"), Rig.Panel->FuelLookupCountForTest(), 1);
	TestEqual(TEXT("the turnaround composed once"), Rig.Panel->TurnaroundComposeCountForTest(), 1);
	TestTrue(FString::Printf(TEXT("the turnaround is shown ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(TEXT("Turnaround 3 h")));

	// THE FLIGHT BOARD MOVES (another offer): the flight is looked up again.
	UFlight* Other = NewObject<UFlight>(GetTransientPackage());
	Rig.Runtime->GetFlightBoard()->AddOffer(*Rig.Runtime->GetClock(), Other);
	Rig.Frame();
	TestEqual(TEXT("the board's revision: looked up again"), Rig.Panel->FlightLookupCountForTest(), 2);

	// THE JOB BOARD MOVES: a fuel job for it appears, and the card shows it at once.
	FServiceJob& Job = Rig.Runtime->GetJobBoard()->AddJobForTest(Rig.Id, EServiceJobState::Underway, EServiceRefusal::None, 0);
	Job.QuantityOwed = 300.0;
	Rig.Frame();
	TestEqual(TEXT("the job board's revision: asked again"), Rig.Panel->FuelLookupCountForTest(), 2);
	TestTrue(FString::Printf(TEXT("the fuel line is on the card ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(TEXT("truck en route")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheTurnaroundTest, "AirportMgr.Inspector.Cache.TurnaroundTicksByTheMinute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheTurnaroundTest::RunTest(const FString&)
{
	// THE CLOCK, at the resolution the sentence prints: "N min left" moves once a minute, and the card with it.
	using namespace InspectorCacheTest;
	FAircraftRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	Rig.Frame();
	const FString Was = Rig.Panel->FactsForTest();
	USimClock& Clock = *Rig.Runtime->GetClock();
	const double Until = Clock.Now() + 90.0;
	for (int32 Step = 0; Step < 100000 && Clock.Now() < Until; ++Step) { Clock.Advance(0.5 / FMath::Max(Clock.TimeScale(), 1e-6)); }
	Rig.Frame();
	TestEqual(TEXT("a minute and a half: composed again"), Rig.Panel->TurnaroundComposeCountForTest(), 2);
	TestNotEqual(FString::Printf(TEXT("and the minutes left moved ('%s')"), *Rig.Panel->FactsForTest()), Rig.Panel->FactsForTest(), Was);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheFuelLiveTest, "AirportMgr.Inspector.Cache.FuelLineLiveWhilePumping",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheFuelLiveTest::RunTest(const FString&)
{
	// WHILE PUMPING the fuel line counts litres down with the clock and no board change - so it is asked every frame
	// then, and the card follows it.
	using namespace InspectorCacheTest;
	FAircraftRig Rig;
	if (!TestTrue(TEXT("the rig"), Rig.Ok())) { return false; }
	FServiceJob& Job = Rig.Runtime->GetJobBoard()->AddJobForTest(Rig.Id, EServiceJobState::Serving, EServiceRefusal::None, 0);
	Job.QuantityOwed = 2900.0;
	Job.TankLitres = 1000.0;
	Job.TripQuantity = 1000.0;
	USimClock& Clock = *Rig.Runtime->GetClock();
	Job.TripStartedAt = Clock.Now();
	Job.TripEndsAt = Clock.Now() + 800.0;
	Rig.Frame();
	const FString Was = Rig.Panel->FactsForTest();
	const double Until = Clock.Now() + 400.0;
	for (int32 Step = 0; Step < 100000 && Clock.Now() < Until; ++Step) { Clock.Advance(0.5 / FMath::Max(Clock.TimeScale(), 1e-6)); }
	Rig.Frame();
	TestNotEqual(FString::Printf(TEXT("halfway through the trip, fewer litres are left ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest(), Was);
	return true;
}

#endif
