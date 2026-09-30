#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineRoster.h"
#include "Model/Flight.h"
#include "Model/FlightBoard.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

// AIRLINE SATISFACTION (spec 2026-09-29 §3), world-free. The roster's handlers are wired onto a bare
// bus here exactly as UOpsRuntime::WireBus wires them, so each test drives it through the bus.

namespace
{
	struct FRosterFixture
	{
		UAirlineRoster* Roster = nullptr;
		FOpsEventBus Bus;

		/** The board the roster resolves a turnaround's airline through: one flight of airline A, flown by agent 7. */
		UFlightBoard* Flights = nullptr;
		TArray<FAirlineSatisfactionEvent> Changes;

		FRosterFixture()
		{
			Roster = NewObject<UAirlineRoster>(GetTransientPackage());
			Roster->Bus = &Bus;
			Roster->Ensure(TEXT("A"));
			Flights = NewObject<UFlightBoard>(GetTransientPackage());
			UFlight* Flight = NewObject<UFlight>(GetTransientPackage());
			Flight->AirlineId = TEXT("A");
			Flight->AgentId = 7;
			Flight->SetPhaseForTest(EFlightPhase::TaxiOut);
			Flights->AddOffer(*NewObject<USimClock>(GetTransientPackage()), Flight);
			Bus.BeginWiring();
			Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FFlightAirborneEvent& E) { Roster->OnFlightAirborne(E); });
			Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FOfferExpiredEvent& E) { Roster->OnOfferExpired(E); });
			Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FOfferDeclinedEvent& E) { Roster->OnOfferDeclined(E); });
			Bus.Subscribe<FDayEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FDayEndedEvent& E) { Roster->OnDayEnded(E); });
			Bus.Subscribe<FTurnaroundEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FTurnaroundEndedEvent& E) { Roster->OnTurnaroundEnded(E, Flights); });
			Bus.Subscribe<FFlightCancelledEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FFlightCancelledEvent& E) { Roster->OnFlightCancelled(E); });
			Bus.Subscribe<FAirlineSatisfactionEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FAirlineSatisfactionEvent& E) { Changes.Add(E); });
			Bus.EndWiring();
		}

		double Sat(FName Id = TEXT("A")) const
		{
			const FAirlineStanding* Row = Roster->Find(Id);
			return Row != nullptr ? Row->Satisfaction : -1.0;
		}

		void TurnaroundEnded(EFuelOutcome Outcome, double Delivered, double Wanted, int32 Agent = 7)
		{
			FTurnaroundEndedEvent Event;
			Event.AircraftAgentId = Agent;
			Event.Outcome = Outcome;
			Event.Delivered = Delivered;
			Event.Wanted = Wanted;
			Bus.Publish(MoveTemp(Event));
			Bus.Drain();
		}

		void Airborne(double LateBy, FName Id = TEXT("A"))
		{
			Bus.Publish(FFlightAirborneEvent{ 1, Id, LateBy });
			Bus.Drain();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesOnTimeTest, "AirportOps.Model.Airlines.OnTimeRaises",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesOnTimeTest::RunTest(const FString&)
{
	FRosterFixture F;
	TestEqual(TEXT("a new airline starts at the tuning's start"), F.Sat(), 0.5, 1e-9);
	F.Airborne(-30.0);
	TestEqual(TEXT("airborne before the deadline is on time: +0.03"), F.Sat(), 0.53, 1e-9);
	if (!TestEqual(TEXT("the change is published once"), F.Changes.Num(), 1)) { return false; }
	TestTrue(TEXT("and says why"), F.Changes[0].Cause.Contains(TEXT("on time")));
	TestEqual(TEXT("with the figures before and after"), F.Changes[0].Old, 0.5, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesLateTest, "AirportOps.Model.Airlines.LateLowers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesLateTest::RunTest(const FString&)
{
	{
		FRosterFixture F;
		F.Airborne(1500.0);   // 25 min: 2.5 tenths x 0.02
		TestEqual(TEXT("late costs 0.02 per ten game minutes"), F.Sat(), 0.45, 1e-9);
		TestTrue(TEXT("the cause names the lateness"), F.Changes.Num() == 1 && F.Changes[0].Cause.Contains(TEXT("25 min")));
	}
	{
		FRosterFixture F;
		F.Airborne(6000.0);   // 100 min would be 0.20
		TestEqual(TEXT("capped at 0.10 - two hours late has missed its slot as surely as ten minutes"), F.Sat(), 0.40, 1e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesExpiredTest, "AirportOps.Model.Airlines.ExpiredByReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesExpiredTest::RunTest(const FString&)
{
	{
		FRosterFixture F;
		F.Bus.Publish(FOfferExpiredEvent{ 1, TEXT("A"), ELapseReason::Ignored });
		F.Bus.Drain();
		TestEqual(TEXT("an offer the player could have taken and ignored costs 0.02"), F.Sat(), 0.48, 1e-9);
	}
	{
		FRosterFixture F;
		F.Bus.Publish(FOfferExpiredEvent{ 1, TEXT("A"), ELapseReason::NeverAcceptable });
		F.Bus.Drain();
		TestEqual(TEXT("one that no stand could take costs less, 0.01 - the airport's fault, not inattention"), F.Sat(), 0.49, 1e-9);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDeclinedTest, "AirportOps.Model.Airlines.DeclinedIsFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDeclinedTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.Bus.Publish(FOfferDeclinedEvent{ 1, TEXT("A") });
	F.Bus.Drain();
	TestEqual(TEXT("declining is a legitimate choice - no penalty"), F.Sat(), 0.5, 1e-9);
	TestEqual(TEXT("and, since nothing moved, nothing is announced"), F.Changes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDriftTest, "AirportOps.Model.Airlines.DayDrift",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDriftTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.Roster->Ensure(TEXT("B"));
	for (int32 Index = 0; Index < 20; ++Index) { F.Airborne(-1.0); }      // A up to 1.0 (clamped)
	for (int32 Index = 0; Index < 3; ++Index) { F.Airborne(6000.0, TEXT("B")); }   // B to 0.2
	TestEqual(TEXT("A is clamped at the ceiling"), F.Sat(), 1.0, 1e-9);
	TestEqual(TEXT("B has fallen to 0.2"), F.Sat(TEXT("B")), 0.2, 1e-9);
	F.Bus.Publish(FDayEndedEvent{ 1 });
	F.Bus.Drain();
	TestEqual(TEXT("a day forgives 20% of the way back to the start, from above"), F.Sat(), 0.9, 1e-9);
	TestEqual(TEXT("and from below"), F.Sat(TEXT("B")), 0.26, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesClampedTest, "AirportOps.Model.Airlines.Clamped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesClampedTest::RunTest(const FString&)
{
	FRosterFixture F;
	for (int32 Index = 0; Index < 40; ++Index) { F.Airborne(6000.0); }
	TestEqual(TEXT("satisfaction never goes below zero"), F.Sat(), 0.0, 1e-9);
	TestEqual(TEXT("a zero airline offers at the bottom of the range"), F.Roster->RateMultiplier(TEXT("A"), false), 0.5, 1e-9);
	TestEqual(TEXT("a penalty at zero moves nothing and so announces nothing"), F.Changes.Num(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesFloorTest, "AirportOps.Model.Airlines.FloorNeverBelowOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesFloorTest::RunTest(const FString&)
{
	FRosterFixture F;
	for (int32 Index = 0; Index < 10; ++Index) { F.Airborne(6000.0); }
	TestEqual(TEXT("a floor airline at zero still offers at 1.0x - the floor exists so the airport is never empty"),
		F.Roster->RateMultiplier(TEXT("A"), true), 1.0, 1e-9);
	TestEqual(TEXT("an ordinary one at zero offers at half"), F.Roster->RateMultiplier(TEXT("A"), false), 0.5, 1e-9);
	for (int32 Index = 0; Index < 40; ++Index) { F.Airborne(-1.0); }
	TestEqual(TEXT("and a delighted floor airline still gets the top of the range"), F.Roster->RateMultiplier(TEXT("A"), true), 1.5, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesUnknownTest, "AirportOps.Model.Airlines.UnknownAirlineIgnored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesUnknownTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.Airborne(999.0, NAME_None);            // the debug flight (key 7)
	F.Airborne(999.0, TEXT("Removed"));      // an airline since taken out of content
	F.Bus.Publish(FOfferExpiredEvent{ 2, TEXT("Removed"), ELapseReason::Ignored });
	F.Bus.Drain();
	TestEqual(TEXT("an event for an airline with no row changes nothing"), F.Changes.Num(), 0);
	TestNull(TEXT("and grows no row - the roster is seeded from the catalog, never from an event"), F.Roster->Find(TEXT("Removed")));
	TestEqual(TEXT("only the seeded airline has a row"), F.Roster->GetStandings().Num(), 1);
	TestEqual(TEXT("an unknown airline's demand is not scaled"), F.Roster->RateMultiplier(NAME_None, false), 1.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesRecentTest, "AirportOps.Model.Airlines.RecentCapped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesRecentTest::RunTest(const FString&)
{
	FRosterFixture F;
	for (int32 Index = 0; Index < 6; ++Index) { F.Airborne(-1.0); }
	F.Airborne(1500.0);
	const FAirlineStanding* Row = F.Roster->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the row"), Row)) { return false; }
	TestEqual(TEXT("only the last few changes are kept"), Row->Recent.Num(), UAirlineRoster::RecentCap);
	TestTrue(TEXT("newest last - the row reads the latest cause"), Row->Recent.Last().Cause.Contains(TEXT("late")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesSaveTest, "AirportOps.Model.Airlines.SaveRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesSaveTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.Airborne(1500.0);
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	TArray<IOpsPersistent*> Saved = { F.Roster };
	OpsSave::Capture(Saved, *Net, Snapshot);

	UAirlineRoster* Loaded = NewObject<UAirlineRoster>(GetTransientPackage());
	Loaded->Ensure(TEXT("Stale"));   // a load replaces what was there, it does not merge
	TArray<IOpsPersistent*> Into = { Loaded };
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, Into, *Net))) { return false; }
	const FAirlineStanding* Row = Loaded->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the airline comes back"), Row)) { return false; }
	TestEqual(TEXT("at the satisfaction it was saved at"), Row->Satisfaction, 0.45, 1e-9);
	TestEqual(TEXT("with its history, so the row can still say why"), Row->Recent.Num(), 1);
	TestNull(TEXT("and nothing the loading roster held before"), Loaded->Find(TEXT("Stale")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesFloorLapseTest, "AirportOps.Model.Airlines.FloorLapseIsFree",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesFloorLapseTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.Bus.Publish(FOfferExpiredEvent{ 1, TEXT("A"), ELapseReason::Ignored, /*bFloorAirline=*/true });
	F.Bus.Drain();
	TestEqual(TEXT("a floor airline's lapsed offer costs nothing - rulings 7 and 8, UFlight::bFloorAirline"), F.Sat(), 0.5, 1e-9);
	TestEqual(TEXT("and says nothing"), F.Changes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesDriftSettlesTest, "AirportOps.Model.Airlines.DriftSettlesAndKeepsTheCause",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesDriftSettlesTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.Airborne(1500.0);   // 0.45, "late departure (25 min)"
	for (int32 Day = 1; Day <= 40; ++Day)
	{
		F.Bus.Publish(FDayEndedEvent{ Day });
		F.Bus.Drain();
	}
	TestEqual(TEXT("the drift reaches the start exactly rather than creeping at it for ever"), F.Sat(), 0.5, 1e-12);
	const int32 Announced = F.Changes.Num();
	F.Bus.Publish(FDayEndedEvent{ 41 });
	F.Bus.Drain();
	TestEqual(TEXT("and once there, a day ending announces nothing"), F.Changes.Num(), Announced);
	const FAirlineStanding* Row = F.Roster->Find(TEXT("A"));
	if (!TestNotNull(TEXT("the row"), Row)) { return false; }
	TestTrue(TEXT("the row's cause is still what the player did, not the forgiveness"),
		Row->Recent.Num() == 1 && Row->Recent.Last().Cause.Contains(TEXT("late")));
	return true;
}


// --- TurnaroundEnded (spec 2026-09-29-ops-batch3 §2): the shortfall the log used to be the only witness of.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesPartFuelledTest, "AirportOps.Model.Airlines.TurnaroundPartFuelledScoresInProportion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesPartFuelledTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.TurnaroundEnded(EFuelOutcome::PartFuelled, 1000.0, 2500.0);
	// 60% short of 0.06: -0.036. PROPORTIONAL, one knob - a truck that got most of the way there costs less
	// than one that never came (user ruling 2026-09-29).
	TestEqual(TEXT("part-fuelled costs the penalty times the fraction not delivered"), F.Sat(), 0.5 - 0.06 * 0.6, 1e-9);
	if (!TestEqual(TEXT("the change is published once"), F.Changes.Num(), 1)) { return false; }
	TestEqual(TEXT("and its cause is what the inbox row shows"), F.Changes[0].Cause, FString(TEXT("left part-fuelled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesUnfuelledTest, "AirportOps.Model.Airlines.TurnaroundUnfuelledScoresTheFullPenalty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesUnfuelledTest::RunTest(const FString&)
{
	FRosterFixture F;
	F.TurnaroundEnded(EFuelOutcome::Unfuelled, 0.0, 2500.0);
	TestEqual(TEXT("nothing delivered costs the whole penalty"), F.Sat(), 0.5 - 0.06, 1e-9);
	if (!TestEqual(TEXT("the change is published once"), F.Changes.Num(), 1)) { return false; }
	TestEqual(TEXT("left unfuelled"), F.Changes[0].Cause, FString(TEXT("left unfuelled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesFuelledTest, "AirportOps.Model.Airlines.TurnaroundFuelledScoresNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesFuelledTest::RunTest(const FString&)
{
	FRosterFixture F;
	// FUELLED SCORES 0 (user ruling 2026-09-29): the on-time bonus already rewards a turnaround that went right,
	// and paying for it twice would make fuel worth more to an airline than punctuality.
	F.TurnaroundEnded(EFuelOutcome::Fuelled, 2500.0, 2500.0);
	// WANTED 0 IS FUELLED, whatever the outcome says: an aircraft that asked for nothing was not let down.
	F.TurnaroundEnded(EFuelOutcome::Unfuelled, 0.0, 0.0);
	TestEqual(TEXT("neither moves the airline"), F.Sat(), 0.5, 1e-9);
	TestEqual(TEXT("and neither is announced"), F.Changes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesTurnaroundNoFlightTest, "AirportOps.Model.Airlines.TurnaroundOfNoFlightIsSkipped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesTurnaroundNoFlightTest::RunTest(const FString&)
{
	FRosterFixture F;
	// AGENT 99 FLIES NO FLIGHT - key 7's arrival before it had one, or a flight already retired. Skipped: an
	// event is a fact about the past, and there is no airline to tell.
	F.TurnaroundEnded(EFuelOutcome::Unfuelled, 0.0, 2500.0, /*Agent=*/99);
	TestEqual(TEXT("no airline is charged for an aircraft that flew for none"), F.Sat(), 0.5, 1e-9);
	TestEqual(TEXT("and nothing is announced"), F.Changes.Num(), 0);
	// AND THE RESOLUTION IS BY AGENT, not "whichever airline": the same event for agent 7 does score.
	F.TurnaroundEnded(EFuelOutcome::Unfuelled, 0.0, 2500.0, /*Agent=*/7);
	TestEqual(TEXT("the flight agent 7 flies is found, and its airline charged"), F.Sat(), 0.44, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesClosureCancelTest, "AirportOps.Model.Airlines.ClosureCancelScoresOnlyAirportClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesClosureCancelTest::RunTest(const FString&)
{
	FRosterFixture F;
	// ONLY THE PLAYER'S CLOSURE COSTS (spec 2026-09-29-ops-batch3 §0): the runway loophole was accepted, and a
	// despawn is a rescue, not an insult.
	F.Bus.Publish(FFlightCancelledEvent{ 1, TEXT("A"), ECancelReason::NoRunway });
	F.Bus.Publish(FFlightCancelledEvent{ 2, TEXT("A"), ECancelReason::Unstuck });
	F.Bus.Drain();
	TestEqual(TEXT("a flight lost to a missing runway, or to Unstick, costs nothing"), F.Sat(), 0.5, 1e-9);
	TestEqual(TEXT("and is not announced"), F.Changes.Num(), 0);

	if (!TestTrue(TEXT("the penalty is a real cost"), F.Roster->Tuning.ClosureCancelPenalty > 0.0)) { return false; }
	F.Bus.Publish(FFlightCancelledEvent{ 3, TEXT("A"), ECancelReason::AirportClosed });
	F.Bus.Drain();
	TestEqual(TEXT("a flight the player's closure cancelled costs the closure penalty"), F.Sat(), 0.5 - F.Roster->Tuning.ClosureCancelPenalty, 1e-9);
	if (!TestEqual(TEXT("announced once"), F.Changes.Num(), 1)) { return false; }
	TestEqual(TEXT("with the cause the inbox row shows"), F.Changes[0].Cause, FString(TEXT("cancelled: airport closed")));

	F.Bus.Publish(FFlightCancelledEvent{ 4, TEXT("NoSuchAirline"), ECancelReason::AirportClosed });
	F.Bus.Drain();
	TestEqual(TEXT("an airline the roster was never seeded with grows no row"), F.Roster->Find(TEXT("NoSuchAirline")) == nullptr, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAirlinesPlayerCancelTest, "AirportOps.Model.Airlines.PlayerCancelCostsTheClosurePenalty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FAirlinesPlayerCancelTest::RunTest(const FString&)
{
	// THE PLAYER'S CANCEL OF A FLIGHT THAT HAD NOT ARRIVED (#442) costs the SAME per-flight penalty the player's closure does - the
	// owner's open question is whether it should cost at all (see UFlightBoard::CancelByPlayer), and until that is ruled the two
	// exits out of an unlandable holding flight cost alike.
	FRosterFixture F;
	if (!TestTrue(TEXT("the penalty is a real cost"), F.Roster->Tuning.ClosureCancelPenalty > 0.0)) { return false; }
	F.Bus.Publish(FFlightCancelledEvent{ 1, TEXT("A"), ECancelReason::PlayerCancelled });
	F.Bus.Drain();
	TestEqual(TEXT("a flight the player cancelled costs the closure penalty"), F.Sat(), 0.5 - F.Roster->Tuning.ClosureCancelPenalty, 1e-9);
	if (!TestEqual(TEXT("announced once"), F.Changes.Num(), 1)) { return false; }
	TestEqual(TEXT("with its own cause"), F.Changes[0].Cause, FString(TEXT("cancelled by the player")));

	F.Bus.Publish(FFlightCancelledEvent{ 2, TEXT("A"), ECancelReason::AirportClosed });
	F.Bus.Drain();
	TestEqual(TEXT("and the closure still costs exactly the same, on top"), F.Sat(), 0.5 - 2.0 * F.Roster->Tuning.ClosureCancelPenalty, 1e-9);
	return true;
}

#endif
