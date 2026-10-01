#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Content/AirsideSettings.h"
#include "Model/Airport.h"
#include "Model/ArrivalPlanner.h"
#include "Model/ArrivalSequencer.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/LandingRun.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadNetwork.h"
#include "Model/RouteSearch.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Model/TrafficOccupancy.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"
#include "OpsEventsTestListener.h"
#include "OpsTransitionTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The arrival queue as a bus pass (ops batch 3 §5): Airside's derived OnRunwayFreed / OnStandsFreed, bridged by
 * UOpsRuntime::Attach, and the "ArrivalQueue" pass they and six other events dirty in place of the per-frame
 * UFlightBoard::TickQueue call. Each test fails if one seam is unwired.
 */
namespace ArrivalQueuePassTest
{
	constexpr double Frame = 1.0 / 30.0;

	/**
	 * Every LogOpsBus Warning naming the safety pass, while registered. UNBUFFERED (CanBeUsedOnMultipleThreads), or
	 * the dedicated log thread delivers the line after the spy is gone - FLogLineSpy's #216 reason. Not FLogLineSpy
	 * itself: that captures Log verbosity only, and this line is a Warning.
	 */
	struct FSafetyWarningSpy : public FOutputDevice
	{
		TArray<FString> Lines;
		FSafetyWarningSpy() { GLog->AddOutputDevice(this); }
		virtual ~FSafetyWarningSpy() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == FName(TEXT("LogOpsBus")) && (Verbosity & ELogVerbosity::VerbosityMask) == ELogVerbosity::Warning
				&& FCString::Strstr(V, TEXT("safety pass")) != nullptr)
			{
				Lines.Add(V);
			}
		}
	};

	/**
	 * An attached runtime over a world actor whose network holds Build's field, laid BEFORE the attach so the airport
	 * is open (an accept asks). 1 game s per real s, so the safety net's 30 s is 900 frames. Prefixed: unity build.
	 */
	struct FRig
	{
		FAirsideTestWorld World;
		UOpsRuntime* Runtime = nullptr;
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Model = nullptr;
		FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

		template <typename FBuildField>
		bool Attach(FBuildField BuildField)
		{
			if (World.Actor == nullptr)
			{
				return false;
			}
			// A NETWORK, which a fresh actor lacks until its first edit - and the runtime's Tick reaches the model
			// only through one (the old RuntimeTicksTheQueue's reason). Far from any field Build lays.
			World.Actor->PlaceNode(FVector2D(0.0, 90000.0));
			Net = World.Actor->Network;
			if (Net == nullptr)
			{
				return false;
			}
			BuildField(*Net);
			Runtime = NewObject<UOpsRuntime>();
			Runtime->Attach(World.Actor);
			Runtime->GetClock()->SetUniformDay(USimClock::SecondsPerDay);
			Model = World.Actor->GetTraffic()->GetModel();
			return Model != nullptr;
		}

		/** An offer accepted with Lead game seconds to its ETA, aimed at Focus; null if refused. */
		UFlight* Accept(const FVector2D& Focus, double Lead = 0.0)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Id = Runtime->GetFlightBoard()->TakeNextId();
			Flight->Airframe = Airframe;
			Flight->OfferWindowSeconds = 60.0;
			Flight->OfferSecondsLeft = 60.0;
			Flight->LeadTimeSeconds = Lead;
			Flight->RunwayPreference = Focus;
			Runtime->GetFlightBoard()->AddOffer(*Runtime->GetClock(), Flight);
			return Runtime->GetFlightBoard()->Accept(*Model, *Net, *Runtime->GetClock(), *Flight) ? Flight : nullptr;
		}

		int32 QueueRuns() const { return Runtime->GetFlightBoard()->TickQueueCallsForTest(); }
	};

	/** Every surface of Seed's strip claimed OCCUPIED by a fake holder, straight into the table - no agent, no event. */
	void HoldStrip(UGroundTraffic& Model, const URoadNetwork& Net, FRoadSegmentId Seed, int32 Holder)
	{
		for (const FTrafficResource& Surface : Net.RunwaySurfaces(Seed))
		{
			Model.OccupancyForTest().Assert(FTrafficClaim::Make(Holder, Surface, /*bOccupied*/ true, 2));
		}
	}
}

