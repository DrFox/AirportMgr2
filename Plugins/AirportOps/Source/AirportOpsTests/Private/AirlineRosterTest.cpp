#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/AirlineRoster.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

// AIRLINE SATISFACTION (spec 2026-09-29 §3), world-free. The roster's handlers are wired onto a bare
// bus here exactly as UOpsRuntime::WireBus wires them, so each test drives it through the bus.

namespace
{
	struct FRosterFixture
	{
		UAirlineRoster* Roster = nullptr;
		FOpsEventBus Bus;
		TArray<FAirlineSatisfactionEvent> Changes;

		FRosterFixture()
		{
			Roster = NewObject<UAirlineRoster>(GetTransientPackage());
			Roster->Bus = &Bus;
			Roster->Ensure(TEXT("A"));
			Bus.BeginWiring();
			Bus.Subscribe<FFlightAirborneEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FFlightAirborneEvent& E) { Roster->OnFlightAirborne(E); });
			Bus.Subscribe<FOfferExpiredEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FOfferExpiredEvent& E) { Roster->OnOfferExpired(E); });
			Bus.Subscribe<FOfferDeclinedEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FOfferDeclinedEvent& E) { Roster->OnOfferDeclined(E); });
			Bus.Subscribe<FDayEndedEvent>(EOpsTier::Reaction, TEXT("Airlines"), [this](const FDayEndedEvent& E) { Roster->OnDayEnded(E); });
			Bus.Subscribe<FAirlineSatisfactionEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FAirlineSatisfactionEvent& E) { Changes.Add(E); });
			Bus.EndWiring();
		}

		double Sat(FName Id = TEXT("A")) const
		{
			const FAirlineStanding* Row = Roster->Find(Id);
			return Row != nullptr ? Row->Satisfaction : -1.0;
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

#endif
