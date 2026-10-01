#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ScopeExit.h"
#include "Build/AnchorLink.h"
#include "Content/AirsideSettings.h"
#include "Entities/EntityDefinition.h"
#include "Model/DeparturePlanner.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/OpsEventBus.h"
#include "Model/RoadAgent.h"
#include "Model/RoadGuideline.h"
#include "Model/RoadNetwork.h"
#include "Model/RoutePolicy.h"
#include "Model/RouteSearch.h"
#include "Model/ServiceJob.h"
#include "Model/SimClock.h"
#include "Model/TrafficOccupancy.h"
#include "Present/AirsideTraffic.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Profiles/RoadProfile.h"
#include "Testing/AirsideTestGraph.h"
#include "Testing/AirsideTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A refused departure retried by event, not every frame (the follow-up to ops batch 3 PR D's item 6): Airside's derived
 * OnPushGroundFreed, bridged by UOpsRuntime::Attach onto FPushGroundFreedEvent, dirties the JobBoard pass; every other
 * refusal waits for NetworkChanged; the 30 s safety net runs while a due turnaround's departure is refused, and names a
 * departure no event covered. Each test fails if one seam is unwired.
 */
namespace PushGroundFreedPassTest
{
	constexpr float Frame = 1.0f / 30.0f;

	/** Every LogOpsBus Warning naming the safety pass. UNBUFFERED - ArrivalQueuePassTest's FSafetyWarningSpy's reason
	 *  (#216). Its own copy: that one lives in another file's namespace. */
	struct FPushSafetySpy : public FOutputDevice
	{
		TArray<FString> Lines;
		FPushSafetySpy() { GLog->AddOutputDevice(this); }
		virtual ~FPushSafetySpy() override { GLog->RemoveOutputDevice(this); }
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
	 * UnfuelledDepartureLowersAirline's field, stretched: a stand at the origin facing +X, its lead-in cast west onto a
	 * north-south taxiway at x = -10000, the runway far to the north - so an aircraft set off up the taxiway is a long
	 * way (and more than the safety net's 30 s) from parking, which would be a phase event of its own. No fuel depot:
	 * the fuel job is refused at once and the turnaround ends on its clock alone.
	 *
	 * bSouthArm: the taxiway starts at South (-10000, -20000), the arm a push reverses onto (the departure goes north).
	 * Without it the taxiway STARTS where the lead-in meets it, (-10000, 0) - a junction with no second arm, so the
	 * departure is refused NoPushbackRoute. 1 game s per real s (SetUniformDay): the net's 30 s is 900 frames.
	 */
	struct FPushRig
	{
		FAirsideTestWorld World;
		ARoadNetworkActor* Actor = nullptr;
		UOpsRuntime* Runtime = nullptr;
		URoadNetwork* Net = nullptr;
		UGroundTraffic* Model = nullptr;
		FGuidelineNodeId South;
		FGuidelineNodeId North;
		FEntityInstanceId Stand;
		int32 Aircraft = 0;
		FAirframe Airframe = UAirsideSettings::ResolveDefaultAirframe();

		static constexpr double NorthY = 200000.0;

		/** A guideline line From -> To, from an existing node when FromNode is set; returns the To node. */
		FGuidelineNodeId Lay(const FVector2D& From, const FVector2D& To, FGuidelineNodeId FromNode = FGuidelineNodeId(),
			FGuidelineNodeId* OutFrom = nullptr)
		{
			const FGuidelineNodeId A = FromNode.IsSet() ? FromNode : Net->AddGuidelineNode(From);
			const FGuidelineNodeId B = Net->AddGuidelineNode(To);
			FGuidelineEdge Edge;
			Edge.A = A;
			Edge.B = B;
			Edge.Control = (From + To) * 0.5;
			Edge.AllowedTraffic = FTrafficMask::Only(ETraversalClass::Aircraft);
			Edge.AllowedTraffic.Add(ETraversalClass::Emergency);
			Edge.Direction = EGuidelineDir::Bidirectional;
			Edge.Width = 600.0;
			Edge.bDerived = true;
			Net->AddGuidelineEdge(MoveTemp(Edge));
			if (OutFrom != nullptr)
			{
				*OutFrom = A;
			}
			return B;
		}

		/** The field, an attached runtime, and one aircraft dispatched from South to the stand. */
		bool Build(bool bSouthArm, double TurnaroundSeconds)
		{
			Actor = World.Actor;
			if (Actor == nullptr)
			{
				return false;
			}
			// A NETWORK, which a fresh actor lacks until its first edit. Far from the field.
			Actor->PlaceNode(FVector2D(200000.0, 0.0));
			Net = Actor->Network;
			if (Net == nullptr)
			{
				return false;
			}
			North = Lay(FVector2D(-10000.0, bSouthArm ? -20000.0 : 0.0), FVector2D(-10000.0, NorthY), FGuidelineNodeId(), &South);

			URoadProfile* Strip = TestProfiles::Runway();
			const FRoadNodeId West = Net->AddNode(FVector2D(-50000.0, NorthY + 10000.0));
			const FRoadNodeId Mid = Net->AddNode(FVector2D(-10000.0, NorthY + 10000.0));
			const FRoadNodeId East = Net->AddNode(FVector2D(50000.0, NorthY + 10000.0));
			Net->AddStraightSegment(West, Mid, Strip);
			Net->AddStraightSegment(Mid, East, Strip);
			Lay(FVector2D(-10000.0, NorthY), FVector2D(-10000.0, NorthY + 10000.0), North);

			UEntityDefinition* StandDef = UEntityDefinition::MakeStandTransient();
			Stand = Net->PlaceEntity(StandDef, StandDef->Anchors, FVector2D(0.0, 0.0), 0.0, 3600.0, StandDef->PoseRole, StandDef->Trucks);
			// THE HAND-LAID LINES ARE THE GRAPH - UnfuelledDepartureLowersAirline's reason: the runway segments would
			// otherwise read as a road the graph is behind, and DepartAgent refuses GraphBeingEdited for ever.
			// THE DERIVATION'S TAIL (#438) links the stand and stamps the graph; was FAnchorLink::Build + MarkGuidelinesDerived.
			TestGraph::Link(*Net);

			Runtime = NewObject<UOpsRuntime>();
			Runtime->Attach(Actor);
			Runtime->GetClock()->SetUniformDay(USimClock::SecondsPerDay);
			Model = Actor->GetTraffic()->GetModel();
			if (Model == nullptr || Net->GetEntity(Stand) == nullptr)
			{
				return false;
			}
			const FRoutePlan Plan = TestGraph::Probe(*Net, South, Net->GetEntity(Stand)->PoseNode, ETraversalClass::Aircraft);
			Airframe.TurnaroundSeconds = TurnaroundSeconds;
			if (!Plan.IsValid() || !Actor->DispatchAgent(Plan, Airframe))
			{
				return false;
			}
			Aircraft = Actor->GetTraffic()->GetNewestAgentId();
			return true;
		}

		/**
		 * The taxiway node just south of the lead-in's junction - FAnchorLink's back tangent node, where the push's
		 * sweep meets the arm. On the push ground whatever the push's exact length: the push reverses THROUGH it.
		 * Found by position, since the anchor link makes it.
		 */
		FGuidelineNodeId PushArmNode() const
		{
			FGuidelineNodeId Best;
			double BestY = -TNumericLimits<double>::Max();
			const TArray<FGuidelineNode>& Nodes = Net->GetGuidelineNodes();
			for (int32 Index = 0; Index < Nodes.Num(); ++Index)
			{
				const FGuidelineNodeId Id = Net->GuidelineNodeIdAt(Index);
				const FVector2D At = Nodes[Index].Position;
				if (Id.IsSet() && Id != South && FMath::Abs(At.X + 10000.0) < 1.0 && At.Y < -1.0 && At.Y > BestY)
				{
					Best = Id;
					BestY = At.Y;
				}
			}
			return Best;
		}

		void Tick()
		{
			Actor->Tick(Frame);
			Runtime->Tick(Frame);
		}

		const FRoadAgent* Agent() const { return Model->FindAgent(Aircraft); }
		bool IsParked() const { return Agent() != nullptr && Agent()->Phase == EAgentPhase::Parked; }
		const FTurnaround* Turnaround() const { return Runtime->GetJobBoard()->TurnaroundFor(Aircraft); }
		EDepartureRefusal Refusal() const { return Turnaround() != nullptr ? Turnaround()->LastDepartureRefusal : EDepartureRefusal::None; }

		/** Ticks until the aircraft is parked with a turnaround open; false if it never is. */
		bool ParkAndOpen()
		{
			for (int32 Tick = 0; Tick < 30 * 300 && !(IsParked() && Turnaround() != nullptr); ++Tick)
			{
				this->Tick();
			}
			return IsParked() && Turnaround() != nullptr;
		}

		/** Ticks until the turnaround's departure has been refused Why; false if it never is. */
		bool TickUntilRefused(EDepartureRefusal Why)
		{
			for (int32 Tick = 0; Tick < 30 * 60 && Refusal() != Why; ++Tick)
			{
				this->Tick();
			}
			return Refusal() == Why;
		}
	};
}

using PushGroundFreedPassTest::FPushRig;
using PushGroundFreedPassTest::FPushSafetySpy;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedBridgedTest, "AirportOps.Present.Bus.PushGroundFreedIsBridged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedBridgedTest::RunTest(const FString&)
{
	FPushRig Rig;
	if (!TestTrue(TEXT("a field and an attached runtime"), Rig.Build(true, 5.0))) { return false; }
	FOpsEventBus& Bus = Rig.Runtime->GetBus();

	// THROUGH THE MODEL'S OWN DELEGATE: the broadcast is the delegate the bridge binds (#445 item 6).
	const int32 Before = Bus.QueuedCount();
	Rig.Model->OnPushGroundFreed.Broadcast(Rig.Aircraft);
	TestEqual(TEXT("a freed push ground is published onto the bus"), Bus.QueuedCount(), Before + 1);

	// AND UNBOUND BY A DETACH (Attach(nullptr) detaches first).
	Rig.Runtime->Attach(nullptr);
	const int32 Detached = Bus.QueuedCount();
	Rig.Model->OnPushGroundFreed.Broadcast(Rig.Aircraft);
	TestEqual(TEXT("detached: nothing more is published"), Bus.QueuedCount(), Detached);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedDepartsTest, "AirportOps.Present.PushGroundFreed.DepartsTheFrameAfter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedDepartsTest::RunTest(const FString&)
{
	// END TO END, NOTHING PUBLISHED BY HAND: the turnaround comes due while a second aircraft is taxiing up the push arm,
	// so the push is refused; the blocker drives on (the claim pass frees the ground - no event of its own); Airside's
	// watch says so, the bridge publishes it, the JobBoard pass runs, and the aircraft pushes back. With no safety
	// Warning: an event, not the net, got it away.
	FPushSafetySpy Spy;
	FPushRig Rig;
	// ONE GAME SECOND ON STAND, so the turnaround is due while the blocker below is still on the push ground.
	if (!TestTrue(TEXT("a field and an attached runtime"), Rig.Build(true, 1.0))) { return false; }
	if (!TestTrue(TEXT("the aircraft parks and its turnaround opens"), Rig.ParkAndOpen())) { return false; }

	// THE BLOCKER, set off from the push arm's node, up the taxiway past the junction, a long way north.
	const FRoutePlan Up = TestGraph::Probe(*Rig.Net, Rig.PushArmNode(), Rig.North, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("a second aircraft sets off up the push arm"), Up.IsValid() && Rig.Actor->DispatchAgent(Up, Rig.Airframe))) { return false; }
	const int32 Blocker = Rig.Actor->GetTraffic()->GetNewestAgentId();

	int32 Frame = 0;
	int32 FreedAt = INDEX_NONE;
	int32 DepartedAt = INDEX_NONE;
	bool bRefusedBlocked = false;
	Rig.Model->OnPushGroundFreed.AddLambda([&Frame, &FreedAt](int32) { if (FreedAt == INDEX_NONE) { FreedAt = Frame; } });
	for (Frame = 1; Frame <= 30 * 120; ++Frame)
	{
		Rig.Tick();
		bRefusedBlocked |= Rig.Refusal() == EDepartureRefusal::PushbackBlocked;
		if (DepartedAt == INDEX_NONE && !Rig.IsParked())
		{
			DepartedAt = Frame;
		}
		if (DepartedAt != INDEX_NONE && Frame > DepartedAt + 30)
		{
			break;
		}
	}
	TestTrue(TEXT("its turnaround came due while the push was blocked: refused PushbackBlocked"), bRefusedBlocked);
	const FRoadAgent* Still = Rig.Model->FindAgent(Blocker);
	TestTrue(TEXT("the blocker is still taxiing - it parked nowhere, so its phase woke nobody"),
		Still != nullptr && Still->Phase == EAgentPhase::Taxiing);
	TestTrue(FString::Printf(TEXT("the push ground freed (frame %d)"), FreedAt), FreedAt != INDEX_NONE);
	TestTrue(FString::Printf(TEXT("and the aircraft pushed back within a frame of it (freed %d, departed %d)"), FreedAt, DepartedAt),
		DepartedAt != INDEX_NONE && FreedAt != INDEX_NONE && DepartedAt - FreedAt >= 0 && DepartedAt - FreedAt <= 1);
	TestEqual(TEXT("no safety Warning: an event covered it"), Spy.Lines.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedQuietTest, "AirportOps.Present.PushGroundFreed.NoPushbackRouteIsQuiet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedQuietTest::RunTest(const FString&)
{
	// A REFUSAL NOTHING BUT THE PLAYER CAN CHANGE costs the job board nothing: it used to re-run the whole Step every
	// frame (bDepartureWaiting) for as long as the stand stayed a dead end.
	FPushRig Rig;
	if (!TestTrue(TEXT("a field with no push arm"), Rig.Build(false, 5.0))) { return false; }
	if (!TestTrue(TEXT("the aircraft parks and its turnaround opens"), Rig.ParkAndOpen())) { return false; }
	if (!TestTrue(TEXT("and at its deadline it is refused NoPushbackRoute"), Rig.TickUntilRefused(EDepartureRefusal::NoPushbackRoute))) { return false; }

	const int32 Settled = Rig.Runtime->GetJobBoard()->StepCountForTest();
	for (int32 Tick = 0; Tick < 150; ++Tick)
	{
		Rig.Tick();
	}
	TestEqual(TEXT("five quiet seconds run no job board step"), Rig.Runtime->GetJobBoard()->StepCountForTest(), Settled);
	TestTrue(TEXT("and it is still parked"), Rig.IsParked());

	// THE PLAYER DRAWS THE MISSING ARM - a graph edit, NetworkChanged, which dirties the pass: it goes.
	// HAND-LAID, so ANNOUNCED BY HAND the way the actor announces every rebuild (#446: ARoadNetworkActor::OnNetworkChanged,
	// which ops bridges to FNetworkChangedEvent - no longer a per-frame compare of the graph's revision). A real rebuild
	// would re-derive from the runway and sweep this rig's hand-laid lines.
	Rig.Lay(FVector2D(-10000.0, 0.0), FVector2D(-10000.0, -20000.0), Rig.South);
	Rig.Actor->OnNetworkChanged.Broadcast(EChangeKind::Topology, *Rig.Net);
	int32 Frames = 0;
	for (; Frames < 60 && Rig.IsParked(); ++Frames)
	{
		Rig.Tick();
	}
	TestFalse(FString::Printf(TEXT("drawing the arm lets it push back (%d frames)"), Frames), Rig.IsParked());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedSafetyNetTest, "AirportOps.Present.PushGroundFreed.SafetyNetDepartsAMissedOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedSafetyNetTest::RunTest(const FString&)
{
	// A FREED GROUND NO EVENT SAYS: a hold on the push arm dropped straight out of the table with the traffic model not
	// advanced, so no diff runs. The net finds it, departs it, and names the gap.
	FPushSafetySpy Spy;
	FPushRig Rig;
	if (!TestTrue(TEXT("a field and an attached runtime"), Rig.Build(true, 5.0))) { return false; }
	if (!TestTrue(TEXT("the aircraft parks and its turnaround opens"), Rig.ParkAndOpen())) { return false; }
	if (!TestTrue(TEXT("a hold on the push arm"), Rig.Model->HoldStand(-7, Rig.PushArmNode()))) { return false; }
	if (!TestTrue(TEXT("refused PushbackBlocked"), Rig.TickUntilRefused(EDepartureRefusal::PushbackBlocked))) { return false; }

	Rig.Model->OccupancyForTest().ReleaseAll(-7);
	int32 Frames = 0;
	for (; Frames < 30 * 40 && Rig.IsParked(); ++Frames)
	{
		Rig.Runtime->Tick(PushGroundFreedPassTest::Frame);
	}
	TestFalse(FString::Printf(TEXT("the net got it away (%d frames)"), Frames), Rig.IsParked());
	TestTrue(TEXT("no sooner than the net's 30 s"), Frames >= 30 * 29);
	if (TestEqual(TEXT("and said so, once"), Spy.Lines.Num(), 1))
	{
		TestTrue(TEXT("naming the aircraft"), Spy.Lines[0].Contains(FString::Printf(TEXT("departed aircraft %d"), Rig.Aircraft)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedNetTest, "AirportOps.Present.PushGroundFreed.NetArmedAndCancelled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedNetTest::RunTest(const FString&)
{
	{
		FPushRig Rig;
		if (!TestTrue(TEXT("a field and an attached runtime"), Rig.Build(true, 5.0))) { return false; }
		if (!TestTrue(TEXT("parked"), Rig.ParkAndOpen())) { return false; }
		TestFalse(TEXT("nothing refused yet: no net"), Rig.Runtime->IsSafetyNetArmedForTest());
		if (!TestTrue(TEXT("a hold on the push arm"), Rig.Model->HoldStand(-7, Rig.PushArmNode()))) { return false; }
		if (!TestTrue(TEXT("refused PushbackBlocked"), Rig.TickUntilRefused(EDepartureRefusal::PushbackBlocked))) { return false; }
		TestTrue(TEXT("a refused departure arms the net"), Rig.Runtime->IsSafetyNetArmedForTest());
		Rig.Runtime->Attach(nullptr);
		TestFalse(TEXT("a detach cancels it"), Rig.Runtime->IsSafetyNetArmedForTest());
	}
	{
		FPushRig Rig;
		if (!TestTrue(TEXT("a second field"), Rig.Build(true, 5.0))) { return false; }
		if (!TestTrue(TEXT("parked"), Rig.ParkAndOpen())) { return false; }
		if (!TestTrue(TEXT("a hold on the push arm"), Rig.Model->HoldStand(-7, Rig.PushArmNode()))) { return false; }
		if (!TestTrue(TEXT("refused PushbackBlocked"), Rig.TickUntilRefused(EDepartureRefusal::PushbackBlocked))) { return false; }
		const FString Slot = TEXT("AirportOpsTest_PushNetCancel");
		ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot, 0); };
		if (!TestTrue(TEXT("saved"), Rig.Runtime->SaveToSlot(Slot))) { return false; }
		if (!TestTrue(TEXT("loaded"), Rig.Runtime->LoadFromSlot(Slot))) { return false; }
		TestFalse(TEXT("a load cancels the net"), Rig.Runtime->IsSafetyNetArmedForTest());
		for (int32 Tick = 0; Tick < 5; ++Tick)
		{
			Rig.Tick();
		}
		// AGENTS ARE NOT SAVED: the loaded board has no aircraft whose departure waits.
		TestFalse(TEXT("and the load's catch-up does not re-arm it for an aircraft that is gone"), Rig.Runtime->IsSafetyNetArmedForTest());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPushGroundFreedPausedEditTest, "AirportOps.Present.PushGroundFreed.PausedEditDepartsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FPushGroundFreedPausedEditTest::RunTest(const FString&)
{
	// REVIEW I1, END TO END: the player pauses with a pushback refused for a taxiing aircraft on its ground, and edits. The
	// rebuild used to drop the taxiing body's claims until the next Advance - which a paused game does not reach - while the
	// edit's NetworkChanged ran the job board every paused frame: DepartAgent read the ground free and pushed the aircraft
	// back into the one taxiing behind it.
	FPushRig Rig;
	if (!TestTrue(TEXT("a field and an attached runtime"), Rig.Build(true, 1.0))) { return false; }
	if (!TestTrue(TEXT("the aircraft parks and its turnaround opens"), Rig.ParkAndOpen())) { return false; }
	const FRoutePlan Up = TestGraph::Probe(*Rig.Net, Rig.PushArmNode(), Rig.North, ETraversalClass::Aircraft);
	if (!TestTrue(TEXT("a second aircraft sets off up the push arm"), Up.IsValid() && Rig.Actor->DispatchAgent(Up, Rig.Airframe))) { return false; }
	if (!TestTrue(TEXT("refused PushbackBlocked"), Rig.TickUntilRefused(EDepartureRefusal::PushbackBlocked))) { return false; }

	Rig.Runtime->TogglePause();
	Rig.Tick();
	if (!TestTrue(TEXT("paused"), Rig.Runtime->GetClock()->IsPaused())) { return false; }
	// THE EDIT: the traffic model's rebuild, and a graph change the runtime sees (a line laid far off) - NetworkChanged.
	Rig.Model->OnGraphRebuilt(*Rig.Net);
	Rig.Lay(FVector2D(150000.0, 50000.0), FVector2D(160000.0, 50000.0));
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Rig.Tick();
	}
	TestTrue(TEXT("paused through the edit, the aircraft has not pushed back into the blocker"), Rig.IsParked());
	TestEqual(TEXT("its departure is still refused for the ground"), Rig.Refusal(), EDepartureRefusal::PushbackBlocked);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
