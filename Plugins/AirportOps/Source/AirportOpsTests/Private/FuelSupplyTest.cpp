#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Model/FacilityPurchases.h"
#include "Model/FuelSupply.h"
#include "Model/Ledger.h"
#include "Model/OpsEventBus.h"
#include "Model/OpsSave.h"
#include "Model/RoadNetwork.h"
#include "Model/ServiceRolePolicy.h"

#if WITH_DEV_AUTOMATION_TESTS

// FUEL SUPPLY (spec 2026-10-02-progression-and-fuel-supply §7): world-free, on a NewObject supply whose capacity is a
// number the test sets - the tanks' arithmetic is UJobBoard's and is tested there (Task 3).

namespace
{
	UFuelSupply* SupplyHolding(double Stock, double Capacity)
	{
		UFuelSupply* Supply = NewObject<UFuelSupply>(GetTransientPackage());
		Supply->CapacityOf = [Capacity]() { return Capacity; };
		Supply->StockLitres = Stock;
		return Supply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyDrawTest, "AirportOps.Model.FuelSupply.DrawGrantsWhatIsHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyDrawTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyHolding(1000.0, 30000.0);
	TestEqual(TEXT("a draw within the stock is granted whole"), Supply->Draw(600.0), 600.0, 1e-9);
	TestEqual(TEXT("and leaves the rest"), Supply->StockLitres, 400.0, 1e-9);
	TestEqual(TEXT("a draw beyond it is granted only what is there"), Supply->Draw(600.0), 400.0, 1e-9);
	TestEqual(TEXT("leaving nothing - never a negative stock"), Supply->StockLitres, 0.0, 1e-9);
	TestEqual(TEXT("and nothing is available"), Supply->Available(), 0.0, 1e-9);
	TestEqual(TEXT("a negative draw grants nothing and adds nothing"), Supply->Draw(-50.0), 0.0, 1e-9);
	TestEqual(TEXT("CONTROL: the stock did not move"), Supply->StockLitres, 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyReceiveTest, "AirportOps.Model.FuelSupply.ReceiveStopsAtCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyReceiveTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyHolding(25000.0, 30000.0);
	TestEqual(TEXT("TAKE-OR-PAY: only what fits is added"), Supply->Receive(10000.0), 5000.0, 1e-9);
	TestEqual(TEXT("and the tanks are full"), Supply->StockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("a full supply has no free space"), Supply->FreeSpace(), 0.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyOverCapacityTest, "AirportOps.Model.FuelSupply.StockAboveCapacityIsKeptNotTopped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyOverCapacityTest::RunTest(const FString&)
{
	// THE TANKS SHRANK UNDER THE STOCK - a depot bulldozed, or the plot repair (review focus 4): the fuel is not destroyed and not refunded - it is held, and nothing
	// more is taken until it is drawn below the new capacity.
	UFuelSupply* Supply = SupplyHolding(50000.0, 30000.0);
	TestEqual(TEXT("free space is never negative"), Supply->FreeSpace(), 0.0, 1e-9);
	TestEqual(TEXT("a delivery adds nothing"), Supply->Receive(5000.0), 0.0, 1e-9);
	TestEqual(TEXT("and the held stock is untouched"), Supply->StockLitres, 50000.0, 1e-9);
	TestEqual(TEXT("it can still all be drawn"), Supply->Draw(50000.0), 50000.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyUnwiredTest, "AirportOps.Model.FuelSupply.UnwiredCapacityIsUnbounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyUnwiredTest::RunTest(const FString&)
{
	// A BARE NewObject (every pre-existing fuel test's board) must behave as fuel did before stock existed only when
	// the board has NO supply at all - see UJobBoard::FuelSupply. A supply with no capacity reader holds anything.
	UFuelSupply* Supply = NewObject<UFuelSupply>(GetTransientPackage());
	TestTrue(TEXT("no capacity reader: capacity unbounded"), Supply->Capacity() > 1e12);
	TestEqual(TEXT("so a delivery is taken whole"), Supply->Receive(1e6), 1e6, 1e-6);
	return true;
}

namespace
{
	UFuelSupply* SupplyWithLedger(double Stock, double Capacity, double Balance)
	{
		UFuelSupply* Supply = SupplyHolding(Stock, Capacity);
		Supply->Figures = FFuelSupplyFigures();
		Supply->Ledger = NewObject<ULedger>(GetTransientPackage());
		Supply->Ledger->Open(Balance);
		return Supply;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSpotTest, "AirportOps.Model.FuelSupply.SpotPaysNowArrivesLater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSpotTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 100000.0);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	TestEqual(TEXT("an order that fits is taken"), Supply->OrderSpot(10000.0, 0.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("paid at once, at the spot price"), Supply->Ledger->Balance(), 100000.0 - 10000.0 * Fig.SpotPricePerLitre, 1e-6);
	TestEqual(TEXT("nothing in the tanks yet"), Supply->StockLitres, 0.0, 1e-9);
	TestEqual(TEXT("an hour early, nothing arrives"), Supply->ReceiveDueSpot(Fig.SpotDelaySeconds - 3600.0), 0);
	TestEqual(TEXT("at the delay, it does"), Supply->ReceiveDueSpot(Fig.SpotDelaySeconds), 1);
	TestEqual(TEXT("and fills the tanks"), Supply->StockLitres, 10000.0, 1e-9);
	TestEqual(TEXT("an order bigger than the room left (counting orders on the way) is refused"),
		Supply->OrderSpot(25000.0, Fig.SpotDelaySeconds), EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("CONTROL: and charged nothing"), Supply->Ledger->Balance(), 100000.0 - 10000.0 * Fig.SpotPricePerLitre, 1e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelContractTest, "AirportOps.Model.FuelSupply.ContractIsTakeOrPay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelContractTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyWithLedger(28000.0, 30000.0, 100000.0);
	const FFuelContractTier Tier = Supply->Figures.ContractTiers[0];
	TestEqual(TEXT("the smallest tier is signed"), Supply->SignContract(0, 0.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("signing costs nothing - each day is paid on delivery"), Supply->Ledger->Balance(), 100000.0, 1e-6);
	Supply->DeliverContractDay(86400.0);
	TestEqual(TEXT("TAKE-OR-PAY: the whole day is charged"), Supply->Ledger->Balance(), 100000.0 - Tier.LitresPerDay * Tier.PricePerLitre, 1e-6);
	TestEqual(TEXT("but only what fitted was added"), Supply->StockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("a day of the term is used"), Supply->Contract.DaysLeft, Supply->Figures.ContractTermDays - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelContractEndsTest, "AirportOps.Model.FuelSupply.ContractEndsAndCancels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelContractEndsTest::RunTest(const FString&)
{
	UFuelSupply* Supply = SupplyWithLedger(0.0, 1e9, 1e9);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	Supply->SignContract(1, 0.0);
	for (int32 Day = 1; Day <= Fig.ContractTermDays; ++Day) { Supply->DeliverContractDay(Day * 86400.0); }
	TestEqual(TEXT("at the end of its term the contract is over"), Supply->Contract.Tier, INDEX_NONE);
	const double Stock = Supply->StockLitres;
	Supply->DeliverContractDay((Fig.ContractTermDays + 1) * 86400.0);
	TestEqual(TEXT("and delivers nothing after"), Supply->StockLitres, Stock, 1e-9);

	Supply->SignContract(0, 0.0);
	Supply->DeliverContractDay(86400.0);
	const double Before = Supply->Ledger->Balance();
	TestEqual(TEXT("a running contract may be cancelled"), Supply->CancelContract(86400.0), EFuelOrderRefusal::None);
	const FFuelContractTier& T0 = Fig.ContractTiers[0];
	TestEqual(TEXT("for CancelFraction of the days left at the daily cost"), Before - Supply->Ledger->Balance(),
		(Fig.ContractTermDays - 1) * T0.LitresPerDay * T0.PricePerLitre * Fig.CancelFraction, 1e-6);
	TestEqual(TEXT("and nothing is left to cancel"), Supply->CancelContract(86400.0), EFuelOrderRefusal::NoContract);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelContractTierTest, "AirportOps.Model.FuelSupply.TierNeedsTheTanksForADay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelContractTierTest::RunTest(const FString&)
{
	// STORAGE GATES THE TIERS (spec §7): a tier whose daily delivery the tanks cannot hold is refused - it would pour
	// fuel away from the first day.
	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 1e9);
	TestEqual(TEXT("20,000 L a day fits 30,000 L of tanks"), Supply->JudgeContract(2), EFuelOrderRefusal::None);
	TestEqual(TEXT("40,000 does not"), Supply->JudgeContract(3), EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("there is no fifth tier"), Supply->JudgeContract(4), EFuelOrderRefusal::UnknownTier);
	Supply->SignContract(0, 0.0);
	TestEqual(TEXT("one contract at a time - a skipped tier is no upgrade"), Supply->JudgeContract(2), EFuelOrderRefusal::AlreadyContracted);
	TestEqual(TEXT("nor is the running tier again"), Supply->JudgeContract(0), EFuelOrderRefusal::AlreadyContracted);
	TestEqual(TEXT("but the next one is an upgrade"), Supply->JudgeContract(1), EFuelOrderRefusal::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplySaveTest, "AirportOps.Model.FuelSupply.ContractAndOrdersSurviveASave",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplySaveTest::RunTest(const FString&)
{
	// A load mid-delay must still deliver what was paid for, and a running contract must still run: both are state, not figures.
	UFuelSupply* Supply = SupplyWithLedger(500.0, 30000.0, 1e6);
	Supply->SignContract(1, 0.0);
	Supply->OrderSpot(4000.0, 100.0);
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	TArray<IOpsPersistent*> Saved = { Supply };
	OpsSave::Capture(Saved, *Net, Snapshot);

	UFuelSupply* Loaded = NewObject<UFuelSupply>(GetTransientPackage());
	TArray<IOpsPersistent*> Into = { Loaded };
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, Into, *Net))) { return false; }
	TestEqual(TEXT("stock"), Loaded->StockLitres, 500.0, 1e-9);
	TestEqual(TEXT("contract tier"), Loaded->Contract.Tier, 1);
	TestEqual(TEXT("contract days left"), Loaded->Contract.DaysLeft, Supply->Figures.ContractTermDays);
	if (!TestEqual(TEXT("the order on the way"), Loaded->SpotOrders.Num(), 1)) { return false; }
	TestEqual(TEXT("with its litres"), Loaded->SpotOrders[0].Litres, 4000.0, 1e-9);
	TestEqual(TEXT("and its due time"), Loaded->SpotOrders[0].DueAt, 100.0 + Supply->Figures.SpotDelaySeconds, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyRestoreClearsTest, "AirportOps.Model.FuelSupply.RestoreClearsTheSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyRestoreClearsTest::RunTest(const FString&)
{
	// THE MIRROR OF THE TEST ABOVE: a save whose fuel is all at its defaults - dry, nothing ordered, no contract - writes
	// none of it, so restoring it into a supply that holds this session's fuel must still leave none of that standing.
	UFuelSupply* Empty = NewObject<UFuelSupply>(GetTransientPackage());
	URoadNetwork* Net = NewObject<URoadNetwork>(GetTransientPackage());
	FOpsSnapshot Snapshot;
	TArray<IOpsPersistent*> Saved = { Empty };
	OpsSave::Capture(Saved, *Net, Snapshot);

	UFuelSupply* Session = SupplyWithLedger(5000.0, 30000.0, 1e6);
	Session->SignContract(0, 0.0);
	Session->OrderSpot(4000.0, 0.0);
	TArray<IOpsPersistent*> Into = { Session };
	if (!TestTrue(TEXT("restore succeeds"), OpsSave::Restore(Snapshot, Into, *Net))) { return false; }
	TestEqual(TEXT("the save's empty tank"), Session->StockLitres, 0.0, 1e-9);
	TestEqual(TEXT("no contract the save did not have"), Session->Contract.Tier, static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("no order the save did not make"), Session->SpotOrders.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSpotRefusalsChargeNothingTest, "AirportOps.Model.FuelSupply.SpotRefusalsChargeNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSpotRefusalsChargeNothingTest::RunTest(const FString&)
{
	// A SPOT ORDER IS PAID AT ONCE, so a refusal must leave the books alone: the player who cannot afford it keeps what
	// they had, and no order is put on the way that nobody paid for.
	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 100.0);
	const int32 Rows = Supply->Ledger->Entries().Num();
	TestEqual(TEXT("an order dearer than the balance is refused CannotAfford"), Supply->OrderSpot(10000.0, 0.0), EFuelOrderRefusal::CannotAfford);
	TestEqual(TEXT("the balance is untouched"), Supply->Ledger->Balance(), 100.0, 1e-9);
	TestEqual(TEXT("no ledger row was posted"), Supply->Ledger->Entries().Num(), Rows);
	TestEqual(TEXT("and nothing is on the way"), Supply->SpotOrders.Num(), 0);
	// NaN COMPARES FALSE BOTH WAYS: `Litres <= 0.0` let it through, and so did `Litres > FreeSpace() - Pending` - an
	// order of NaN litres at NaN cost.
	TestEqual(TEXT("NaN litres are refused NoRoom"), Supply->OrderSpot(std::numeric_limits<double>::quiet_NaN(), 0.0), EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("and charged nothing"), Supply->Ledger->Entries().Num(), Rows);
	TestEqual(TEXT("CONTROL: an affordable order is taken"), Supply->OrderSpot(10.0, 0.0), EFuelOrderRefusal::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSupplyPublishesTest, "AirportOps.Model.FuelSupply.EveryDeliveryIsPublished",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSupplyPublishesTest::RunTest(const FString&)
{
	// THE EVENT IS WHAT RE-OPENS A NoFuelStock JOB (UOpsRuntime::WireBus), so every delivery must publish one - spot and
	// contract alike - and say what it ADDED, not only what was sent: a contract day poured partly away is still a delivery.
	FOpsEventBus Bus;
	TArray<FFuelDeliveredEvent> Seen;
	Bus.BeginWiring();
	Bus.Subscribe<FFuelDeliveredEvent>(EOpsTier::Sim, TEXT("test"), [&Seen](const FFuelDeliveredEvent& E) { Seen.Add(E); });
	Bus.EndWiring();

	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 1e9);
	Supply->Bus = &Bus;
	const FFuelSupplyFigures& Fig = Supply->Figures;
	Supply->OrderSpot(10000.0, 0.0);
	Supply->OrderSpot(5000.0, 0.0);
	TestEqual(TEXT("both spot orders arrive"), Supply->ReceiveDueSpot(Fig.SpotDelaySeconds), 2);
	Bus.Drain();
	if (!TestEqual(TEXT("one event per spot order"), Seen.Num(), 2)) { return false; }
	Seen.Sort([](const FFuelDeliveredEvent& A, const FFuelDeliveredEvent& B) { return A.Litres < B.Litres; });
	TestEqual(TEXT("the 5,000 L order"), Seen[0].Litres, 5000.0, 1e-9);
	TestEqual(TEXT("added whole"), Seen[0].Added, 5000.0, 1e-9);
	TestFalse(TEXT("and not a contract"), Seen[0].bContract);
	TestEqual(TEXT("the 10,000 L order"), Seen[1].Litres, 10000.0, 1e-9);
	TestEqual(TEXT("added whole"), Seen[1].Added, 10000.0, 1e-9);
	TestFalse(TEXT("and not a contract"), Seen[1].bContract);

	Seen.Reset();
	const FFuelContractTier& Tier = Fig.ContractTiers[2];
	if (!TestEqual(TEXT("setup: the 20,000 L tier is signed"), Supply->SignContract(2, 0.0), EFuelOrderRefusal::None)) { return false; }
	Supply->DeliverContractDay(86400.0);
	Bus.Drain();
	if (!TestEqual(TEXT("one event for the contract day"), Seen.Num(), 1)) { return false; }
	TestEqual(TEXT("naming the day's litres"), Seen[0].Litres, Tier.LitresPerDay, 1e-9);
	TestEqual(TEXT("but only what fitted as added (15,000 L stocked of 30,000)"), Seen[0].Added, 15000.0, 1e-9);
	TestTrue(TEXT("CONTROL: less than was sent"), Seen[0].Added < Seen[0].Litres);
	TestTrue(TEXT("and a contract"), Seen[0].bContract);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelQuoteIsTheJudgesTest, "AirportOps.Model.FuelSupply.QuoteIsTheJudges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelQuoteIsTheJudgesTest::RunTest(const FString&)
{
	// THE CARD RENDERS THE QUOTE, the commands run the judges: the two must be one answer, in every state the card can be in -
	// full tanks, broke, no contract, a contract running. Each case asks the quote, then the command, and they must agree.
	UFuelSupply* Supply = SupplyWithLedger(12000.0, 30000.0, 100000.0);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	FFuelQuote Q = Supply->Quote(10000.0);
	TestEqual(TEXT("the stock as held"), Q.StockLitres, 12000.0, 1e-9);
	TestEqual(TEXT("the capacity as wired"), Q.CapacityLitres, 30000.0, 1e-9);
	TestTrue(TEXT("and bounded - a reader is wired"), Q.bBounded);
	TestEqual(TEXT("a spot order that fits is offered"), Q.Spot, EFuelOrderRefusal::None);
	TestEqual(TEXT("at the spot price"), Q.SpotCost, 10000.0 * Fig.SpotPricePerLitre, 1e-6);
	TestEqual(TEXT("no contract: the next tier is the smallest"), Q.NextTier, 0);
	TestEqual(TEXT("its day as the tier prices it"), Q.NextDailyCost, Fig.ContractTiers[0].LitresPerDay * Fig.ContractTiers[0].PricePerLitre, 1e-6);
	TestEqual(TEXT("for the term"), Q.TermDays, Fig.ContractTermDays);
	TestEqual(TEXT("which may be signed"), Q.Sign, EFuelOrderRefusal::None);
	TestEqual(TEXT("and nothing to cancel"), Q.Cancel, EFuelOrderRefusal::NoContract);
	TestEqual(TEXT("CONTROL: the cancel command agrees"), Supply->CancelContract(0.0), EFuelOrderRefusal::NoContract);

	// A CONTRACT RUNNING: one at a time, so Sign is refused whatever tier is next, and Cancel quotes what the command posts.
	TestEqual(TEXT("the smallest tier signs"), Supply->SignContract(Q.NextTier, 0.0), EFuelOrderRefusal::None);
	Q = Supply->Quote(10000.0);
	TestEqual(TEXT("the running tier is quoted"), Q.ContractTier, 0);
	TestEqual(TEXT("with its days"), Q.ContractDaysLeft, Fig.ContractTermDays);
	TestEqual(TEXT("and its day's litres"), Q.ContractLitresPerDay, Fig.ContractTiers[0].LitresPerDay, 1e-9);
	TestEqual(TEXT("a running contract offers the next tier as an upgrade"), Q.NextTier, 1);
	TestEqual(TEXT("which is allowed"), Q.Sign, EFuelOrderRefusal::None);
	TestEqual(TEXT("CONTROL: a tier the quote does not name is refused by the command"), Supply->SignContract(2, 0.0), EFuelOrderRefusal::AlreadyContracted);
	TestEqual(TEXT("it may be cancelled"), Q.Cancel, EFuelOrderRefusal::None);
	const double Before = Supply->Ledger->Balance();
	TestEqual(TEXT("the cancel runs"), Supply->CancelContract(0.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("and charges exactly the quoted figure"), Before - Supply->Ledger->Balance(), Q.CancelCharge, 1e-6);
	TestTrue(TEXT("CONTROL: a figure that is not zero"), Q.CancelCharge > 0.0);

	// FULL TANKS AND AN EMPTY PURSE: each refusal the quote names is the one the command returns.
	Supply->StockLitres = 30000.0;
	TestEqual(TEXT("full tanks: the spot order is quoted NoRoom"), Supply->Quote(10000.0).Spot, EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("CONTROL: and the command agrees"), Supply->OrderSpot(10000.0, 0.0), EFuelOrderRefusal::NoRoom);
	Supply->StockLitres = 0.0;
	Supply->Ledger->Post(0.0, ELedgerCategory::FuelPurchase, -Supply->Ledger->Balance(), FText::FromString(TEXT("test: spent")));
	TestEqual(TEXT("broke: quoted CannotAfford"), Supply->Quote(10000.0).Spot, EFuelOrderRefusal::CannotAfford);
	TestEqual(TEXT("CONTROL: and the command agrees"), Supply->OrderSpot(10000.0, 0.0), EFuelOrderRefusal::CannotAfford);

	// THE UPGRADE'S TWO REFUSALS, quote and command agreeing: past the last tier, and a tier the tanks cannot hold a day of.
	UFuelSupply* Small = SupplyWithLedger(0.0, 15000.0, 1e9);
	Small->SignContract(1, 0.0);   // 10,000 L a day fits 15,000
	TestEqual(TEXT("an upgrade to 20,000 L a day on 15,000 L of tanks is quoted NoRoom"), Small->Quote(10000.0).Sign, EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("CONTROL: and the command agrees"), Small->SignContract(2, 0.0), EFuelOrderRefusal::NoRoom);
	UFuelSupply* Big = SupplyWithLedger(0.0, 1e6, 1e9);
	Big->SignContract(0, 0.0);
	for (int32 Tier = 1; Tier < Big->Figures.ContractTiers.Num(); ++Tier) { Big->SignContract(Tier, 0.0); }
	const FFuelQuote Top = Big->Quote(10000.0);
	TestEqual(TEXT("CONTROL: the largest tier runs"), Top.ContractTier, Big->Figures.ContractTiers.Num() - 1);
	TestEqual(TEXT("past the last tier the upgrade is quoted UnknownTier"), Top.Sign, EFuelOrderRefusal::UnknownTier);
	TestEqual(TEXT("CONTROL: and the command agrees"), Big->SignContract(Top.NextTier, 0.0), EFuelOrderRefusal::UnknownTier);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelUpgradeTest, "AirportOps.Model.FuelSupply.UpgradeReplacesTheContract",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelUpgradeTest::RunTest(const FString&)
{
	// RULED 2026-10-03: while a contract runs, the next tier REPLACES it - its own litres, a fresh term, and no charge for the days
	// the old one had left (a downgrade is a cancel, which is charged).
	UFuelSupply* Supply = SupplyWithLedger(0.0, 30000.0, 100000.0);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	TestEqual(TEXT("tier 0 signs"), Supply->SignContract(0, 0.0), EFuelOrderRefusal::None);
	Supply->DeliverContractDay(86400.0);
	if (!TestEqual(TEXT("setup: a day of the term is used"), Supply->Contract.DaysLeft, Fig.ContractTermDays - 1)) { return false; }
	const double Before = Supply->Ledger->Balance();
	TestEqual(TEXT("the upgrade to tier 1 runs"), Supply->SignContract(1, 90000.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("the contract is tier 1 now"), Supply->Contract.Tier, 1);
	TestEqual(TEXT("with a FRESH term"), Supply->Contract.DaysLeft, Fig.ContractTermDays);
	TestEqual(TEXT("and no charge was posted"), Supply->Ledger->Balance(), Before, 1e-6);
	TestEqual(TEXT("its next day delivers the new tier's litres"), Supply->Quote(0.0).ContractLitresPerDay, Fig.ContractTiers[1].LitresPerDay, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelRefusalSentencesTest, "AirportOps.Model.FuelSupply.EveryRefusalHasASentence",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelRefusalSentencesTest::RunTest(const FString&)
{
	// A GREYED BUTTON SAYS WHY: every refusal but None has words, and no two share them (a reason that reads like another teaches
	// the wrong fix). A MISSING CASE is not this test's to catch - the switch is exhaustive, a build error (C4062 via
	// AIRSIDE_EXHAUSTIVE_SWITCH_BEGIN); this walks the values up to the last one known and catches empty or shared words.
	TestTrue(TEXT("None says nothing"), UFacilityPurchases::FuelOrderRefusalText(EFuelOrderRefusal::None).IsEmpty());
	TSet<FString> Seen;
	for (uint8 Value = 1; Value <= static_cast<uint8>(EFuelOrderRefusal::NoContract); ++Value)
	{
		const FString Words = UFacilityPurchases::FuelOrderRefusalText(static_cast<EFuelOrderRefusal>(Value)).ToString();
		TestFalse(FString::Printf(TEXT("refusal %d has words"), Value), Words.IsEmpty());
		TestFalse(FString::Printf(TEXT("refusal %d's words are its own ('%s')"), Value, *Words), Seen.Contains(Words));
		Seen.Add(Words);
	}
	TestEqual(TEXT("Can't afford reads as the shed's does"), UFacilityPurchases::FuelOrderRefusalText(EFuelOrderRefusal::CannotAfford).ToString(),
		UFacilityPurchases::RefusalText(EPurchaseRefusal::CannotAfford).ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSpotOverflowRefundedTest, "AirportOps.Model.FuelSupply.SpotThatDoesNotFitIsRefunded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSpotOverflowRefundedTest::RunTest(const FString&)
{
	// SPOT IS NOT TAKE-OR-PAY (ruled 2026-10-03): JudgeSpot measures the room at the ORDER, but a contract day - or a depot
	// bulldozed - can fill the tanks before the tanker arrives. The litres that do not fit come back as money, at the spot
	// price, on a positive FuelPurchase line; before the ruling they were added as 0 and the money was simply gone.
	UFuelSupply* Supply = SupplyWithLedger(15000.0, 30000.0, 1e6);
	const FFuelSupplyFigures& Fig = Supply->Figures;
	if (!TestEqual(TEXT("setup: a 10,000 L spot order fits 15,000 L of room"), Supply->OrderSpot(10000.0, 0.0), EFuelOrderRefusal::None)) { return false; }
	if (!TestEqual(TEXT("setup: the 10,000 L tier signs"), Supply->SignContract(1, 0.0), EFuelOrderRefusal::None)) { return false; }
	Supply->DeliverContractDay(3600.0);   // the contract arrives first: 25,000 of 30,000
	if (!TestEqual(TEXT("setup: the contract day filled to 25,000"), Supply->StockLitres, 25000.0, 1e-9)) { return false; }
	const double Before = Supply->Ledger->Balance();
	const int32 Rows = Supply->Ledger->Entries().Num();
	TestEqual(TEXT("the spot order arrives"), Supply->ReceiveDueSpot(Fig.SpotDelaySeconds), 1);
	TestEqual(TEXT("only what fitted was added"), Supply->StockLitres, 30000.0, 1e-9);
	TestEqual(TEXT("the 5,000 L that did not fit are refunded at the spot price"), Supply->Ledger->Balance() - Before,
		Supply->SpotCostOf(10000.0 - 5000.0), 1e-6);
	TestTrue(TEXT("CONTROL: a refund that is not zero"), Supply->SpotCostOf(5000.0) > 0.0);
	if (TestEqual(TEXT("on one ledger line"), Supply->Ledger->Entries().Num(), Rows + 1))
	{
		const FLedgerEntry& Row = Supply->Ledger->Entries().Last();
		TestEqual(TEXT("a fuel purchase line"), Row.Category, ELedgerCategory::FuelPurchase);
		TestTrue(TEXT("money IN"), Row.Amount > 0.0);
	}

	// CONTROL: no contract, so the order fits as it did when it was paid for - added whole, and no refund line.
	UFuelSupply* Clear = SupplyWithLedger(15000.0, 30000.0, 1e6);
	Clear->OrderSpot(10000.0, 0.0);
	const double ClearBefore = Clear->Ledger->Balance();
	const int32 ClearRows = Clear->Ledger->Entries().Num();
	TestEqual(TEXT("CONTROL: the order arrives"), Clear->ReceiveDueSpot(Fig.SpotDelaySeconds), 1);
	TestEqual(TEXT("CONTROL: added whole"), Clear->StockLitres, 25000.0, 1e-9);
	TestEqual(TEXT("CONTROL: the balance did not move"), Clear->Ledger->Balance(), ClearBefore, 1e-9);
	TestEqual(TEXT("CONTROL: and no line was posted"), Clear->Ledger->Entries().Num(), ClearRows);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFuelSpotTopsUpTest, "AirportOps.Model.FuelSupply.SpotTopsUpWhatFits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FFuelSpotTopsUpTest::RunTest(const FString&)
{
	// THE CARD'S ONE BUTTON ASKS FOR 10,000 L; with 25,000 of 30,000 held it used to refuse NoRoom though 5,000 L fitted. It
	// offers what fits now - quote and command agreeing (QuoteIsTheJudges' rule) - and refuses only when nothing does.
	UFuelSupply* Supply = SupplyWithLedger(25000.0, 30000.0, 1e6);
	FFuelQuote Q = Supply->Quote(10000.0);
	TestEqual(TEXT("the quote offers the 5,000 L that fit"), Q.SpotLitres, 5000.0, 1e-9);
	TestEqual(TEXT("at their price"), Q.SpotCost, Supply->SpotCostOf(5000.0), 1e-6);
	TestEqual(TEXT("and takes it"), Q.Spot, EFuelOrderRefusal::None);
	TestEqual(TEXT("CONTROL: the command agrees"), Supply->OrderSpot(Q.SpotLitres, 0.0), EFuelOrderRefusal::None);
	TestEqual(TEXT("and 5,000 L are on the way"), Supply->PendingSpotLitres(), 5000.0, 1e-9);
	// NOTHING FITS NOW (the 5,000 L on the way fill the room): the full size is quoted, refused.
	Q = Supply->Quote(10000.0);
	TestEqual(TEXT("no room: the full order is quoted"), Q.SpotLitres, 10000.0, 1e-9);
	TestEqual(TEXT("and refused NoRoom"), Q.Spot, EFuelOrderRefusal::NoRoom);
	TestEqual(TEXT("CONTROL: the command agrees"), Supply->OrderSpot(Q.SpotLitres, 0.0), EFuelOrderRefusal::NoRoom);
	// ROOM FOR THE WHOLE ORDER: the asked size, not the room.
	UFuelSupply* Empty = SupplyWithLedger(0.0, 30000.0, 1e6);
	TestEqual(TEXT("CONTROL: empty tanks quote the asked 10,000 L"), Empty->Quote(10000.0).SpotLitres, 10000.0, 1e-9);
	return true;
}

#endif
