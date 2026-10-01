#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/ArrivalPlanner.h"
#include "Model/ArrivalSequencer.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/OpsSave.h"
#include "Model/RoadEntity.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Model/TrafficOccupancy.h"
#include "Testing/AirsideTestGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The arrival queue (spec 2026-09-28-arrival-queue section 2): a flight due while the runway is
 * busy HOLDS - off-map, stand kept, contract ticking - and is cleared the first frame its runway
 * is free. Before this it was dispatched blind, refused, and lost with no stand.
 */
namespace
{
	FAirframe QueueAirframe()
	{
		FAirframe Out;
		Out.Wingspan = 1200.0;
		Out.TurnaroundSeconds = 1800.0;
		return Out;
	}

	struct FQueueRig
	{
		FTestAirport Airport;
		UGroundTraffic* Traffic = nullptr;
		USimClock* Clock = nullptr;
		UFlightBoard* Board = nullptr;
		int32 Dispatched = 0;
		bool bDispatcherAccepts = true;

		explicit FQueueRig(int32 Stands = 1)
		{
			FTestAirportOptions Options;
			Options.StandCount = Stands;
			Airport = FTestAirport::Build(QueueAirframe(), Options);
			Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
			Clock = NewObject<USimClock>(GetTransientPackage());
			Clock->SetUniformDay(USimClock::SecondsPerDay);   // 1 game s per real s
			Board = NewObject<UFlightBoard>(GetTransientPackage());
			Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
			Board->Sequencer = NewObject<UArrivalSequencer>(GetTransientPackage());
			Board->Dispatcher = [this](const FVector2D&, const FAirframe&) { ++Dispatched; return bDispatcherAccepts; };
		}

		UFlight* Accepted(double Lead = 10.0)
		{
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->Airframe = QueueAirframe();
			Flight->OfferWindowSeconds = 60.0;
			Flight->OfferSecondsLeft = 60.0;
			Flight->LeadTimeSeconds = Lead;
			Flight->RunwayPreference = Airport.Threshold;
			Board->AddOffer(*Clock, Flight);
			Board->Accept(*Traffic, *Airport.Net, *Clock, *Flight);
			return Flight;
		}

		void HoldRunway()
		{
			for (const FTrafficResource& Surface : Airport.Net->RunwaySurfaces(Airport.ThresholdSegment))
			{
				FTrafficClaim Claim;
				Claim.AgentId = 99;
				Claim.Resource = Surface;
				Claim.bOccupied = true;
				FTrafficClaim Blocker;
				Traffic->OccupancyForTest().TryClaim(Claim, Blocker);
			}
		}

		void FreeRunway() { Traffic->OccupancyForTest().ReleaseAll(99); }

		bool StandHeldFor(const UFlight& Flight) const
		{
			const FEntityInstance* Stand = Airport.Net->GetEntity(Flight.Stand);
			return Stand != nullptr && Traffic->IsStandHeld(Stand->PoseNode, 0);
		}

