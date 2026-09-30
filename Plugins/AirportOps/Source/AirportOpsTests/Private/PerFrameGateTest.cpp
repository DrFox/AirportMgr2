#include "CoreMinimal.h"
#include "Entities/EntityDefinition.h"
#include "Misc/AutomationTest.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ScopeExit.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/GroundTraffic.h"
#include "Model/JobBoard.h"
#include "Model/RoadAgent.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"
#include "Model/StandAllocator.h"
#include "Present/OpsRuntime.h"
#include "Present/RoadNetworkActor.h"
#include "Testing/AirsideTestWorld.h"
#include "OpsTransitionTestHelpers.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * OPS BATCH 3 PR E, the AirportOps half (spec §6): work that ran every frame whether or not anything had changed.
 * Each test counts the calls, so a gate removed turns it red - the value alone would read the same either way.
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEmptyBoardCopiesNothingTest, "AirportOps.Model.FlightBoard.EmptyBoardCopiesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FEmptyBoardCopiesNothingTest::RunTest(const FString&)
{
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());

	for (int32 Frame = 0; Frame < 100; ++Frame) { Board->TickOffers(*Traffic, *Net, *Clock, 1.0 / 60.0); }
	TestEqual(TEXT("an empty inbox: 100 frames copy the flight list no times"), Board->OfferSnapshotCountForTest(), 0);

	// AND AN OFFER STILL DRAINS: the early-out is on the count of offers, not on the board being quiet.
	UFlight* Offer = NewObject<UFlight>(GetTransientPackage());
	Offer->OfferWindowSeconds = 60.0;
	Offer->OfferSecondsLeft = 60.0;
	Offer->LeadTimeSeconds = 900.0;
	Offer->ContractSeconds = 4200.0;
	Board->AddOffer(*Clock, Offer);
	Board->TickOffers(*Traffic, *Net, *Clock, 10.0);
	TestEqual(TEXT("with an offer, each frame walks the list"), Board->OfferSnapshotCountForTest(), 1);
	TestEqual(TEXT("and drains its countdown"), Offer->OfferSecondsLeft, 50.0, 1e-9);
	Board->TickOffers(*Traffic, *Net, *Clock, 60.0);
	TestEqual(TEXT("it lapses"), Offer->Phase, EFlightPhase::Expired);
	Board->TickOffers(*Traffic, *Net, *Clock, 1.0);
	TestEqual(TEXT("and the board is empty again: no copy"), Board->OfferSnapshotCountForTest(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobBoardRevisionTest, "AirportOps.Fuel.RevisionMovesOnEveryChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FJobBoardRevisionTest::RunTest(const FString&)
{
	// EVERY PUBLIC MUTATOR, since the state is private: the inspector keys the depot card and the fuel line on this
	// number, and a door that changed the board without it would leave either one stale on screen.
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	uint32 Last = Board->Revision();
	auto Moved = [&](const TCHAR* What)
	{
		TestTrue(FString::Printf(TEXT("%s moves the revision"), What), Board->Revision() != Last);
		Last = Board->Revision();
	};
	// A STEP, AN AGENT'S PHASE AND A RECALL ARE NOT HERE: since #443 they move it only when they change something, and a
	// call that changes nothing is what AirportOps.Fuel.RevisionHoldsStillWhenNothingChanged pins.
	Board->AddJobForTest(1, EServiceJobState::Queued, EServiceRefusal::None, 0);
	Moved(TEXT("a job added"));
	Board->AddVehicleForTest(TEXT("FUEL"), FEntityInstanceId(), EServiceVehicleState::Idle, 1000.0);
	Moved(TEXT("a vehicle added"));
	Board->AddTurnaroundForTest(1, 600.0, 1);
	Moved(TEXT("a turnaround added"));
	// THE PLAYER'S FLEET DOOR (facility-upgrades, #417; FServiceFleet since #443): a vehicle bought or sold changes the depot
	// card's vehicle list, which the card keys on this number alone within a game minute.
	FEntityInstanceId Depot;
	Depot.Index = 3;
	const int32 Bought = Board->Fleet().Add(TEXT("FUEL"), Depot, EFleetOrigin::Bought, 0.0);
	if (TestTrue(TEXT("a vehicle bought"), Bought != 0))
	{
		Moved(TEXT("a vehicle bought"));
		TestTrue(TEXT("and sold"), Board->Fleet().Withdraw(Bought, EFleetReason::Sold, 0.0));
		Moved(TEXT("a vehicle sold"));
	}
	Board->OnBeforeRestore();
	Moved(TEXT("a restore"));

	// THE POINTS OF CHANGE THAT MOVE NO VEHICLE (#443, "changes, not calls"), each on a FRESH board and each read off
	// RevisionCountForTest - the vehicle transitions that usually ride beside them move FleetRevision, so Revision() alone
	// cannot say whether the leaf's own ++RevisionCount is still there. Delete that line and its check goes red.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UEntityDefinition* Def = UEntityDefinition::MakeFuelDepotTransient();
	const FEntityInstanceId Home = Net->PlaceEntity(Def, Def->Anchors, FVector2D(0.0, 0.0), 0.0, 0.0, EServiceRole::Fuel, 0);
	auto PointBumps = [this](UJobBoard& Fresh, const TCHAR* What, TFunctionRef<void()> Do)
	{
		const uint32 Before = Fresh.RevisionCountForTest();
		Do();
		TestTrue(FString::Printf(TEXT("%s moves the change count on its own"), What), Fresh.RevisionCountForTest() != Before);
	};
	{
		// A QUEUE TRIMMED of a job that is gone (StartNext): an idle vehicle at home whose queue names a job nobody has.
		UJobBoard* Fresh = NewObject<UJobBoard>(GetTransientPackage());
		Fresh->AddVehicleForTest(TEXT("FUEL"), Home, EServiceVehicleState::Idle, 1000.0).Queue = { 9999 };
		PointBumps(*Fresh, TEXT("a queue trimmed of a job that is gone"), [&] { Fresh->Step(*Traffic, *Net, *Clock); });
	}
	{
		// A REFUSED JOB RE-OPENED (ReopenRefusedJob): refused at a graph revision the airport is no longer at.
		UJobBoard* Fresh = NewObject<UJobBoard>(GetTransientPackage());
		Fresh->AddJobForTest(1, EServiceJobState::Unserviceable, EServiceRefusal::NoDepot, /*RefusedAtRevision=*/777);
		PointBumps(*Fresh, TEXT("a refused job asking again"), [&] { Fresh->Step(*Traffic, *Net, *Clock); });
	}
	{
		// A JOB HANDED BACK TO THE BOARD (Reopen, via ReleaseJobsOf): the player's despawn of a vehicle holding a queued job.
		UJobBoard* Fresh = NewObject<UJobBoard>(GetTransientPackage());
		FServiceVehicle& Vehicle = Fresh->AddVehicleForTest(TEXT("FUEL"), Home, EServiceVehicleState::ToFacility, 1000.0);
		Vehicle.AgentId = 5;
		const int32 VehicleId = Vehicle.Id;
		FServiceJob& Job = Fresh->AddJobForTest(2, EServiceJobState::Queued, EServiceRefusal::None, 0);
		Job.VehicleId = VehicleId;
		const int32 JobId = Job.Id;
		const_cast<FServiceVehicle*>(Fresh->FindVehicle(VehicleId))->Queue = { JobId };
		PointBumps(*Fresh, TEXT("a queued job handed back"), [&] { Fresh->RecallVehicleOfAgent(5, /*bRetire*/ true, *Traffic, *Net, *Clock); });
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJobBoardRevisionStillTest, "AirportOps.Fuel.RevisionHoldsStillWhenNothingChanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FJobBoardRevisionStillTest::RunTest(const FString&)
{
	// #443: Revision COUNTED CALLS, NOT CHANGES - it moved on entry to every OnAgentPhase for every agent in the airport and
	// to every Step, so the inspector's depot card and fuel line (keyed on it) were rebuilt for changes that were not there.
	// A call that changes nothing leaves it where it was; a call that does change something still moves it.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	const uint32 Start = Board->Revision();
	Board->Step(*Traffic, *Net, *Clock);
	TestEqual(TEXT("a Step with nothing to do changes nothing"), Board->Revision(), Start);
	Board->OnAgentPhase(*Traffic, *Net, *Clock, OpsTestTransition(5, EAgentPhase::Gone, EAgentPhase::Taxiing, EAgentEvent::Dispatched));
	TestEqual(TEXT("an agent's phase that is none of the board's business changes nothing"), Board->Revision(), Start);
	TestFalse(TEXT("a recall of an agent that drives no vehicle here reports it"),
		Board->RecallVehicleOfAgent(5, /*bRetire*/ false, *Traffic, *Net, *Clock));
	TestEqual(TEXT("and changes nothing"), Board->Revision(), Start);

	// AND A STEP THAT DOES CHANGE SOMETHING STILL MOVES IT: an open job on an airport with no depot is refused NoDepot.
	Board->AddJobForTest(1, EServiceJobState::Open, EServiceRefusal::None, 0);
	const uint32 Staged = Board->Revision();
	Board->Step(*Traffic, *Net, *Clock);
	TestTrue(TEXT("a Step that refuses a job moves it - the fuel line now says why"), Board->Revision() != Staged);
	const FServiceJob* Job = Board->JobForAircraft(1);
	if (!TestNotNull(TEXT("setup: the job is on the board"), Job)) { return false; }
	TestEqual(TEXT("setup: refused"), static_cast<int32>(Job->State), static_cast<int32>(EServiceJobState::Unserviceable));
	const uint32 Refused = Board->Revision();
	Board->Step(*Traffic, *Net, *Clock);
	TestEqual(TEXT("and the next Step, finding it refused and the airport unchanged, does not"), Board->Revision(), Refused);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelLineLiveTest, "AirportOps.Fuel.LineSaysWhenItMovesWithTheClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelLineLiveTest::RunTest(const FString&)
{
	// THE FUEL LINE COUNTS DOWN WHILE PUMPING and holds still otherwise - so the inspector may keep it until the
	// board moves, except while it is live. The flag is what tells the two apart.
	UJobBoard* Board = NewObject<UJobBoard>(GetTransientPackage());
	FServiceJob& Job = Board->AddJobForTest(1, EServiceJobState::Serving, EServiceRefusal::None, 0);
	Job.QuantityOwed = 2900.0;
	Job.TankLitres = 1000.0;
	Job.TripQuantity = 1000.0;
	Job.TripStartedAt = 0.0;
	Job.TripEndsAt = 800.0;
	bool bLive = false;
	const FString AtStart = Board->DescribeAgent(1, 0.0, bLive);
	TestTrue(TEXT("pumping: it moves with the clock"), bLive);
	TestNotEqual(TEXT("and it does - 400 s in, less is left"), Board->DescribeAgent(1, 400.0, bLive), AtStart);
	Job.State = EServiceJobState::Underway;
	Board->DescribeAgent(1, 0.0, bLive);
	TestFalse(TEXT("a truck on its way: it does not"), bLive);
	Job.State = EServiceJobState::Done;
	Board->DescribeAgent(1, 0.0, bLive);
	TestFalse(TEXT("done: it does not"), bLive);
	Board->DescribeAgent(42, 0.0, bLive);
	TestFalse(TEXT("an agent it knows nothing of: it does not"), bLive);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSimTimeScaleOnChangeTest, "AirportOps.Present.SimTimeScale.SetOnlyWhenItChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FSimTimeScaleOnChangeTest::RunTest(const FString&)
{
	// THE COMPOSITION: the actor's scale is what its Tick multiplies the traffic's delta by
	// (Airside.Present.SimTimeScale), so the actor moving agents at the player's speed is this number being right.
	// It used to be re-set every frame to the same double; now only where it can change - and each of those is here.
	FAirsideTestWorld TestWorld;
	ARoadNetworkActor* Actor = TestWorld.Actor;
	if (!TestNotNull(TEXT("actor"), Actor)) { return false; }
	Actor->PlaceNode(FVector2D(0.0, 30000.0));
	UOpsRuntime* Runtime = NewObject<UOpsRuntime>(GetTransientPackage());
	Runtime->Attach(Actor);
	const USimClock& Clock = *Runtime->GetClock();
	auto AtSpeed = [&](const TCHAR* When)
	{
		TestEqual(FString::Printf(TEXT("%s: the actor runs at the clock's multiplier"), When),
			Actor->GetSimTimeScale(), USimClock::Multiplier(Clock.GetSpeed()), 1e-12);
	};
	AtSpeed(TEXT("attached"));

	const int32 Sets = Runtime->TimeScaleSetsForTest();
	for (int32 Frame = 0; Frame < 100; ++Frame) { Runtime->Tick(1.0 / 30.0); }
	TestEqual(TEXT("100 frames at one speed set the scale no times"), Runtime->TimeScaleSetsForTest() - Sets, 0);

	Runtime->StepSpeed(+1);
	AtSpeed(TEXT("a speed step"));
	const double Saved = Actor->GetSimTimeScale();
	const FString Slot = TEXT("AirportOpsTest_SimTimeScale");
	// THE SLOT GOES WITH THE TEST, however it ends - a save left on disk is state the next run did not make.
	ON_SCOPE_EXIT { UGameplayStatics::DeleteGameInSlot(Slot, 0); };
	if (!TestTrue(TEXT("saved"), Runtime->SaveToSlot(Slot))) { return false; }
	Runtime->StepSpeed(+1);
	AtSpeed(TEXT("another step"));
	if (!TestTrue(TEXT("a faster speed than the save's"), Actor->GetSimTimeScale() > Saved)) { return false; }

	if (!TestTrue(TEXT("loaded"), Runtime->LoadFromSlot(Slot))) { return false; }
	TestEqual(TEXT("a load: the saved speed reaches the actor"), Actor->GetSimTimeScale(), Saved, 1e-12);
	AtSpeed(TEXT("a load"));

	// A NETWORK CLEAR replaces the network object and keeps the actor, whose scale is x2 here - so a clear that reset
	// the scale to its default (1) would read wrong below, and the runtime does NOT repair it: no set in between. The
	// only code that could make this red is a second writer of the scale, which rule 34 (scale-on-change) fails first.
	const int32 SetsBeforeClear = Runtime->TimeScaleSetsForTest();
	if (!TestNotEqual(TEXT("the actor is off its default scale - or the clear step measures nothing"), Actor->GetSimTimeScale(), 1.0)) { return false; }
	Actor->ClearNetwork();
	Runtime->Tick(1.0 / 30.0);
	AtSpeed(TEXT("a network clear"));
	TestEqual(TEXT("and the runtime set nothing to make it so"), Runtime->TimeScaleSetsForTest() - SetsBeforeClear, 0);

	Runtime->TogglePause();
	AtSpeed(TEXT("paused"));
	Runtime->TogglePause();
	AtSpeed(TEXT("resumed"));

	// A NEW ACTOR: a level change, or PIE's duplicate. Its Transient scale starts at 1 whatever the player's speed, and
	// Attach is the only thing that tells it otherwise.
	Runtime->StepSpeed(+1);
	ARoadNetworkActor* Fresh = TestWorld.World->SpawnActor<ARoadNetworkActor>();
	if (!TestNotNull(TEXT("a second actor"), Fresh)) { return false; }
	Fresh->PlaceNode(FVector2D(0.0, 30000.0));
	if (!TestEqual(TEXT("it starts at its default"), Fresh->GetSimTimeScale(), 1.0, 1e-12)) { return false; }
	Runtime->Attach(Fresh);
	TestEqual(FString::Printf(TEXT("attached to it at x%.0f: it runs at the clock's multiplier"), USimClock::Multiplier(Clock.GetSpeed())),
		Fresh->GetSimTimeScale(), USimClock::Multiplier(Clock.GetSpeed()), 1e-12);
	TestNotEqual(TEXT("which is not its default - or this measured nothing"), Fresh->GetSimTimeScale(), 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FArrivalRequeuesOnlyAcceptedTest, "AirportOps.Model.FlightBoard.ArrivalRequeuesOnlyAnAccepted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FArrivalRequeuesOnlyAcceptedTest::RunTest(const FString&)
{
	// THE ARRIVAL CALLBACK ASKS WHAT IT IS QUEUEING (whole-stack review M3): it found its flight by id - which History
	// keeps - and put it in the queue whatever it had become. Every cancel today disarms it first; a cancel that did
	// not would have landed a cancelled flight. Only an Accepted flight joins the queue at its ETA.
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	UGroundTraffic* Traffic = NewObject<UGroundTraffic>(GetTransientPackage());
	USimClock* Clock = NewObject<USimClock>(GetTransientPackage());
	UFlightBoard* Board = NewObject<UFlightBoard>(GetTransientPackage());
	Board->Allocator = NewObject<UStandAllocator>(GetTransientPackage());
	auto Plant = [&]()
	{
		UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
		Flight->Phase = EFlightPhase::Accepted;
		Flight->ArrivesAt = Clock->Now() + 10.0;
		Board->AddOffer(*Clock, Flight);
		return Flight;
	};
	UFlight* Kept = Plant();
	UFlight* Cancelled = Plant();
	Board->RearmSchedules(*Traffic, *Net, *Clock);
	Cancelled->Phase = EFlightPhase::Cancelled;   // a cancel that left its arrival armed
	const double Until = Kept->ArrivesAt + 1.0;
	for (int32 Step = 0; Step < 100000 && Clock->Now() < Until; ++Step) { Clock->Advance(0.5 / FMath::Max(Clock->TimeScale(), 1e-6)); }
	TestEqual(TEXT("an Accepted flight joins the queue at its ETA - the arrival was armed"), Kept->Phase, EFlightPhase::Inbound);
	TestEqual(TEXT("a cancelled one stays cancelled"), Cancelled->Phase, EFlightPhase::Cancelled);
	return true;
}

#endif
