#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/Pricing.h"

#if WITH_DEV_AUTOMATION_TESTS

// MONEY IS ANNOUNCED (ops alerts spec 2026-09-29 §2): the ledger says what moved, so the alerts pass and the
// back-in-credit toast hear it without polling. World-free.

namespace
{
	struct FMoneyEventsFixture
	{
		FOpsEventBus Bus;
		ULedger* Ledger = nullptr;
		TArray<FMoneyPostedEvent> Posted;
		TArray<FBalanceSignChangedEvent> Signs;

		FMoneyEventsFixture()
		{
			Ledger = NewObject<ULedger>(GetTransientPackage());
			Ledger->Bus = &Bus;
			Bus.BeginWiring();
			Bus.Subscribe<FMoneyPostedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FMoneyPostedEvent& E) { Posted.Add(E); });
			Bus.Subscribe<FBalanceSignChangedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FBalanceSignChangedEvent& E) { Signs.Add(E); });
			Bus.EndWiring();
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoneyPostedTest, "AirportOps.Model.Money.PostIsAnnounced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMoneyPostedTest::RunTest(const FString&)
{
	FMoneyEventsFixture F;
	F.Ledger->Open(1000.0);
	const int32 Id = F.Ledger->Post(10.0, ELedgerCategory::LandingFee, 250.0, FText::FromString(TEXT("Landing")));
	F.Bus.Drain();
	if (!TestEqual(TEXT("every post is announced once - the one funnel every fee, charge and refund goes through"), F.Posted.Num(), 1)) { return false; }
	TestEqual(TEXT("naming the entry"), F.Posted[0].EntryId, Id);
	TestEqual(TEXT("its category"), F.Posted[0].Category, ELedgerCategory::LandingFee);
	TestEqual(TEXT("its amount"), F.Posted[0].Amount, 250.0, 1e-9);
	TestEqual(TEXT("and the balance after it"), F.Posted[0].Balance, 1250.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoneySignTest, "AirportOps.Model.Money.CrossingZeroIsAnnouncedOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMoneySignTest::RunTest(const FString&)
{
	FMoneyEventsFixture F;
	F.Ledger->Open(100.0);
	F.Ledger->Post(0.0, ELedgerCategory::Placement, -150.0, FText::FromString(TEXT("a")));
	F.Ledger->Post(0.0, ELedgerCategory::Placement, -10.0, FText::FromString(TEXT("b")));
	F.Ledger->Post(0.0, ELedgerCategory::LandingFee, 200.0, FText::FromString(TEXT("c")));
	F.Ledger->Post(0.0, ELedgerCategory::LandingFee, 5.0, FText::FromString(TEXT("d")));
	F.Bus.Drain();
	if (!TestEqual(TEXT("going into the red and coming out are two events - staying either side is none"), F.Signs.Num(), 2)) { return false; }
	TestTrue(TEXT("first overdrawn - building locks"), F.Signs[0].bOverdrawn);
	TestFalse(TEXT("then back in credit"), F.Signs[1].bOverdrawn);
	return true;
}

#endif