		void Tick() { Board->TickQueue(*Traffic, *Airport.Net, *Clock); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueAcceptWhileBusyTest, "AirportOps.Model.ArrivalQueue.AcceptWhileRunwayBusy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueAcceptWhileBusyTest::RunTest(const FString& Parameters)
{
	FQueueRig Rig;
	Rig.HoldRunway();
	UFlight* Offer = NewObject<UFlight>(GetTransientPackage());
	Offer->Airframe = QueueAirframe();
	Offer->OfferSecondsLeft = 60.0;
	Offer->RunwayPreference = Rig.Airport.Threshold;
	Rig.Board->AddOffer(*Rig.Clock, Offer);
	TestEqual(TEXT("a busy runway is no reason to grey out Accept"),
		Rig.Board->WhyNotAcceptable(*Rig.Traffic, *Rig.Airport.Net, *Offer), EArrivalRefusal::None);
	TestTrue(TEXT("and the accept goes through"), Rig.Board->Accept(*Rig.Traffic, *Rig.Airport.Net, *Rig.Clock, *Offer));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueDueWhileBusyTest, "AirportOps.Model.ArrivalQueue.DueWhileBusyWaitsThenLands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueDueWhileBusyTest::RunTest(const FString& Parameters)
{
	// THE LOST-FLIGHT BUG: due while the strip is held, it used to be dispatched, refused, and
	// left Accepted with no stand for ever.
	FQueueRig Rig;
	UFlight* Flight = Rig.Accepted(10.0);
	Rig.HoldRunway();
	Rig.Clock->Advance(11.0);
	TestEqual(TEXT("past its ETA it is holding, not lost"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("it joined the queue at its ETA"), Flight->HoldingSince, Flight->ArrivesAt, 1.0);
	TestTrue(TEXT("its stand is still held for it"), Rig.StandHeldFor(*Flight));
	Rig.Tick();
	TestEqual(TEXT("nothing is cleared onto a busy runway"), Rig.Dispatched, 0);
	TestEqual(TEXT("it is in the queue"), Rig.Board->Queue().Num(), 1);

	Rig.FreeRunway();
	Rig.Tick();
	TestEqual(TEXT("the frame the runway frees, it is cleared"), Rig.Dispatched, 1);
	TestEqual(TEXT("and lands"), Flight->GetPhase(), EFlightPhase::Landing);
	TestEqual(TEXT("leaving the queue"), Rig.Board->Queue().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueJoinOrderTest, "AirportOps.Model.ArrivalQueue.ClearsInJoinOrderOnePerFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueJoinOrderTest::RunTest(const FString& Parameters)
{
	FQueueRig Rig(/*Stands*/ 2);
	UFlight* First = Rig.Accepted(10.0);
	UFlight* Second = Rig.Accepted(20.0);
	Rig.HoldRunway();
	Rig.Clock->Advance(21.0);
	const TArray<UFlight*> Queue = Rig.Board->Queue();
	if (!TestEqual(TEXT("both are holding"), Queue.Num(), 2)) { return false; }
	TestTrue(TEXT("in the order they joined"), Queue[0] == First && Queue[1] == Second);
	Rig.FreeRunway();
	Rig.Tick();
	TestEqual(TEXT("one clearance a frame"), Rig.Dispatched, 1);
	TestEqual(TEXT("the first to join goes first"), First->GetPhase(), EFlightPhase::Landing);
	TestEqual(TEXT("the second still waits"), Second->GetPhase(), EFlightPhase::Inbound);
	Rig.Tick();
	TestEqual(TEXT("then the second"), Second->GetPhase(), EFlightPhase::Landing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueOnePerFrameRuleTest, "AirportOps.Model.FlightBoard.OneClearanceAFrameIsTheQueuesRule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueOnePerFrameRuleTest::RunTest(const FString& Parameters)
{
	// #445: ONE CLEARANCE A FRAME was the runtime's - a frame counter and a "cleared in frame N" on UOpsRuntime - though the rule is the queue's:
	// the aircraft just cleared claims the runway on its first motion tick, so a second clearance in the frame of the first is decided before
	// that claim exists. The board keeps it now (BeginQueueFrame), and says bDeferred to the second ask.
	FQueueRig Rig(/*Stands*/ 2);
	UFlight* First = Rig.Accepted(10.0);
	UFlight* Second = Rig.Accepted(20.0);
	Rig.HoldRunway();
	Rig.Clock->Advance(21.0);
	Rig.FreeRunway();
	if (!TestEqual(TEXT("PRECONDITION: both are holding"), Rig.Board->Queue().Num(), 2)) { return false; }

	Rig.Board->BeginQueueFrame();
	const FQueueTick Cleared = Rig.Board->TickQueue(*Rig.Traffic, *Rig.Airport.Net, *Rig.Clock);
	TestTrue(TEXT("the first ask of the frame clears the first to join"), Cleared.Cleared == First && !Cleared.bDeferred);
	const FQueueTick Again = Rig.Board->TickQueue(*Rig.Traffic, *Rig.Airport.Net, *Rig.Clock);
	TestTrue(TEXT("a second ask in the SAME frame is deferred, not decided"), Again.bDeferred && Again.Cleared == nullptr);
	TestEqual(TEXT("and the second flight still waits"), Second->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("with one dispatch in total"), Rig.Dispatched, 1);

	Rig.Board->BeginQueueFrame();
	const FQueueTick Next = Rig.Board->TickQueue(*Rig.Traffic, *Rig.Airport.Net, *Rig.Clock);
	TestTrue(TEXT("the next frame clears the second"), Next.Cleared == Second && !Next.bDeferred);

	// A BOARD NOBODY FRAMES sets its own pace (a world-free test driving TickQueue by hand), and is not refused for ever after one clearance.
	FQueueRig Loose(/*Stands*/ 2);
	UFlight* A = Loose.Accepted(10.0);
	UFlight* B = Loose.Accepted(20.0);
	Loose.HoldRunway();
	Loose.Clock->Advance(21.0);
	Loose.FreeRunway();
	const FQueueTick One = Loose.Board->TickQueue(*Loose.Traffic, *Loose.Airport.Net, *Loose.Clock);
	const FQueueTick Two = Loose.Board->TickQueue(*Loose.Traffic, *Loose.Airport.Net, *Loose.Clock);
	TestTrue(TEXT("CONTROL: with no frame begun, nothing is deferred"), One.Cleared == A && Two.Cleared == B && !Two.bDeferred);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueFreeRunwayJumpsTest, "AirportOps.Model.ArrivalQueue.FreeRunwayJumpsTheQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueFreeRunwayJumpsTest::RunTest(const FString& Parameters)
{
	// POLICY, tested without a world: a flight for a free runway is not held behind one waiting
	// on a busy runway.
	UArrivalSequencer* Sequencer = NewObject<UArrivalSequencer>(GetTransientPackage());
	UFlight* OnBusy = NewObject<UFlight>(GetTransientPackage());
	UFlight* OnFree = NewObject<UFlight>(GetTransientPackage());
	const TArray<UFlight*> Queue = { OnBusy, OnFree };
	UFlight* Next = Sequencer->Next(Queue, [OnBusy](const UFlight& F) { return &F != OnBusy; });
	TestTrue(TEXT("the flight that can land goes"), Next == OnFree);
	TestNull(TEXT("and with nobody able to land, nobody"),
		Sequencer->Next(Queue, [](const UFlight&) { return false; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueFailedDispatchTest, "AirportOps.Model.ArrivalQueue.FailedDispatchStaysQueued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueFailedDispatchTest::RunTest(const FString& Parameters)
{
	FQueueRig Rig;
	UFlight* Flight = Rig.Accepted(10.0);
	Rig.Clock->Advance(11.0);
	Rig.bDispatcherAccepts = false;
	Rig.Tick();
	TestEqual(TEXT("a refused clearance was attempted"), Rig.Dispatched, 1);
	TestEqual(TEXT("and the flight is still holding"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestTrue(TEXT("with its stand held again"), Rig.StandHeldFor(*Flight));
	Rig.bDispatcherAccepts = true;
	Rig.Tick();
	TestEqual(TEXT("retried, it lands"), Flight->GetPhase(), EFlightPhase::Landing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueuePausedTest, "AirportOps.Model.ArrivalQueue.PausedClearsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueuePausedTest::RunTest(const FString& Parameters)
{
	FQueueRig Rig;
	Rig.Accepted(10.0);
	Rig.Clock->Advance(11.0);
	Rig.Clock->TogglePause();
	Rig.Tick();
	TestEqual(TEXT("a paused game clears nobody"), Rig.Dispatched, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueSurvivesSaveTest, "AirportOps.Model.ArrivalQueue.HoldingSurvivesSave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueSurvivesSaveTest::RunTest(const FString& Parameters)
{
	FQueueRig Rig;
	UFlight* Flight = Rig.Accepted(10.0);
	Rig.HoldRunway();
	Rig.Clock->Advance(11.0);
	if (!TestEqual(TEXT("holding before the save"), Flight->GetPhase(), EFlightPhase::Inbound)) { return false; }

	TArray<uint8> Bytes;
	OpsSave::SerializeObject(*Rig.Board, Bytes);
	FQueueRig Loaded;
	OpsSave::DeserializeObject(*Loaded.Board, Bytes);
	Loaded.Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	Loaded.Board->Sequencer = NewObject<UArrivalSequencer>(GetTransientPackage());
	Loaded.Board->Dispatcher = [&Loaded](const FVector2D&, const FAirframe&) { ++Loaded.Dispatched; return true; };
	Loaded.Board->RestoreStandHolds(*Loaded.Traffic, *Loaded.Airport.Net);
	Loaded.Board->RearmSchedules(*Loaded.Traffic, *Loaded.Airport.Net, *Loaded.Clock);

	if (!TestEqual(TEXT("it comes back queued"), Loaded.Board->Queue().Num(), 1)) { return false; }
	TestTrue(TEXT("with its stand held again"), Loaded.StandHeldFor(*Loaded.Board->Queue()[0]));
	Loaded.Tick();
	TestEqual(TEXT("and is cleared on the loaded field's free runway"), Loaded.Dispatched, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueOverdueOnLoadTest, "AirportOps.Model.ArrivalQueue.OverdueOnLoadJoinsQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueOverdueOnLoadTest::RunTest(const FString& Parameters)
{
	// An Accepted flight whose ETA passed while the game was shut joins the queue - the queue,
	// not a blind dispatch, decides whether the runway is free. HELD, THEN FREE (merged with
	// AirportOps.Model.FlightSave.AFlightDueWhileTheGameWasShutIsNotLost, #462 M15 - both planted an overdue Accepted flight and called
	// RearmSchedules): the held half is "holding, not dispatched blind", the free half is "and cleared once the runway is free".
	//
	// The flight is put in the Accepted state DIRECTLY, because that is what a restore does - it deserialises phases and ETAs, and
	// arms nothing. Accepting it through the board here would arm the clock, the clock would fire on its own, and the test would
	// prove that the ordinary path works rather than that the load path does. Its slot passed while the game was closed: dropping it
	// silently is the failure this guards - the player accepted a flight and it simply never came.
	FQueueRig Rig;
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe = QueueAirframe();
	Flight->SetPhaseForTest(EFlightPhase::Accepted);
	Flight->ArrivesAt = -5.0;
	Flight->RunwayPreference = Rig.Airport.Threshold;
	Rig.Board->AddOffer(*Rig.Clock, Flight);
	Rig.HoldRunway();
	Rig.Clock->Advance(1.0);   // 1 game s: the ETA is already behind us
	TestEqual(TEXT("nothing has been dispatched, because nothing was armed"), Rig.Dispatched, 0);
	// THE PREMISE, ASKED OF THE FLIGHT: Dispatched == 0 holds with the runway held whether or not anything was armed, so it cannot fail. An
	// armed arrival fires on that Advance and joins the queue - Inbound - which is what a flight left Accepted proves did not happen.
	TestEqual(TEXT("and the flight is still Accepted - an armed arrival would already be Inbound"), Flight->GetPhase(), EFlightPhase::Accepted);
	Rig.Board->RearmSchedules(*Rig.Traffic, *Rig.Airport.Net, *Rig.Clock);
	TestEqual(TEXT("it is holding"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("and nothing was dispatched onto the busy runway"), Rig.Dispatched, 0);

	Rig.FreeRunway();
	Rig.Tick();
	TestEqual(TEXT("the queue clears it once the runway frees - a flight already due joins the queue at once, not dropped"), Rig.Dispatched, 1);
	TestEqual(TEXT("so it is landing, not still waiting"), Flight->GetPhase(), EFlightPhase::Landing);
	return true;
}

namespace
{
	/** A flight put straight into the queue, the shape a load or a refused clearance leaves. */
	UFlight* Holding(FQueueRig& Rig, double Wingspan, double Since)
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Airframe = QueueAirframe();
		Flight->Airframe.Wingspan = Wingspan;
		Flight->SetPhaseForTest(EFlightPhase::Inbound);
		Flight->HoldingSince = Since;
		Flight->RunwayPreference = Rig.Airport.Threshold;
		Rig.Board->AddOffer(*Rig.Clock, Flight);
		return Flight;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueStuckHeadTest, "AirportOps.Model.ArrivalQueue.StuckHeadDoesNotBlockTheQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueStuckHeadTest::RunTest(const FString& Parameters)
{
	// REVIEW C2: a head the runway can no longer take (here a span the strip does not admit -
	// in play, a runway shortened under a holding flight) is returned every frame by a
	// runway-busy-only test, and blocks everything behind it. The queue asks the real plan.
	FQueueRig Rig;
	UFlight* Stuck = Holding(Rig, 90000.0, 1.0);
	UFlight* Fine = Rig.Accepted(10.0);
	Rig.Clock->Advance(11.0);
	for (int32 Frame = 0; Frame < 5; ++Frame) { Rig.Tick(); }
	TestEqual(TEXT("the flight that can land did, once"), Rig.Dispatched, 1);
	TestEqual(TEXT("it is landing"), Fine->GetPhase(), EFlightPhase::Landing);
	TestEqual(TEXT("the stuck one is still holding, never thrown at the dispatcher"), Stuck->GetPhase(), EFlightPhase::Inbound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueRefusalQuietTest, "AirportOps.Model.ArrivalQueue.RefusalIsQuiet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueRefusalQuietTest::RunTest(const FString& Parameters)
{
	// REVIEW C1: a flight that cannot land yet must cost nothing visible frame after frame - no
	// dispatch attempt (which toasts), no board revision (which re-plans every offer row).
	FQueueRig Rig;
	Holding(Rig, 90000.0, 1.0);
	Rig.Tick();
	const uint32 After = Rig.Board->Revision();
	for (int32 Frame = 0; Frame < 10; ++Frame) { Rig.Tick(); }
	TestEqual(TEXT("never dispatched"), Rig.Dispatched, 0);
	TestEqual(TEXT("and the board did not move"), Rig.Board->Revision(), After);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueReholdTest, "AirportOps.Model.ArrivalQueue.HoldingWithoutAStandReholdsOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueReholdTest::RunTest(const FString& Parameters)
{
	// REVIEW I1: a holding flight that lost its hold must take one again as soon as a stand is
	// free, or the next offer could be accepted onto the stand it is about to land on.
	FQueueRig Rig;
	Rig.HoldRunway();
	UFlight* Flight = Holding(Rig, QueueAirframe().Wingspan, 1.0);
	TestFalse(TEXT("it starts with no stand"), Flight->Stand.IsSet());
	Rig.Tick();
	TestTrue(TEXT("and holds one after a tick"), Rig.StandHeldFor(*Flight));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueChurnFreedStandTest, "AirportOps.Model.ArrivalQueue.StandFreedByChurnIsNotStale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueChurnFreedStandTest::RunTest(const FString& Parameters)
{
	// THE CLEARANCE CACHE'S KNOCK-ON (PR D review): ClearanceFor caches NoFreeStand against OccupancyRevision, and a
	// stand freed by claim churn - a body rolling off the pose node - moves no revision. The pass then runs on the
	// StandsFreed event. It must not read the stale refusal: the stand-less flight's re-reserve (HoldStand, which does
	// bump the revision) comes first and invalidates it. Pinned: a HoldStand without its bump turns this red.
	FQueueRig Rig;
	UFlight* Flight = Holding(Rig, QueueAirframe().Wingspan, 1.0);
	const FGuidelineNodeId Pose = Rig.Airport.Net->GetEntity(Rig.Airport.Stands[0])->PoseNode;
	Rig.Traffic->OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Pose), /*bOccupied*/ true, 2));
	Rig.Traffic->Advance(0.05, Rig.Airport.Net);
	const uint32 Revision = Rig.Traffic->OccupancyRevision();
	Rig.Tick();
	TestEqual(TEXT("a body on the only stand: nothing cleared"), Rig.Dispatched, 0);
	TestFalse(TEXT("and no stand held"), Flight->Stand.IsSet());

	Rig.Traffic->OccupancyForTest().ReleaseAll(77);
	Rig.Traffic->Advance(0.05, Rig.Airport.Net);
	TestEqual(TEXT("the body rolled off with no revision bump - the cache's blind spot"), Rig.Traffic->OccupancyRevision(), Revision);
	Rig.Tick();
	TestTrue(TEXT("the pass takes the stand"), Flight->Stand.IsSet());
	TestEqual(TEXT("and clears the flight - not the cached NoFreeStand"), Rig.Dispatched, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueDeadStandTest, "AirportOps.Model.ArrivalQueue.DeadStandReservesWhenOneFrees",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueDeadStandTest::RunTest(const FString& Parameters)
{
	// PR C'S FOLLOW-UP: a holding flight whose stand was DELETED keeps the dead Stand (HeldStandLost's evidence), so
	// "no stand" alone never re-reserved it - it waited on a stand that will never come back. A dead stand is no stand.
	FQueueRig Rig(2);
	Rig.HoldRunway();
	UFlight* Holding = Rig.Accepted(1.0);
	UFlight* Later = Rig.Accepted(100000.0);
	if (!TestTrue(TEXT("both accepted, one stand each"), Holding != nullptr && Later != nullptr
		&& Holding->Stand.IsSet() && Later->Stand.IsSet() && Holding->Stand != Later->Stand)) { return false; }
	Rig.Clock->Advance(2.0);
	if (!TestEqual(TEXT("the first is holding"), Holding->GetPhase(), EFlightPhase::Inbound)) { return false; }

	const FEntityInstanceId Dead = Holding->Stand;
	Rig.Airport.Net->RemoveEntity(Dead);
	// AND THE GRAPH RE-DERIVED, as the deletion's own rebuild does in play: since #471 a re-hold is a plan, which refuses a
	// graph behind the road (GraphBeingEdited) - the size-only re-hold it replaced read no graph at all.
	TestGraph::Rebuild(*Rig.Airport.Net);
	TestTrue(TEXT("its stand is gone"), UStandAllocator::HeldStandIsGone(*Holding, *Rig.Airport.Net));
	Rig.Tick();
	TestEqual(TEXT("no stand is free, so it keeps the dead one"), Holding->Stand, Dead);

	// THE STAND FREES BEHIND THE BOARD'S BACK - the table lets go and Later's copy is left naming it. Since #442 the pass
	// brings that copy back to the table too (ReconcileStandHolds): Later gives the stand up, after the holding flight - the
	// queue is reconciled first - has taken it.
	const FEntityInstanceId Freed = Later->Stand;
	Rig.Traffic->ReleaseHold(Later->HolderId());
	Rig.Tick();
	TestEqual(TEXT("a stand frees: the holding flight takes it"), Holding->Stand, Freed);
	TestTrue(TEXT("and holds it"), Rig.StandHeldFor(*Holding));
	TestFalse(TEXT("and the accepted flight whose hold went names it no more - its copy agrees with the table"), Later->Stand.IsSet());
	return true;
}

namespace
{
	/**
	 * Every LogAirportOps Warning saying a cleared flight's dispatch was refused, while registered. UNBUFFERED
	 * (CanBeUsedOnMultipleThreads), or the dedicated log thread delivers the line after the spy is gone - the #216 reason.
	 */
	struct FQueueRefusalSpy : public FOutputDevice
	{
		int32 Lines = 0;
		FQueueRefusalSpy() { GLog->AddOutputDevice(this); }
		virtual ~FQueueRefusalSpy() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == FName(TEXT("LogAirportOps")) && (Verbosity & ELogVerbosity::VerbosityMask) == ELogVerbosity::Warning
				&& FCString::Strstr(V, TEXT("could not be cleared to land")) != nullptr)
			{
				++Lines;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueClearanceChurnTest, "AirportOps.Model.ArrivalQueue.ClearanceIsDatedByStandChurn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueClearanceChurnTest::RunTest(const FString& Parameters)
{
	// #497 RE-REVIEW: the clearance cache (FArrivalQueue's FClearance) was dated by the guideline and occupancy revisions alone, and a
	// body the per-tick claim pass rolls onto or off a stand's pose moves NEITHER - only UGroundTraffic::StandHoldChangeCount, which
	// FReholdMiss already reads. With an allocator the stand-less flight's re-hold bumps the revision first and hides the gap
	// (StandFreedByChurnIsNotStale); with NO allocator there is no re-hold, so this measures the cache's own date: a body on the only
	// stand, the clearance judged NoFreeStand, the body rolls off - and the next pass must judge again and clear it.
	FQueueRig Rig;
	Rig.Board->Allocator = nullptr;
	UFlight* Flight = Holding(Rig, QueueAirframe().Wingspan, 1.0);
	const FGuidelineNodeId Pose = Rig.Airport.Net->GetEntity(Rig.Airport.Stands[0])->PoseNode;
	Rig.Traffic->OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Pose), /*bOccupied*/ true, 2));
	Rig.Traffic->Advance(0.05, Rig.Airport.Net);
	Rig.Tick();
	if (!TestEqual(TEXT("PRECONDITION: a body on the only stand - nothing cleared"), Rig.Dispatched, 0)) { return false; }

	const uint32 Revision = Rig.Traffic->OccupancyRevision();
	const uint32 Churn = Rig.Traffic->StandHoldChangeCount();
	Rig.Traffic->OccupancyForTest().ReleaseAll(77);
	Rig.Traffic->Advance(0.05, Rig.Airport.Net);
	if (!TestEqual(TEXT("PRECONDITION: the body rolled off with no occupancy revision - the blind spot"), Rig.Traffic->OccupancyRevision(), Revision)) { return false; }
	if (!TestTrue(TEXT("PRECONDITION: but the stand churn counted it"), Rig.Traffic->StandHoldChangeCount() != Churn)) { return false; }
	Rig.Tick();
	TestEqual(TEXT("the clearance is judged again on the churn and the flight cleared - not the cached NoFreeStand"), Rig.Dispatched, 1);
	TestEqual(TEXT("and it lands"), Flight->GetPhase(), EFlightPhase::Landing);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueBodyOnStandQuietTest, "AirportOps.Model.ArrivalQueue.BodyOnTheStandRefusesNoDispatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueBodyOnStandQuietTest::RunTest(const FString& Parameters)
{
	// #497 RE-REVIEW's FEAR: a body rolling ONTO a stand while the clearance is cached None would let the queue dispatch every frame
	// into a refusal - a "could not be cleared to land" Warning a frame. Measured with the REAL dispatcher (DispatchArrival): the
	// accepted flight's only stand gains a body while it holds for a busy runway, the runway frees, and thirty passes run. The
	// clearance is re-judged (the plan sees the body: NoFreeStand), nothing is dispatched, and not one refusal is logged. Green before
	// the churn date was added too: a None is never cached across a pass - the pass that judges it clears the flight in the same call,
	// and a refused dispatch's release moves the occupancy revision - so the flood did not reproduce; this keeps it that way.
	FQueueRig Rig;
	UGroundTraffic* Traffic = Rig.Traffic;
	URoadNetwork* Net = Rig.Airport.Net;
	Rig.Board->Dispatcher = [Traffic, Net](const FVector2D& Near, const FAirframe& Frame)
	{
		return Traffic->DispatchArrival(*Net, Near, Frame, 1.0) != 0;
	};
	UFlight* Flight = Rig.Accepted(1.0);
	if (!TestNotNull(TEXT("accepted"), Flight)) { return false; }
	Rig.HoldRunway();
	Rig.Clock->Advance(2.0);
	Rig.Tick();
	if (!TestEqual(TEXT("PRECONDITION: holding behind the busy runway"), Flight->GetPhase(), EFlightPhase::Inbound)) { return false; }

	FQueueRefusalSpy Spy;
	const FGuidelineNodeId Pose = Net->GetEntity(Rig.Airport.Stands[0])->PoseNode;
	Traffic->OccupancyForTest().Assert(FTrafficClaim::Make(77, FTrafficResource::OfNode(Pose), /*bOccupied*/ true, 2));
	Traffic->Advance(0.05, Net);
	Rig.FreeRunway();
	for (int32 Pass = 0; Pass < 30; ++Pass)
	{
		Rig.Clock->Advance(1.0 / 30.0);
		Rig.Tick();
	}
	TestEqual(TEXT("a body on its stand: still holding"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("nothing dispatched into the refusal"), Traffic->GetAgentCount(), 0);
	TestEqual(TEXT("and no refused-dispatch Warning, let alone one a pass"), Spy.Lines, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueueUnusableFreeRunwayTest, "AirportOps.Model.ArrivalQueue.FreeRunwayItCannotUseIsNoClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FQueueUnusableFreeRunwayTest::RunTest(const FString& Parameters)
{
	// samples/refused.png (2026-10-01): two runways, the one the flight can use held, the other FREE but no use to it. The queue's
	// gate asked "is ANY arrival runway free" (ArrivalPlanner::IsRunwayBusy) and the cached clearance ignores the runway, so it
	// dispatched; the dispatch's own plan found the usable strip held and refused RunwayOccupied - every frame, about 200 refusals and
	// toasts a second. Here the second runway is a 30 m strip on its own, too short and with no way off it: free, and useless.
	FQueueRig Rig;
	UGroundTraffic* Traffic = Rig.Traffic;
	URoadNetwork* Net = Rig.Airport.Net;
	const FRoadNodeId A = Net->AddNode(FVector2D(0.0, 300000.0));
	const FRoadNodeId B = Net->AddNode(FVector2D(3000.0, 300000.0));
	Net->AddStraightSegment(A, B, TestProfiles::Runway());
	TestGraph::Rebuild(*Net);   // the whole derivation: Derive alone leaves the stands unlinked
	Rig.Board->Dispatcher = [Traffic, Net](const FVector2D& Near, const FAirframe& Frame)
	{
		return Traffic->DispatchArrival(*Net, Near, Frame, 1.0) != 0;
	};
	UFlight* Flight = Rig.Accepted(1.0);
	if (!TestNotNull(TEXT("accepted"), Flight)) { return false; }
	Rig.HoldRunway();
	TestFalse(TEXT("PRECONDITION: the useless strip is free, so 'any runway free' says go"),
		ArrivalPlanner::IsRunwayBusy(*Net, Flight->RunwayPreference, &Traffic->GetOccupancy()));

	FQueueRefusalSpy Spy;
	for (int32 Pass = 0; Pass < 30; ++Pass)
	{
		Rig.Clock->Advance(1.0 / 30.0 + (Pass == 0 ? 2.0 : 0.0));
		Rig.Tick();
	}
	TestEqual(TEXT("holding while the only runway it can use is held"), Flight->GetPhase(), EFlightPhase::Inbound);
	TestEqual(TEXT("nothing dispatched into the refusal"), Traffic->GetAgentCount(), 0);
	TestEqual(TEXT("and no refused-dispatch Warning, let alone one a pass"), Spy.Lines, 0);

	Rig.FreeRunway();
	Rig.Clock->Advance(1.0 / 30.0);
	Rig.Tick();
	TestEqual(TEXT("the frame its runway frees, it is cleared"), Flight->GetPhase(), EFlightPhase::Landing);
	return true;
}

#endif
