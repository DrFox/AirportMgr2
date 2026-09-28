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
			Flight->ApproachFocus = Airport.Threshold;
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
	Offer->ApproachFocus = Rig.Airport.Threshold;
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
	TestEqual(TEXT("past its ETA it is holding, not lost"), Flight->Phase, EFlightPhase::Inbound);
	TestEqual(TEXT("it joined the queue at its ETA"), Flight->HoldingSince, Flight->ArrivesAt, 1.0);
	TestTrue(TEXT("its stand is still held for it"), Rig.StandHeldFor(*Flight));
	Rig.Tick();
	TestEqual(TEXT("nothing is cleared onto a busy runway"), Rig.Dispatched, 0);
	TestEqual(TEXT("it is in the queue"), Rig.Board->Queue().Num(), 1);

	Rig.FreeRunway();
	Rig.Tick();
	TestEqual(TEXT("the frame the runway frees, it is cleared"), Rig.Dispatched, 1);
	TestEqual(TEXT("and lands"), Flight->Phase, EFlightPhase::Landing);
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
	TestEqual(TEXT("the first to join goes first"), First->Phase, EFlightPhase::Landing);
	TestEqual(TEXT("the second still waits"), Second->Phase, EFlightPhase::Inbound);
	Rig.Tick();
	TestEqual(TEXT("then the second"), Second->Phase, EFlightPhase::Landing);
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
	UFlight* Next = Sequencer->Next(Queue, [OnBusy](const UFlight& F) { return &F == OnBusy; });
	TestTrue(TEXT("the flight whose runway is free goes"), Next == OnFree);
	TestNull(TEXT("and with every runway busy, nobody"),
		Sequencer->Next(Queue, [](const UFlight&) { return true; }));
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
	TestEqual(TEXT("and the flight is still holding"), Flight->Phase, EFlightPhase::Inbound);
	TestTrue(TEXT("with its stand held again"), Rig.StandHeldFor(*Flight));
	Rig.bDispatcherAccepts = true;
	Rig.Tick();
	TestEqual(TEXT("retried, it lands"), Flight->Phase, EFlightPhase::Landing);
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
	if (!TestEqual(TEXT("holding before the save"), Flight->Phase, EFlightPhase::Inbound)) { return false; }

	TArray<uint8> Bytes;
	OpsSave::SerializeObject(*Rig.Board, Bytes);
	FQueueRig Loaded;
	OpsSave::DeserializeObject(*Loaded.Board, Bytes);
	Loaded.Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	Loaded.Board->Sequencer = NewObject<UArrivalSequencer>(GetTransientPackage());
	Loaded.Board->Dispatcher = [&Loaded](const FVector2D&, const FAirframe&) { ++Loaded.Dispatched; return true; };
	Loaded.Board->OnGraphRebuilt(*Loaded.Traffic, *Loaded.Airport.Net);
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
	// not a blind dispatch, decides whether the runway is free.
	FQueueRig Rig;
	UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
	Flight->Airframe = QueueAirframe();
	Flight->Phase = EFlightPhase::Accepted;
	Flight->ArrivesAt = -5.0;
	Flight->ApproachFocus = Rig.Airport.Threshold;
	Rig.Board->AddOffer(*Rig.Clock, Flight);
	Rig.HoldRunway();
	Rig.Board->RearmSchedules(*Rig.Traffic, *Rig.Airport.Net, *Rig.Clock);
	TestEqual(TEXT("it is holding"), Flight->Phase, EFlightPhase::Inbound);
	TestEqual(TEXT("and nothing was dispatched onto the busy runway"), Rig.Dispatched, 0);
	return true;
}

#endif
