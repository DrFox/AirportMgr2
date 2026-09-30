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
#include "Model/OpsDefinition.h"
#include "Model/RoadAgent.h"
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
	// TRAFFIC MOVES THROUGH THE QUIET FRAMES (PR E review): an aircraft taxiing on a line of its own, far from the
	// field. Its claims churn every tick; nothing any of the three cards reads does.
	const FGuidelineNodeId LineA = TestGraph::Node(*Rig.Net, -250000.0, 250000.0);
	const FGuidelineNodeId LineB = TestGraph::Node(*Rig.Net, -50000.0, 250000.0);
	TestGraph::FJoinOptions Options;
	Options.bDerived = false;
	TestGraph::Join(*Rig.Net, LineA, LineB, Options);
	if (!TestTrue(TEXT("an aircraft dispatched"), Rig.Actor->DispatchAgent(TestGraph::Probe(*Rig.Net, LineA, LineB,
		ETraversalClass::Aircraft), UAirsideSettings::ResolveDefaultAirframe()))) { return false; }
	const int32 Mover = Rig.Actor->GetTraffic()->GetNewestAgentId();
	const FVector2D Start = Rig.Traffic().FindAgent(Mover)->LastMotion.Position;
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
	const FRoadAgent* Moved = Rig.Traffic().FindAgent(Mover);
	TestTrue(TEXT("and the aircraft was moving all the while"),
		Moved != nullptr && FVector2D::Distance(Moved->LastMotion.Position, Start) > 100.0);

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
			// THE VEHICLE CATALOGUE, as Attach resolves it (#430): the rig's runtime is never attached, and the fleet's door
			// refuses a kind the catalogue lacks.
			UOpsRuntime::ResolveVehicleCatalogue(*Runtime->GetJobBoard(), *GetDefault<UScenario>());
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorCacheDepotBoughtTest, "AirportMgr.Inspector.Cache.DepotSeesAVehicleBoughtAndSold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorCacheDepotBoughtTest::RunTest(const FString&)
{
	// THE PLAYER'S FLEET DOOR (#417; since #443 FServiceFleet::Add and Withdraw) moves UJobBoard::Revision like every other
	// mutator: within one game minute the card's key holds nothing else that a purchase moves.
	using namespace InspectorCacheTest;
	FDepotRig Rig;
	if (!TestTrue(TEXT("the rig, with a depot"), Rig.Ok() && Rig.Depot.IsSet())) { return false; }
	const FSelection Sel = Rig.Select(ESelectionKind::Stand, Rig.Depot.Index);
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	const int32 Bought = Rig.Runtime->GetJobBoard()->Fleet().Add(TEXT("FUEL"), Rig.Depot, EFleetOrigin::Bought, 0.0);
	if (!TestTrue(TEXT("a vehicle bought"), Bought != 0)) { return false; }
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	const FString Tag = FString::Printf(TEXT("#%d"), Bought);
	TestTrue(FString::Printf(TEXT("the bought vehicle is listed at once ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(Tag));
	TestTrue(TEXT("sold"), Rig.Runtime->GetJobBoard()->Fleet().Withdraw(Bought, EFleetReason::Sold, 0.0));
	Rig.Panel->RefreshWith(Rig.Runtime, Rig.Actor, Sel);
	TestFalse(FString::Printf(TEXT("and gone from the card at once ('%s')"), *Rig.Panel->FactsForTest()),
		Rig.Panel->FactsForTest().Contains(Tag));
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
			Flight->SetPhaseForTest(EFlightPhase::TaxiIn);
			Flight->AcceptedAt = 0.0;
			Flight->ContractSeconds = 3.0 * 3600.0;
			Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);
			Panel = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
		}

		bool Ok() const { return Panel != nullptr && Flight != nullptr && Id > 0; }
		FSelection Sel() const { FSelection S; S.Kind = ESelectionKind::Aircraft; S.Id = Id; return S; }
		void Frame() { Actor->Tick(1.0f / 30.0f); Panel->RefreshWith(Runtime, Actor, Sel()); }

		/** The card's line that starts with Prefix, or empty - the one line under test, since the rest of a taxiing
		 *  aircraft's card (its speed, its heading) moves every frame whatever the line does. */
		FString Line(const TCHAR* Prefix) const
		{
			TArray<FString> Lines;
			Panel->FactsForTest().ParseIntoArrayLines(Lines);
			const FString* Found = Lines.FindByPredicate([Prefix](const FString& Each) { return Each.StartsWith(Prefix); });
			return Found != nullptr ? *Found : FString();
		}
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
	const FString Was = Rig.Line(TEXT("Turnaround"));
	if (!TestFalse(TEXT("a turnaround line"), Was.IsEmpty())) { return false; }
	USimClock& Clock = *Rig.Runtime->GetClock();
	const double Until = Clock.Now() + 90.0;
	for (int32 Step = 0; Step < 100000 && Clock.Now() < Until; ++Step) { Clock.Advance(0.5 / FMath::Max(Clock.TimeScale(), 1e-6)); }
	Rig.Frame();
	TestEqual(TEXT("a minute and a half: composed again"), Rig.Panel->TurnaroundComposeCountForTest(), 2);
	TestNotEqual(FString::Printf(TEXT("and the minutes left moved ('%s' was '%s')"), *Rig.Line(TEXT("Turnaround")), *Was),
		Rig.Line(TEXT("Turnaround")), Was);
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
	TestTrue(FString::Printf(TEXT("the pump has not moved: nothing delivered ('%s')"), *Rig.Line(TEXT("Fuel"))),
		Rig.Line(TEXT("Fuel")).Contains(TEXT("2,900 L left")));
	const double Until = Clock.Now() + 400.0;
	for (int32 Step = 0; Step < 100000 && Clock.Now() < Until; ++Step) { Clock.Advance(0.5 / FMath::Max(Clock.TimeScale(), 1e-6)); }
	Rig.Frame();
	TestTrue(FString::Printf(TEXT("halfway through the trip, 500 L are in ('%s')"), *Rig.Line(TEXT("Fuel"))),
		Rig.Line(TEXT("Fuel")).Contains(TEXT("2,400 L left")));
	return true;
}

/**
 * KEY EQUAL MEANS TEXT EQUAL (issue #441's pin): a card that is handed facts whose key matches the last one it composed
 * from shows the last text, and that text must be the one a fresh composition of THESE facts would give. The gate is an
 * optimisation, so it may never be visible - and it was: the aircraft key held speed as tenths of m/s while the sentence
 * also prints whole knots (their boundaries do not line up: 25.5 and 26 uu/s are one tenth and two different knots), and
 * held heading and altitude by FMath::RoundToInt while the sentence printed them with printf's round-half-even.
 *
 * THE OBSERVABLE FORM, through the real widget, so it holds however the key is built: one long-lived panel follows a
 * fixed-seed random walk of one aircraft's facts (each step moves a few of them by a little, or snaps one to a rounding
 * tie), and after every step its three lines must equal what a panel that could not reuse anything composes for the
 * same facts. The reference is forced to compose by showing it another aircraft first - the key holds the agent id.
 * A failure names the first step and the two texts.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectorKeyEqualMeansTextEqualTest, "AirportMgr.Inspector.KeyEqualMeansTextEqual",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FInspectorKeyEqualMeansTextEqualTest::RunTest(const FString&)
{
	FAirsideTestWorld TestWorld;
	if (!TestNotNull(TEXT("a world"), TestWorld.World) || !TestNotNull(TEXT("an actor"), TestWorld.Actor)) { return false; }
	UInspectorWidget* Walker = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	UInspectorWidget* Reference = CreateWidget<UInspectorWidget>(TestWorld.World, UInspectorWidget::StaticClass());
	if (!TestNotNull(TEXT("the panels"), Walker) || !TestNotNull(TEXT("and the reference"), Reference)) { return false; }

	FRandomStream Stream(441);
	FSelection Sel; Sel.Kind = ESelectionKind::Aircraft; Sel.Id = 1;
	FAgentFacts F;
	F.Id = 1;
	F.TypeName = TEXT("TestType");
	F.Destination = TEXT("Stand 1");
	F.Status = TEXT("Taxiing");
	F.HeadingDegrees = 90.0;
	F.GroundSpeed = 200.0;
	F.Altitude = 0.0;
	const TCHAR* Statuses[] = { TEXT("Taxiing"), TEXT("Parked"), TEXT("Rolling / Climbing") };
	const TCHAR* Destinations[] = { TEXT("Stand 1"), TEXT("Stand 2"), TEXT("Runway 09") };

	int32 Mismatches = 0;
	FString First;
	constexpr int32 Steps = 4000;
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		// A FEW FACTS MOVE A LITTLE: the deltas a taxiing aircraft makes, plus a snap to a half-way value, where two
		// roundings of one number can part company.
		switch (Stream.RandRange(0, 6))
		{
		case 0: F.GroundSpeed += Stream.FRandRange(-6.0f, 6.0f); break;
		case 1: F.GroundSpeed = Stream.RandRange(0, 600) * 5.0 + (Stream.RandRange(0, 1) == 0 ? 0.0 : 0.5); break;
		case 2: F.HeadingDegrees = FMath::Fmod(F.HeadingDegrees + Stream.FRandRange(-1.5f, 1.5f) + 360.0, 360.0); break;
		case 3: F.HeadingDegrees = Stream.RandRange(0, 719) * 0.5; break;
		case 4: F.Altitude = FMath::Max(0.0, F.Altitude + Stream.FRandRange(-120.0f, 120.0f)); break;
		case 5: F.Altitude = Stream.RandRange(0, 200) * 50.0; break;
		default:
			F.bEngineRunning = !F.bEngineRunning;
			F.Status = Statuses[Stream.RandRange(0, 2)];
			F.Destination = Destinations[Stream.RandRange(0, 2)];
			break;
		}
		F.GroundSpeed = FMath::Clamp(F.GroundSpeed, -3000.0, 3000.0);

		Walker->Refresh(TestWorld.Actor, Sel, &F);
		FAgentFacts Other = F;
		Other.Id = 100000 + Step;
		Reference->Refresh(TestWorld.Actor, Sel, &Other);
		Reference->Refresh(TestWorld.Actor, Sel, &F);

		if (Walker->TitleForTest() != Reference->TitleForTest() || Walker->FactsForTest() != Reference->FactsForTest()
			|| Walker->StatusForTest() != Reference->StatusForTest())
		{
			if (Mismatches++ == 0)
			{
				First = FString::Printf(TEXT("step %d (speed %.3f uu/s, heading %.3f, altitude %.3f): the long-lived panel shows '%s' but a fresh composition gives '%s'"),
					Step, F.GroundSpeed, F.HeadingDegrees, F.Altitude, *Walker->FactsForTest().Replace(LINE_TERMINATOR, TEXT(" | ")).Replace(TEXT("\n"), TEXT(" | ")),
					*Reference->FactsForTest().Replace(LINE_TERMINATOR, TEXT(" | ")).Replace(TEXT("\n"), TEXT(" | ")));
			}
		}
	}
	TestEqual(FString::Printf(TEXT("%d of %d steps showed text a fresh composition would not - first: %s"), Mismatches, Steps, *First),
		Mismatches, 0);
	return true;
}

#endif