using ArrivalQueuePassTest::FRig;
using ArrivalQueuePassTest::FSafetyWarningSpy;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueFreedBridgedTest, "AirportOps.Present.Bus.FreedIsBridged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueFreedBridgedTest::RunTest(const FString&)
{
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([](URoadNetwork&) {}))) { return false; }
	FOpsEventBus& Bus = Rig.Runtime->GetBus();

	// THROUGH THE MODEL'S OWN DELEGATES: the broadcast is the delegate the bridge binds (#445 item 6).
	const int32 Before = Bus.QueuedCount();
	Rig.Model->OnRunwayFreed.Broadcast(FRoadSegmentId());
	TestEqual(TEXT("a freed runway is published onto the bus"), Bus.QueuedCount(), Before + 1);
	Rig.Model->OnStandsFreed.Broadcast(TArray<FGuidelineNodeId>{ FGuidelineNodeId() });
	TestEqual(TEXT("and freed stands"), Bus.QueuedCount(), Before + 2);

	// AND UNBOUND BY A DETACH (Attach(nullptr) detaches first): a runtime no longer driving the field hears nothing.
	Rig.Runtime->Attach(nullptr);
	const int32 Detached = Bus.QueuedCount();
	Rig.Model->OnRunwayFreed.Broadcast(FRoadSegmentId());
	Rig.Model->OnStandsFreed.Broadcast(TArray<FGuidelineNodeId>{ FGuidelineNodeId() });
	TestEqual(TEXT("detached: nothing more is published"), Bus.QueuedCount(), Detached);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueInboundTest, "AirportOps.Model.FlightBoard.EnqueuePublishesInbound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueInboundTest::RunTest(const FString&)
{
	// THE QUEUE'S WAKE-UP: a flight whose ETA comes joins the queue from a clock callback, and says so.
	const FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();
	const FTestAirport Field = FTestAirport::Build(Airframe);
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	Clock->SetUniformDay(USimClock::SecondsPerDay);
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	FOpsEventBus Bus;
	TArray<FFlightInboundEvent> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FFlightInboundEvent>(EOpsTier::Sim, TEXT("test"), [&Seen](const FFlightInboundEvent& E) { Seen.Add(E); });
	Bus.EndWiring();
	Board->Bus = &Bus;

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Id = Board->TakeNextId();
	Flight->Airframe = Airframe;
	Flight->AirlineId = TEXT("Cumbria");
	Flight->LeadTimeSeconds = 10.0;
	Flight->RunwayPreference = Field.Threshold;
	Board->AddOffer(*Clock, Flight);
	if (!TestTrue(TEXT("accepted"), Board->Accept(*Traffic, *Field.Net, *Clock, *Flight))) { return false; }
	Bus.Drain();
	TestEqual(TEXT("an accept is not an arrival"), Seen.Num(), 0);
	Clock->Advance(11.0);
	Bus.Drain();
	TestEqual(TEXT("it is holding"), Flight->GetPhase(), EFlightPhase::Inbound);
	if (TestEqual(TEXT("and its ETA published FlightInbound once"), Seen.Num(), 1))
	{
		TestEqual(TEXT("naming the flight"), Seen[0].FlightId, Flight->Id);
		TestEqual(TEXT("and its airline"), Seen[0].AirlineId, FName(TEXT("Cumbria")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueQuietTest, "AirportOps.Present.ArrivalQueue.QuietQueueRunsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueQuietTest::RunTest(const FString&)
{
	// THE POINT OF THE PASS, measured: a quiet airport costs the queue nothing - it used to be asked every frame
	// (the old AirportOps.Present.RuntimeTicksTheQueue pinned exactly that, one call a tick).
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&Rig](URoadNetwork& Net) { FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	TestNotNull(TEXT("the board has a sequencer"), Rig.Runtime->GetFlightBoard()->Sequencer.Get());
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestTrue(TEXT("the attach ran it once to catch up"), Rig.QueueRuns() >= 1);
	const int32 Settled = Rig.QueueRuns();
	for (int32 Tick = 0; Tick < 300; ++Tick)
	{
		Rig.Model->Advance(ArrivalQueuePassTest::Frame, Rig.Net);
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	}
	TestEqual(TEXT("ten quiet seconds with nobody holding run the queue not once"), Rig.QueueRuns(), Settled);
	TestFalse(TEXT("and arm no safety net"), Rig.Runtime->IsSafetyNetArmedForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueDirtiersTest, "AirportOps.Present.ArrivalQueue.EachEventDirtiesIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueDirtiersTest::RunTest(const FString&)
{
	// EACH DIRTIER BY NAME, so removing one subscription turns exactly one line red.
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&Rig](URoadNetwork& Net) { FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	FOpsEventBus& Bus = Rig.Runtime->GetBus();

	auto RunsFor = [&Rig](TFunctionRef<void()> Cause)
	{
		const int32 Before = Rig.QueueRuns();
		Cause();
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
		return Rig.QueueRuns() - Before;
	};
	TestEqual(TEXT("nothing: no run"), RunsFor([]() {}), 0);
	TestEqual(TEXT("a runway freed"), RunsFor([&Bus]() { Bus.Publish(FRunwayFreedEvent{}); }), 1);
	TestEqual(TEXT("stands freed"), RunsFor([&Bus]() { Bus.Publish(FStandsFreedEvent{}); }), 1);
	TestEqual(TEXT("an offer accepted"), RunsFor([&Bus]() { Bus.Publish(FOfferAcceptedEvent{}); }), 1);
	TestEqual(TEXT("a flight inbound"), RunsFor([&Bus]() { Bus.Publish(FFlightInboundEvent{}); }), 1);
	TestEqual(TEXT("the network changed"), RunsFor([&Bus]() { Bus.Publish(FNetworkChangedEvent{}); }), 1);
	TestEqual(TEXT("the airport's status changed"), RunsFor([&Bus]() { Bus.Publish(FAirportStatusChangedEvent{}); }), 1);
	TestEqual(TEXT("a resume (the speed changed)"), RunsFor([&Bus]() { Bus.Publish(FSpeedChangedEvent{}); }), 1);
	TestEqual(TEXT("a new arrival's Arriving"), RunsFor([&Bus]() { Bus.Publish(FAgentPhaseEvent{ OpsTestTransition(4242, EAgentPhase::Gone, EAgentPhase::Arriving, EAgentEvent::Dispatched) }); }), 1);
	TestEqual(TEXT("any other phase change: no run"), RunsFor([&Bus]() { Bus.Publish(FAgentPhaseEvent{ OpsTestTransition(4242, EAgentPhase::Taxiing, EAgentPhase::Parked, EAgentEvent::Parked) }); }), 0);

	// A LOAD: nothing before it is an event any more, so MarkAllDirty runs it.
	const FString Slot = TEXT("AirportOpsTest_QueuePassLoad");
	if (!TestTrue(TEXT("saved"), Rig.Runtime->SaveToSlot(Slot))) { return false; }
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	TestEqual(TEXT("a load"), RunsFor([&Rig, &Slot]() { Rig.Runtime->LoadFromSlot(Slot); }) >= 1, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueMarksAlertsTest, "AirportOps.Present.ArrivalQueue.QueueRunMarksTheAlertsPass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueMarksAlertsTest::RunTest(const FString&)
{
	// #445 REVIEW, the belt: FlightCannotLand is derived from what the queue keeps, and the offer-minute catch-all that used to look again is gone, so a
	// queue run marks the alerts pass itself. A freed runway is an event the ALERTS do not subscribe to - so the pass running after it is the queue's
	// doing alone.
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&Rig](URoadNetwork& Net) { FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	const int32 Alerts = Rig.Runtime->GetAlerts()->RecomputeCountForTest();
	const int32 Queue = Rig.QueueRuns();
	Rig.Runtime->GetBus().Publish(FRunwayFreedEvent{});
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	if (!TestEqual(TEXT("PRECONDITION: the freed runway ran the queue pass"), Rig.QueueRuns(), Queue + 1)) { return false; }
	TestEqual(TEXT("and the queue pass marked the alerts pass, which ran once after it in the same drain"), Rig.Runtime->GetAlerts()->RecomputeCountForTest(), Alerts + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueCrossingTest, "AirportOps.Present.ArrivalQueue.CrossingClearDispatchesNextFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueCrossingTest::RunTest(const FString&)
{
	// THE CASE THE PER-FRAME POLL EXISTED FOR (FlightBoard.cpp, ClearanceFor's comment): a taxiing aircraft crossing
	// the only runway holds it through the claim pass, with no event and no OccupancyRevision bump. A flight holds
	// meanwhile. When the crossing clears, Airside's diff fires OnRunwayFreed, the runtime bridges it, and the flight
	// is cleared within a frame - with NO safety-pass Warning, which is how this fails if any link is missing: the net
	// would still land it, 30 game s later, and say so.
	FSafetyWarningSpy Spy;
	FRig Rig;
	FTestAirport Field;
	FGuidelineNodeId S, N;
	const bool bAttached = Rig.Attach([&](URoadNetwork& Net)
	{
		Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net);
		// A TAXIWAY ACROSS THE STRIP past the exit, FCrossingFixture's shape: a bar at H, a node on the centreline.
		const double Needed = FLandingRun::RequiredLandingDistance(
			Rig.Airframe.Chassis.Ground, Rig.Airframe.Climb, Rig.Airframe.Approach) * FLandingRun::LandingMargin;
		const double X = Needed * 2.5;
		S = TestGraph::Node(Net, X, -20000.0);
		const FGuidelineNodeId H = TestGraph::Node(Net, X, -3000.0);
		const FGuidelineNodeId OnStrip = TestGraph::Node(Net, X, 0.0);
		N = TestGraph::Node(Net, X, 20000.0);
		TestGraph::Join(Net, S, H);
		TestGraph::Join(Net, H, OnStrip);
		TestGraph::Join(Net, OnStrip, N);
		Net.SetRunwayHoldingPositionForTest(H, Field.ThresholdSegment);
	});
	if (!TestTrue(TEXT("an attached runtime over a runway with a crossing"), bAttached)) { return false; }
	auto Held = [&]() { return ArrivalPlanner::IsChainHeld(*Rig.Net, Field.ThresholdSegment, &Rig.Model->GetOccupancy()); };

	const FRoutePlan Across = TestGraph::Probe(*Rig.Net, S, N, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("a route across the strip"), Across.IsValid())) { return false; }
	if (!TestTrue(TEXT("an aircraft taxis across"),
		Rig.Model->DispatchAgent(Rig.Net, Across, Rig.Airframe, ETraversalClass::Aircraft, 1.0) > 0)) { return false; }

	auto Step = [&]()
	{
		Rig.Model->Advance(ArrivalQueuePassTest::Frame, Rig.Net);
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	};
	int32 Frames = 0;
	for (; Frames < 30 * 120 && !Held(); ++Frames) { Step(); }
	if (!TestTrue(TEXT("the crossing takes the strip"), Held())) { return false; }

	UFlight* Flight = Rig.Accept(Field.Threshold);
	if (!TestNotNull(TEXT("a flight accepted onto the stand"), Flight)) { return false; }
	for (int32 Tick = 0; Tick < 3 && Held(); ++Tick) { Step(); }
	if (!TestTrue(TEXT("the crossing still holds the strip"), Held())) { return false; }
	TestEqual(TEXT("so the flight holds"), Flight->GetPhase(), EFlightPhase::Inbound);

	int32 ReleasedAt = INDEX_NONE;
	int32 ClearedAt = INDEX_NONE;
	for (int32 Tick = 0; Tick < 30 * 120 && ClearedAt == INDEX_NONE; ++Tick)
	{
		Rig.Model->Advance(ArrivalQueuePassTest::Frame, Rig.Net);
		if (ReleasedAt == INDEX_NONE && !Held())
		{
			ReleasedAt = Tick;
		}
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
		if (Flight->GetPhase() != EFlightPhase::Inbound)
		{
			ClearedAt = Tick;
		}
	}
	if (!TestTrue(TEXT("the crossing cleared"), ReleasedAt != INDEX_NONE)) { return false; }
	if (!TestTrue(TEXT("and the flight was cleared to land"), ClearedAt != INDEX_NONE)) { return false; }
	TestEqual(TEXT("landing"), Flight->GetPhase(), EFlightPhase::Landing);
	// WITHIN ONE FRAME: here the model advances before the runtime ticks, so the same frame; in the other order
	// the drain that hears the event is the next frame's.
	TestTrue(FString::Printf(TEXT("cleared within a frame of the crossing clearing (released %d, cleared %d)"), ReleasedAt, ClearedAt),
		ClearedAt - ReleasedAt <= 1);
	TestEqual(TEXT("and by the event, not the safety net: no safety Warning"), Spy.Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueSafetyNetTest, "AirportOps.Present.ArrivalQueue.SafetyNetCatchesAMissedEvent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueSafetyNetTest::RunTest(const FString&)
{
	// A RUNWAY FREED WITH NO EVENT - its claims dropped straight out of the table, and the model never advanced, so
	// Airside's diff never ran. Nothing dirties the pass; the net must land the flight anyway, and say it had to.
	FSafetyWarningSpy Spy;
	FRig Rig;
	FTestAirport Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	ArrivalQueuePassTest::HoldStrip(*Rig.Model, *Rig.Net, Field.ThresholdSegment, 99);
	UFlight* Flight = Rig.Accept(Field.Threshold);
	if (!TestNotNull(TEXT("accepted"), Flight)) { return false; }
	for (int32 Tick = 0; Tick < 10; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestEqual(TEXT("the runway is held: the flight holds"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestTrue(TEXT("and the net is armed while it does"), Rig.Runtime->IsSafetyNetArmedForTest());

	Rig.Model->OccupancyForTest().ReleaseAll(99);
	int32 Frames = 0;
	for (; Frames < 30 * 45 && Flight->GetPhase() == EFlightPhase::Inbound; ++Frames) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestEqual(TEXT("the net landed it"), Flight->GetPhase(), EFlightPhase::Landing);
	TestTrue(FString::Printf(TEXT("within one net interval (%d frames)"), Frames),
		Frames <= FMath::CeilToInt(UOpsRuntime::SafetyNetSeconds / ArrivalQueuePassTest::Frame) + 2);
	if (TestEqual(TEXT("and said so, once"), Spy.Lines.Num(), 1))
	{
		TestTrue(TEXT("naming the flight"), Spy.Lines[0].Contains(FString::Printf(TEXT("flight %d"), Flight->Id)));
	}
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestFalse(TEXT("an empty queue disarms it"), Rig.Runtime->IsSafetyNetArmedForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueTwoRunwaysTest, "AirportOps.Present.ArrivalQueue.SecondRunwayNextFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueTwoRunwaysTest::RunTest(const FString&)
{
	// ONE CLEARANCE A FRAME, across the drain's rounds: two flights due together on a two-runway field. The first is
	// cleared; its Arriving event re-dirties the pass in the same drain, which defers to the next frame - so the
	// second lands on the other runway one frame later, with the first one's claim already in the table.
	FSafetyWarningSpy Spy;
	FRig Rig;
	FTestTwoRunways Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestTwoRunways::Build(Rig.Airframe, &Net); }))) { return false; }
	const FVector2D Focus(-1000.0, 0.0);
	UFlight* First = Rig.Accept(Focus);
	UFlight* Second = Rig.Accept(Focus);
	if (!TestTrue(TEXT("both accepted, one stand each"), First != nullptr && Second != nullptr)) { return false; }

	TArray<int32> ClearedAt;
	for (int32 Tick = 0; Tick < 30 && ClearedAt.Num() < 2; ++Tick)
	{
		const int32 Before = (First->GetPhase() == EFlightPhase::Landing) + (Second->GetPhase() == EFlightPhase::Landing);
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
		const int32 After = (First->GetPhase() == EFlightPhase::Landing) + (Second->GetPhase() == EFlightPhase::Landing);
		for (int32 Each = Before; Each < After; ++Each) { ClearedAt.Add(Tick); }
	}
	if (!TestEqual(TEXT("both cleared"), ClearedAt.Num(), 2)) { return false; }
	TestEqual(TEXT("the second one frame after the first - never in the same frame"), ClearedAt[1] - ClearedAt[0], 1);
	const FRoadAgent* A = Rig.Model->FindAgent(First->AgentId);
	const FRoadAgent* B = Rig.Model->FindAgent(Second->AgentId);
	TestTrue(TEXT("on different runways"), A != nullptr && B != nullptr && A->RunwayHeld.Num() > 0 && B->RunwayHeld.Num() > 0
		&& A->RunwayHeld[0] != B->RunwayHeld[0]);
	TestEqual(TEXT("with no safety Warning"), Spy.Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueEditKeepsHoldTest, "AirportOps.Present.RuntimeEdit.KeepsAcceptedStandHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueEditKeepsHoldTest::RunTest(const FString&)
{
	// REVIEW I1: any edit in play rebuilt the guideline graph, which released every Node claim - the accepted flights'
	// stand holds with the agents' - and nothing but a load re-held them. So the next offer was accepted onto a stand a
	// flight was already promised: one stand, two flights.
	FRig Rig;
	FTestAirport Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	UFlight* First = Rig.Accept(Field.Threshold, 3600.0);
	if (!TestNotNull(TEXT("the first flight is accepted onto the only stand"), First)) { return false; }
	const FGuidelineNodeId Pose = Field.Pose(Field.Stands[0]);
	TestEqual(TEXT("held under its own holder"), Rig.Model->HolderOfNode(Pose), First->HolderId());

	// A TOPOLOGY EDIT THROUGH THE ACTOR - a road drawn well away from the field - and the rebuild it runs.
	ARoadNetworkActor* Actor = Rig.World.Actor;
	const int32 A = Actor->PlaceNode(FVector2D(-60000.0, 60000.0));
	const int32 B = Actor->PlaceNode(FVector2D(-30000.0, 60000.0));
	TestTrue(TEXT("a road drawn"), Actor->ConnectNodes(A, B));
	Actor->RebuildMesh();
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		Rig.Model->Advance(ArrivalQueuePassTest::Frame, Rig.Net);
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	}

	TestEqual(TEXT("after the edit the stand is still held, by the same flight"), Rig.Model->HolderOfNode(Pose), First->HolderId());
	TestNull(TEXT("so a second offer is refused, not handed the promised stand"), Rig.Accept(Field.Threshold, 3600.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueEditRefusedReholdTest, "AirportOps.Present.RuntimeEdit.RefusedReholdAgreesWithTheTable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueEditRefusedReholdTest::RunTest(const FString&)
{
	// #442 ITEM 3's PIN. An edit's rebuild re-makes every stand hold inside Airside, after the agents' goals; one it CANNOT
	// re-make - another holder has the stand - was only warned, and UFlight::Stand went on naming a stand the flight no
	// longer held: no HeldStandLost (the stand lives), no re-hold (the flight "has" one), and the next accept was free to
	// take the last stand it was owed. FORCED HERE in the state the rebuild's own comment names: an agent's goal and a
	// flight's hold on one stand going into the rebuild. An aircraft is sent to the stand the accepted flight holds (its
	// goal claim outranks the hold and takes it at dispatch), and the hold is put back over it by hand - so the rebuild
	// snapshots the flight's hold, re-claims the agent's goal first, and refuses the flight. NO RUNTIME TICK between: the
	// reconcile would settle the dispatch's own preemption first, which is a different door. PAUSED, as the player edits,
	// and the flight only ACCEPTED: neither may wait for the queue.
	FRig Rig;
	FTestAirport Field;
	FTestAirportOptions Options;
	Options.StandCount = 2;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, Options, &Net); }))) { return false; }
	UFlight* Flight = Rig.Accept(Field.Threshold, 3600.0);
	if (!TestNotNull(TEXT("a flight accepted"), Flight)) { return false; }
	const FEntityInstanceId Promised = Flight->Stand;
	if (!TestTrue(TEXT("onto one of the two stands"), Promised == Field.Stands[0] || Promised == Field.Stands[1])) { return false; }
	const FEntityInstanceId Other = Promised == Field.Stands[0] ? Field.Stands[1] : Field.Stands[0];

	const FGuidelineNodeId Exit = RouteSearch::FindNearestNode(*Rig.Net, Field.ExitAt, ETraversalClass::Aircraft, 200.0);
	const FRoutePlan ToPromised = TestGraph::Probe(*Rig.Net, Exit, Field.Pose(Promised), ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("a route to the promised stand"), ToPromised.IsValid())) { return false; }
	Rig.Runtime->TogglePause();
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	if (!TestTrue(TEXT("PRECONDITION: paused"), Rig.Runtime->GetClock()->IsPaused())) { return false; }
	if (!TestTrue(TEXT("an aircraft sent to it"), Rig.Model->DispatchAgent(Rig.Net, ToPromised, Rig.Airframe, ETraversalClass::Aircraft, 1.0) > 0)) { return false; }
	Rig.Model->OccupancyForTest().Assert(FTrafficClaim::Make(Flight->HolderId(), FTrafficResource::OfNode(Field.Pose(Promised)), /*bOccupied*/ false, 99));
	if (!TestEqual(TEXT("PRECONDITION: going into the edit, the table holds the stand for the flight"),
		Rig.Model->HolderOfNode(Field.Pose(Promised)), Flight->HolderId())) { return false; }

	// A TOPOLOGY EDIT THROUGH THE ACTOR - a road well away from the field, as KeepsAcceptedStandHold draws - and the rebuild it runs.
	AddExpectedMessagePlain(TEXT("could not be re-made after the rebuild"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	ARoadNetworkActor* Actor = Rig.World.Actor;
	const int32 A = Actor->PlaceNode(FVector2D(-60000.0, 60000.0));
	const int32 B = Actor->PlaceNode(FVector2D(-30000.0, 60000.0));
	TestTrue(TEXT("a road drawn"), Actor->ConnectNodes(A, B));
	Actor->RebuildMesh();
	if (!TestNotEqual(TEXT("PRECONDITION: the rebuild refused the flight its stand - the agent's goal has it"),
		Rig.Model->HolderOfNode(Field.Pose(Promised)), Flight->HolderId())) { return false; }

	// THE EDIT'S ANNOUNCEMENT, DRAINED: the queue pass runs, paused or not.
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	TestTrue(TEXT("the flight no longer names the stand it lost"), Flight->Stand != Promised);
	TestEqual(TEXT("it is re-held at once, on the free stand"), Flight->Stand, Other);
	TestEqual(TEXT("which the table holds for it - the copy agrees with the record"), Rig.Model->HolderOfNode(Field.Pose(Other)), Flight->HolderId());
	TestFalse(TEXT("so the board's own test finds nothing lost"), UStandAllocator::HoldIsLost(*Flight, *Rig.Model, *Rig.Net));
	int32 HeldByFlight = 0;
	for (const FTrafficClaim& Claim : Rig.Model->GetOccupancy().GetClaims())
	{
		HeldByFlight += Claim.AgentId == Flight->HolderId() ? 1 : 0;
	}
	TestEqual(TEXT("and it holds that one stand and no other"), HeldByFlight, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueDispatchRefusalSentenceTest, "AirportOps.Present.Bus.DispatchRefusalCarriesThePlanSentence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueDispatchRefusalSentenceTest::RunTest(const FString&)
{
	// #471: AIRSIDE'S DISPATCH REFUSAL REACHES THE UI WITH THE PLAN'S SENTENCE, not the reason alone - through all three
	// layers (UGroundTraffic, UAirsideTraffic's relay, the runtime's bridge onto the bus) to UOpsEvents. A wingspan the
	// strip does not admit is NotAdmitted, whose reason-only wording ("not admitted to that runway") cannot carry the figures.
	FRig Rig;
	FTestAirport Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	UOpsEventsTestListener* Listener = NewObject<UOpsEventsTestListener>();
	Rig.Runtime->GetEvents()->OnArrivalRefused.AddDynamic(Listener, &UOpsEventsTestListener::OnRefused);
	FAirframe Wide = Rig.Airframe;
	Wide.Wingspan = 9000.0;
	const FArrivalPlan Plan = ArrivalPlanner::Plan(*Rig.Net, Field.Threshold, Wide, &Rig.Model->GetOccupancy());
	if (!TestEqual(TEXT("PRECONDITION: the plan refuses it as not admitted"), Plan.Why, EArrivalRefusal::NotAdmitted)) { return false; }

	// NO EXPECTED-MESSAGE FILTER for Airside's Warning (a Warning fails nothing): any pattern that matches the plan's sentence
	// also matches this test's own assertion text, which quotes it - and suppressed that failure when the relay was cut.
	TestEqual(TEXT("Airside refuses the dispatch"), Rig.Model->DispatchArrival(*Rig.Net, Field.Threshold, Wide, 1.0), 0);
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	TestEqual(TEXT("the refusal reaches the UI's face of the bus"), Listener->CountOf(TEXT("refused:")), 1);
	TestEqual(TEXT("with the plan's own sentence"), Listener->LastRefusedSentence, ArrivalPlanner::DescribeRefusal(Plan));
	TestNotEqual(TEXT("which is not the reason-only wording"), Listener->LastRefusedSentence, ArrivalPlanner::DescribeRefusal(Plan.Why, Wide.Wingspan));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueRetireTest, "AirportOps.Present.ArrivalQueue.RetireFreesWithoutAdvance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueRetireTest::RunTest(const FString&)
{
	// REVIEW M1: a runway released OUTSIDE Advance - the player's Unstick retiring the aircraft on it - waited for the
	// next Advance's diff, so with none (paused motion, or simply this frame's order) the safety net landed the flight
	// and warned "no event covered it", falsely. RetireAgent diffs at once now.
	FSafetyWarningSpy Spy;
	FRig Rig;
	FTestAirport Field;
	FTestAirportOptions Options;
	Options.StandCount = 2;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, Options, &Net); }))) { return false; }
	const int32 Blocker = Rig.Model->DispatchArrival(*Rig.Net, Field.Threshold, Rig.Airframe, 1.0);
	if (!TestTrue(TEXT("an arrival holds the runway"), Blocker > 0)) { return false; }
	Rig.Model->Advance(ArrivalQueuePassTest::Frame, Rig.Net);

	UFlight* Flight = Rig.Accept(Field.Threshold);
	if (!TestNotNull(TEXT("a flight accepted onto the other stand"), Flight)) { return false; }
	for (int32 Tick = 0; Tick < 10; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	if (!TestEqual(TEXT("the runway is held: it holds"), Flight->GetPhase(), EFlightPhase::Inbound)) { return false; }

	Rig.Model->RetireAgent(Blocker);
	for (int32 Tick = 0; Tick < 30 * 45 && Flight->GetPhase() == EFlightPhase::Inbound; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestEqual(TEXT("cleared to land"), Flight->GetPhase(), EFlightPhase::Landing);
	TestEqual(TEXT("by the retire's own event: no safety Warning"), Spy.Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueRetryCoveredTest, "AirportOps.Present.ArrivalQueue.RetryStaysCovered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueRetryCoveredTest::RunTest(const FString&)
{
	// REVIEW M1: a dispatch refused in a race is retried next frame - a run an event asked for. If the net fires on the
	// frame the retry finally succeeds, that run is still covered, and must not warn.
	FSafetyWarningSpy Spy;
	FRig Rig;
	FTestAirport Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	ArrivalQueuePassTest::HoldStrip(*Rig.Model, *Rig.Net, Field.ThresholdSegment, 99);
	UFlight* Flight = Rig.Accept(Field.Threshold);
	if (!TestNotNull(TEXT("accepted"), Flight)) { return false; }
	double Due = -1.0;
	for (int32 Tick = 0; Tick < 10 && Due < 0.0; ++Tick)
	{
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
		if (Rig.Runtime->IsSafetyNetArmedForTest())
		{
			Due = Rig.Runtime->GetClock()->Now() + UOpsRuntime::SafetyNetSeconds;
		}
	}
	if (!TestTrue(TEXT("the net is armed while it holds"), Due > 0.0)) { return false; }

	// THE RACE, staged: every dispatch refused until the net's first firing, then the real one.
	UFlightBoard* Board = Rig.Runtime->GetFlightBoard();
	const TFunction<bool(const FVector2D&, const FAirframe&)> Real = Board->Dispatcher;
	USimClock* Clock = Rig.Runtime->GetClock();
	Board->Dispatcher = [Real, Clock, Due](const FVector2D& Near, const FAirframe& Frame)
	{
		return Clock->Now() >= Due && Real(Near, Frame);
	};
	Rig.Model->OccupancyForTest().ReleaseAll(99);
	Rig.Runtime->GetBus().Publish(FRunwayFreedEvent{});   // the event that covers it
	for (int32 Tick = 0; Tick < 30 * 45 && Flight->GetPhase() == EFlightPhase::Inbound; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestEqual(TEXT("landed once the race cleared"), Flight->GetPhase(), EFlightPhase::Landing);
	TestEqual(TEXT("a retry of a covered run is covered: no safety Warning"), Spy.Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueCatchUpTest, "AirportOps.Present.ArrivalQueue.AttachAndLoadMarkItDirty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueCatchUpTest::RunTest(const FString&)
{
	// REVIEW M4: both catch-ups pinned where they happen - read straight after the call, before any Tick, since the
	// first Tick's NetworkChanged would run the pass anyway and hide either one missing.
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&Rig](URoadNetwork& Net) { FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	TestTrue(TEXT("an attach marks the queue pass dirty"), Rig.Runtime->GetBus().IsDirtyForTest(TEXT("ArrivalQueue")));
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestFalse(TEXT("settled"), Rig.Runtime->GetBus().IsDirtyForTest(TEXT("ArrivalQueue")));
	const FString Slot = TEXT("AirportOpsTest_QueueCatchUp");
	if (!TestTrue(TEXT("saved"), Rig.Runtime->SaveToSlot(Slot))) { return false; }
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	if (!TestTrue(TEXT("loaded"), Rig.Runtime->LoadFromSlot(Slot))) { return false; }
	TestTrue(TEXT("a load marks the queue pass dirty"), Rig.Runtime->GetBus().IsDirtyForTest(TEXT("ArrivalQueue")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueNetCancelTest, "AirportOps.Present.ArrivalQueue.NetCancelledOnLoadAndDetach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueNetCancelTest::RunTest(const FString&)
{
	// REVIEW M4: the net's clock entry is booked against the clock it was armed on - a load's clock is another, and a
	// detached runtime has no queue to guard.
	FRig Rig;
	FTestAirport Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	ArrivalQueuePassTest::HoldStrip(*Rig.Model, *Rig.Net, Field.ThresholdSegment, 99);
	if (!TestNotNull(TEXT("accepted"), Rig.Accept(Field.Threshold))) { return false; }
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	if (!TestTrue(TEXT("armed while it holds"), Rig.Runtime->IsSafetyNetArmedForTest())) { return false; }
	const FString Slot = TEXT("AirportOpsTest_QueueNetCancel");
	if (!TestTrue(TEXT("saved"), Rig.Runtime->SaveToSlot(Slot))) { return false; }
	if (!TestTrue(TEXT("loaded"), Rig.Runtime->LoadFromSlot(Slot))) { return false; }
	TestFalse(TEXT("a load cancels the net"), Rig.Runtime->IsSafetyNetArmedForTest());
	// THE LOAD CLEARED THE TABLE (ClearAgents), the fake holder with it; held again so the restored flight still waits.
	ArrivalQueuePassTest::HoldStrip(*Rig.Model, *Rig.Net, Field.ThresholdSegment, 99);
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestTrue(TEXT("and the load's pass re-arms it on the loaded clock"), Rig.Runtime->IsSafetyNetArmedForTest());
	Rig.Runtime->Attach(nullptr);
	TestFalse(TEXT("a detach cancels it"), Rig.Runtime->IsSafetyNetArmedForTest());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueClosedTest, "AirportOps.Present.ArrivalQueue.ClosedAirportDispatchesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueClosedTest::RunTest(const FString&)
{
	// A CLOSED AIRPORT ADMITS NO ARRIVALS - AT THE QUEUE TOO (whole-stack review I1). The entry doors (Accept, Land) were
	// gated; the pass that actually puts an aeroplane on the runway was not. A holding flight the closure did not cancel
	// - planted after it here, as a load could leave one - must hold, with no safety net ticking for it, and land the
	// moment the airport opens.
	FRig Rig;
	FTestAirport Field;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); }))) { return false; }
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	Rig.Runtime->SetAirportClosed(true);
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	if (!TestEqual(TEXT("closed"), Rig.Runtime->GetAirport()->Status(), EAirportStatus::ClosedByPlayer)) { return false; }

	UFlight* Holding = NewObject<UFlight>(GetTransientPackage());
	Holding->Airframe = Rig.Airframe;
	Holding->RunwayPreference = Field.Threshold;
	Holding->SetPhaseForTest(EFlightPhase::Inbound);
	Holding->HoldingSince = Rig.Runtime->GetClock()->Now();
	Rig.Runtime->GetFlightBoard()->AddOffer(*Rig.Runtime->GetClock(), Holding);
	// PAUSED AS WELL AS CLOSED (whole-stack re-review m6): a closed airport paused is still closed, and must not arm a
	// net either - the closed test comes before the pause test in TickQueue for this.
	Rig.Runtime->TogglePause();
	Rig.Runtime->GetBus().Publish(FFlightInboundEvent{ Holding->Id, Holding->AirlineId });
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestFalse(TEXT("paused and closed: no safety net either"), Rig.Runtime->IsSafetyNetArmedForTest());
	Rig.Runtime->TogglePause();

	const int32 RunsBefore = Rig.QueueRuns();
	Rig.Runtime->GetBus().Publish(FFlightInboundEvent{ Holding->Id, Holding->AirlineId });
	for (int32 Tick = 0; Tick < 60; ++Tick)
	{
		Rig.Model->Advance(ArrivalQueuePassTest::Frame, Rig.Net);
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	}
	TestTrue(TEXT("the pass ran - it was asked"), Rig.QueueRuns() > RunsBefore);
	TestEqual(TEXT("closed: the holding flight is not cleared to land"), Holding->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("and no aircraft was dispatched"), Rig.Model->GetAgentCount(), 0);
	TestFalse(TEXT("and no safety net ticks for a queue that cannot move"), Rig.Runtime->IsSafetyNetArmedForTest());

	Rig.Runtime->SetAirportClosed(false);
	for (int32 Tick = 0; Tick < 10 && Holding->GetPhase() == EFlightPhase::Inbound; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }
	TestEqual(TEXT("opened: it lands"), Holding->GetPhase(), EFlightPhase::Landing);
	return true;
}

namespace ArrivalQueuePassTest
{
	/** An offer for Field, accepted through the board's own door, or null if refused. Fee and lead the caller's. */
	UFlight* AcceptOnto(FRig& Rig, const FTestAirport& Field, double Lead, double LandingFee)
	{
		UFlightBoard* Board = Rig.Runtime->GetFlightBoard();
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Id = Board->TakeNextId();
		Flight->Airframe = Rig.Airframe;
		Flight->AirlineId = TEXT("Cumbria");
		Flight->OfferWindowSeconds = 60.0;
		Flight->OfferSecondsLeft = 60.0;
		Flight->LeadTimeSeconds = Lead;
		Flight->RunwayPreference = Field.Threshold;
		Flight->LandingFee = LandingFee;
		Board->AddOffer(*Rig.Runtime->GetClock(), Flight);
		return Board->Accept(*Rig.Model, *Rig.Net, *Rig.Runtime->GetClock(), *Flight) ? Flight : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueBillingWiredTest, "AirportOps.Present.Bus.BillingIsWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueBillingWiredTest::RunTest(const FString&)
{
	// #442 ITEM 4: BILLING IS ONE LINE IN UOpsRuntime::WireBus now - "Billing", Sim, on FFlightPhaseChangedEvent - and every model test
	// wires its own on a bare bus, so only a test through the runtime goes red without it. Asserted by name and by its effect: a flight
	// the "ArrivalQueue" pass lands is charged its landing fee BY THE SAME Tick - a round after the dispatch, in the drain that made it -
	// so whatever reads the ledger later in the frame reads it paid: the bar's balance (ULedger::Balance, gated on Revision) and a build
	// quote's affordability (ULedger::CanPay, the one rule CanAfford prices through).
	FTestAirport Field;
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); })))
	{
		return false;
	}
	TestTrue(TEXT("the runtime subscribes the billing reaction to every flight phase change"),
		Rig.Runtime->GetBus().SubscribersOf(FOpsEvent::IndexOfType<FFlightPhaseChangedEvent>()).Contains(FName(TEXT("Billing"))));
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }

	ULedger* Ledger = Rig.Runtime->GetLedger();
	UFlight* Flight = ArrivalQueuePassTest::AcceptOnto(Rig, Field, /*Lead*/ 0.0, /*LandingFee*/ 500.0);
	if (!TestNotNull(TEXT("accepted"), Flight) || !TestNotNull(TEXT("a ledger"), Ledger)) { return false; }
	const double Before = Ledger->Balance();
	const int32 RevisionBefore = Ledger->Revision();
	if (!TestFalse(TEXT("PRECONDITION: a quote priced at the balance plus the fee is not affordable yet"), Ledger->CanPay(Before + 500.0))) { return false; }
	for (int32 Tick = 0; Tick < 60 && Flight->GetPhase() != EFlightPhase::Landing; ++Tick)
	{
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	}
	if (!TestEqual(TEXT("PRECONDITION: the queue pass landed it"), Flight->GetPhase(), EFlightPhase::Landing)) { return false; }
	// NO MORE TICKS: what follows is what a reader later in the SAME frame sees.
	TestEqual(TEXT("the Tick that landed it charged its landing fee"), Ledger->Balance(), Before + 500.0, 1e-6);
	TestTrue(TEXT("and marked it paid"), Flight->bLandingFeePaid);
	TestTrue(TEXT("the ledger's revision moved, so the bar's balance re-reads this frame"), Ledger->Revision() != RevisionBefore);
	TestTrue(TEXT("and a quote priced at the new balance is affordable this frame"), Ledger->CanPay(Before + 500.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalQueueOwnersWiredTest, "AirportOps.Present.FlightBoardOwnersAreWired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalQueueOwnersWiredTest::RunTest(const FString&)
{
	// #442 ITEM 4 SPLIT UFlightBoard INTO THE REGISTRY AND TWO OWNERS, FOfferInbox and FArrivalQueue, keeping every public name as a
	// FORWARDER - and a forwarder that reached nothing would still compile. THROUGH THE RUNTIME, one flight's arrival from its offer:
	// the runtime's Tick drains the countdown (TickOffers -> FOfferInbox), the verdict reads admitted (VerdictFor), the accept holds a
	// stand and arms the arrival (TryAccept -> FOfferInbox; the Accepted row -> FArrivalQueue::Schedule), the ETA queues it (the clock
	// callback -> FArrivalQueue::Enqueue, seen through Queue), and the "ArrivalQueue" pass lands it (TickQueue -> FArrivalQueue::Tick,
	// DispatchNow). Each step names the forwarder whose owner must have done it.
	FTestAirport Field;
	FRig Rig;
	if (!TestTrue(TEXT("an attached runtime"), Rig.Attach([&](URoadNetwork& Net) { Field = FTestAirport::Build(Rig.Airframe, FTestAirportOptions(), &Net); })))
	{
		return false;
	}
	UFlightBoard* Board = Rig.Runtime->GetFlightBoard();
	USimClock* Clock = Rig.Runtime->GetClock();
	for (int32 Tick = 0; Tick < 5; ++Tick) { Rig.Runtime->Tick(ArrivalQueuePassTest::Frame); }

	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Id = Board->TakeNextId();
	Flight->Airframe = Rig.Airframe;
	Flight->OfferWindowSeconds = 60.0;
	Flight->OfferSecondsLeft = 60.0;
	Flight->LeadTimeSeconds = 2.0;
	Flight->RunwayPreference = Field.Threshold;
	Board->AddOffer(*Clock, Flight);
	Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
	TestTrue(TEXT("TickOffers: the runtime's Tick drained the offer's countdown"), Flight->OfferSecondsLeft < 60.0);
	TestTrue(TEXT("TickOffers: and judged it acceptable on the way (what a lapse is classified by)"), Flight->bWasEverAcceptable);
	TestEqual(TEXT("VerdictFor: admitted"), Board->VerdictFor(*Rig.Model, *Rig.Net, *Flight).Why, EArrivalRefusal::None);
	TestEqual(TEXT("QuoteFor: the same yes, through the gate"), Board->QuoteFor(*Rig.Model, *Rig.Net, *Flight).Why, EArrivalRefusal::None);

	const FArrivalQuote Accepted = Board->TryAccept(*Rig.Model, *Rig.Net, *Clock, *Flight);
	if (!TestTrue(FString::Printf(TEXT("TryAccept: accepted ('%s')"), *Accepted.Sentence), Accepted.IsAccepted())) { return false; }
	TestTrue(TEXT("TryAccept: onto the stand its plan taxis to, held"), Flight->Stand == Accepted.Stand && Accepted.Stand.IsSet());
	TestEqual(TEXT("Queue: nobody holds before the ETA"), Board->Queue().Num(), 0);

	// THE ETA, by the runtime's own clock - Schedule armed it - then the pass, on the FlightInbound it publishes.
	int32 HeldFrames = 0;
	for (int32 Tick = 0; Tick < 300 && Flight->GetPhase() != EFlightPhase::Landing; ++Tick)
	{
		Rig.Runtime->Tick(ArrivalQueuePassTest::Frame);
		HeldFrames += Board->Queue().Contains(Flight) ? 1 : 0;
	}
	TestEqual(TEXT("Schedule, Enqueue, TickQueue: armed, queued at its ETA and landed by the pass"), Flight->GetPhase(), EFlightPhase::Landing);
	TestTrue(TEXT("TickQueue: through the board's forwarder, the pass's one call"), Rig.QueueRuns() > 0);
	TestTrue(TEXT("DispatchNow: as the aircraft the dispatch made"), Flight->AgentId != INDEX_NONE && Rig.Model->FindAgent(Flight->AgentId) != nullptr);
	TestEqual(TEXT("Queue: and it left the queue"), Board->Queue().Num(), 0);
	AddInfo(FString::Printf(TEXT("frames it was seen holding: %d"), HeldFrames));
	return true;
}

#endif
