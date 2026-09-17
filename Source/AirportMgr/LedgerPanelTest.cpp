#include "CoreMinimal.h"
#include "BuildActions.h"
#include "LedgerViewModels.h"
#include "Misc/AutomationTest.h"
#include "Model/Ledger.h"
#include "Model/Pricing.h"
#include "Model/SimClock.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FLedgerFixture
	{
		ULedger* Ledger = NewObject<ULedger>();
		UPricing* Pricing = NewObject<UPricing>();
		USimClock* Clock = NewObject<USimClock>();
		ULedgerPanelViewModel* Panel = NewObject<ULedgerPanelViewModel>();

		FLedgerFixture()
		{
			Ledger->Pricing = Pricing;
			Ledger->Clock = Clock;
			Ledger->Open(500000.0);
		}

		bool Refresh() { return Panel->Refresh(*Ledger, *Clock, *Pricing); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerPanelNewestFirstTest,
	"AirportMgr.UI.LedgerPanelNewestFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerPanelNewestFirstTest::RunTest(const FString& Parameters)
{
	FLedgerFixture Fixture;
	Fixture.Ledger->Post(0.0, ELedgerCategory::LandingFee, 1200.0,
		FText::FromString(TEXT("first")));
	Fixture.Ledger->Post(10.0, ELedgerCategory::Placement, -30000.0,
		FText::FromString(TEXT("second")));

	Fixture.Refresh();

	if (!TestEqual(TEXT("both entries became rows"), Fixture.Panel->Rows().Num(), 2))
	{
		return false;
	}

	// NEWEST FIRST. The question this panel answers is "what just happened", and that is the
	// END of an append-only array - a panel that read it forwards would put the answer at the
	// bottom of a scrolling list.
	TestEqual(TEXT("the most recent movement is the top row"),
		Fixture.Panel->Rows()[0]->GetWhat().ToString(), FString(TEXT("second")));
	TestEqual(TEXT("and the older one is beneath it"),
		Fixture.Panel->Rows()[1]->GetWhat().ToString(), FString(TEXT("first")));

	TestTrue(TEXT("a build is marked outgoing, so the row can colour it without parsing a "
		"minus sign back out of formatted money"), Fixture.Panel->Rows()[0]->IsOutgoing());
	TestFalse(TEXT("and a landing fee is not"), Fixture.Panel->Rows()[1]->IsOutgoing());

	TestTrue(TEXT("the amount carries the currency symbol, because UPricing::Format is the one "
		"place money becomes text"),
		Fixture.Panel->Rows()[0]->GetAmount().ToString().Contains(Fixture.Pricing->CurrencySymbol));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerPanelGateTest,
	"AirportMgr.UI.LedgerPanelGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerPanelGateTest::RunTest(const FString& Parameters)
{
	FLedgerFixture Fixture;
	Fixture.Ledger->Post(0.0, ELedgerCategory::Upkeep, -150.0, FText::FromString(TEXT("Upkeep")));

	TestTrue(TEXT("the first refresh builds the rows"), Fixture.Refresh());

	// THE GATE, ASSERTED RATHER THAN TRUSTED. The panel polls every tick while it is open, so
	// without this it would re-derive forty row objects sixty times a second to discover that
	// nothing had happened.
	TestFalse(TEXT("a second refresh with nothing posted does no work at all"), Fixture.Refresh());

	Fixture.Ledger->Post(60.0, ELedgerCategory::LandingFee, 1200.0,
		FText::FromString(TEXT("Landing")));
	TestTrue(TEXT("but money moving rebuilds them"), Fixture.Refresh());

	// ROLL-UP MOVES THE REVISION TOO. It rewrites the array wholesale, so a panel that only
	// watched Post would keep showing rows the ledger had already folded away.
	Fixture.Ledger->MaxDays = 0;
	Fixture.Ledger->RollUp(100000.0);
	TestTrue(TEXT("and so does a roll-up"), Fixture.Refresh());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerPanelCapTest,
	"AirportMgr.UI.LedgerPanelCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerPanelCapTest::RunTest(const FString& Parameters)
{
	FLedgerFixture Fixture;
	Fixture.Panel->MaxRows = 5;

	for (int32 Index = 0; Index < 20; ++Index)
	{
		Fixture.Ledger->Post(Index, ELedgerCategory::Upkeep, -10.0,
			FText::FromString(FString::Printf(TEXT("entry %d"), Index)));
	}
	Fixture.Refresh();

	// CAPPED, because the code-built row path is a VerticalBox with no virtualisation. Twenty
	// entries is nothing; a long game holds thousands, and every one of them would be a widget.
	TestEqual(TEXT("only the most recent MaxRows are shown"), Fixture.Panel->Rows().Num(), 5);
	TestEqual(TEXT("and they are the most recent ones, not the first five"),
		Fixture.Panel->Rows()[0]->GetWhat().ToString(), FString(TEXT("entry 19")));

	// The balance is the WHOLE ledger's, never just the visible rows' - a panel that summed
	// what it could see would disagree with the bar the moment the cap bit.
	TestEqual(TEXT("the balance is the whole ledger's, not the visible rows'"),
		Fixture.Panel->GetBalance().ToString(),
		Fixture.Pricing->Format(Fixture.Ledger->Balance()).ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLedgerToggleIsInTheOneListTest,
	"AirportMgr.Actions.LedgerToggleIsInTheOneList",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FLedgerToggleIsInTheOneListTest::RunTest(const FString& Parameters)
{
	// THROUGH BuildActions, like the fee lever and every tool: the bar, the key bindings and
	// the inspector all read this one table, so a panel toggle added straight to the widget
	// would exist on the bar and nowhere else.
	const FBuildAction* Ledger = FindAction(FName(TEXT("game.ledger")));
	if (!TestNotNull(TEXT("the ledger can be opened from the one action list"), Ledger))
	{
		return false;
	}

	TestEqual(TEXT("it sits in the Game section, beside save and load"),
		Ledger->Section, EActionSection::Game);

	// A KEY IS FINE HERE where the fee lever's is not: opening a panel changes nothing about
	// the airport, so a mis-hit costs a keystroke rather than repricing every future offer.
	TestTrue(TEXT("and it has a key, unlike the fee lever"), Ledger->Key.IsValid());
	TestEqual(TEXT("B, which nothing else claims"), Ledger->Key, EKeys::B);
	return true;
}

#endif
