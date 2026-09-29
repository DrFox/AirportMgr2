#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/Pricing.h"

#if WITH_DEV_AUTOMATION_TESTS

// MONEY IS PUSHED, NOT POLLED (ops alerts spec 2026-09-29 §2): the ledger and the fee lever announce
// what changed, so the bar and the alerts pass stop comparing revisions every frame. World-free.

namespace
{
	struct FMoneyEventsFixture
	{
		FOpsEventBus Bus;
		ULedger* Ledger = nullptr;
		UPricing* Pricing = nullptr;
		TArray<FMoneyPostedEvent> Posted;
		TArray<FBalanceSignChangedEvent> Signs;
		TArray<FLandingFeeChangedEvent> Fees;

		FMoneyEventsFixture()
		{
			Ledger = NewObject<ULedger>(GetTransientPackage());
			Pricing = NewObject<UPricing>(GetTransientPackage());
			Ledger->Bus = &Bus;
			Pricing->Bus = &Bus;
			Bus.BeginWiring();
			Bus.Subscribe<FMoneyPostedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FMoneyPostedEvent& E) { Posted.Add(E); });
			Bus.Subscribe<FBalanceSignChangedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FBalanceSignChangedEvent& E) { Signs.Add(E); });
			Bus.Subscribe<FLandingFeeChangedEvent>(EOpsTier::Presentation, TEXT("test"), [this](const FLandingFeeChangedEvent& E) { Fees.Add(E); });
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoneyFeeTest, "AirportOps.Model.Money.FeeStepIsAnnounced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMoneyFeeTest::RunTest(const FString&)
{
	FMoneyEventsFixture F;
	const double Was = F.Pricing->LandingFeeMultiplier;
	F.Pricing->StepLandingFee(+1);
	F.Bus.Drain();
	if (!TestEqual(TEXT("a fee step is announced"), F.Fees.Num(), 1)) { return false; }
	TestEqual(TEXT("from"), F.Fees[0].Old, Was, 1e-9);
	TestEqual(TEXT("to"), F.Fees[0].New, F.Pricing->LandingFeeMultiplier, 1e-9);
	for (int32 Index = 0; Index < 30; ++Index) { F.Pricing->StepLandingFee(+1); }
	F.Bus.Drain();
	const int32 AtCeiling = F.Fees.Num();
	F.Pricing->StepLandingFee(+1);
	F.Bus.Drain();
	TestEqual(TEXT("a step against the clamp changes nothing and says nothing"), F.Fees.Num(), AtCeiling);
	return true;
}

#endif
